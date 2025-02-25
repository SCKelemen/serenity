/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Debug.h>
#include <AK/Random.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibWebSocket/Impl/TCPWebSocketConnectionImpl.h>
#include <LibWebSocket/Impl/TLSv12WebSocketConnectionImpl.h>
#include <LibWebSocket/WebSocket.h>
#include <stdio.h>
#include <unistd.h>

namespace WebSocket {

// Note : The websocket protocol is defined by RFC 6455, found at https://tools.ietf.org/html/rfc6455
// In this file, section numbers will refer to the RFC 6455

NonnullRefPtr<WebSocket> WebSocket::create(ConnectionInfo connection)
{
    return adopt_ref(*new WebSocket(connection));
}

WebSocket::WebSocket(ConnectionInfo connection)
    : m_connection(connection)
{
}

WebSocket::~WebSocket()
{
}

void WebSocket::start()
{
    VERIFY(m_state == WebSocket::InternalState::NotStarted);
    VERIFY(!m_impl);
    if (m_connection.is_secure())
        m_impl = TLSv12WebSocketConnectionImpl::construct();
    else
        m_impl = TCPWebSocketConnectionImpl::construct();

    m_impl->on_connection_error = [this] {
        dbgln("WebSocket: Connection error (underlying socket)");
        fatal_error(WebSocket::Error::CouldNotEstablishConnection);
    };
    m_impl->on_connected = [this] {
        if (m_state != WebSocket::InternalState::EstablishingProtocolConnection)
            return;
        m_state = WebSocket::InternalState::SendingClientHandshake;
        send_client_handshake();
        drain_read();
    };
    m_impl->on_ready_to_read = [this] {
        drain_read();
    };
    m_state = WebSocket::InternalState::EstablishingProtocolConnection;
    m_impl->connect(m_connection);
}

ReadyState WebSocket::ready_state()
{
    switch (m_state) {
    case WebSocket::InternalState::NotStarted:
    case WebSocket::InternalState::EstablishingProtocolConnection:
    case WebSocket::InternalState::SendingClientHandshake:
    case WebSocket::InternalState::WaitingForServerHandshake:
        return ReadyState::Connecting;
    case WebSocket::InternalState::Open:
        return ReadyState::Open;
    case WebSocket::InternalState::Closing:
        return ReadyState::Closing;
    case WebSocket::InternalState::Closed:
    case WebSocket::InternalState::Errored:
        return ReadyState::Closed;
    default:
        VERIFY_NOT_REACHED();
        return ReadyState::Closed;
    }
}

void WebSocket::send(Message message)
{
    // Calling send on a socket that is not opened is not allowed
    VERIFY(m_state == WebSocket::InternalState::Open);
    VERIFY(m_impl);
    if (message.is_text())
        send_frame(WebSocket::OpCode::Text, message.data(), true);
    else
        send_frame(WebSocket::OpCode::Binary, message.data(), true);
}

void WebSocket::close(u16 code, String message)
{
    // Calling close on a socket that is not opened is not allowed
    VERIFY(m_state == WebSocket::InternalState::Open);
    VERIFY(m_impl);
    auto message_bytes = message.bytes();
    auto close_payload = ByteBuffer::create_uninitialized(message_bytes.size() + 2);
    close_payload.overwrite(0, (u8*)&code, 2);
    close_payload.overwrite(2, message_bytes.data(), message_bytes.size());
    send_frame(WebSocket::OpCode::ConnectionClose, close_payload, true);
}

void WebSocket::drain_read()
{
    if (m_impl->eof()) {
        // The connection got closed by the server
        m_state = WebSocket::InternalState::Closed;
        notify_close(m_last_close_code, m_last_close_message, true);
        discard_connection();
        return;
    }

    while (m_impl->can_read()) {
        if (m_state == WebSocket::InternalState::WaitingForServerHandshake) {
            read_server_handshake();
            return;
        }
        if (m_state == WebSocket::InternalState::Open) {
            read_frame();
            return;
        }
        if (m_state == WebSocket::InternalState::Closing) {
            read_frame();
            return;
        }
    }
}

// The client handshake message is defined in the second list of section 4.1
void WebSocket::send_client_handshake()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::SendingClientHandshake);
    StringBuilder builder;

    // 2. and 3. GET /resource name/ HTTP 1.1
    builder.appendff("GET {} HTTP/1.1\r\n", m_connection.resource_name());

    // 4. Host
    auto url = m_connection.url();
    builder.appendff("Host: {}", url.host());
    if (!m_connection.is_secure() && url.port() != 80)
        builder.appendff(":{}", url.port());
    else if (m_connection.is_secure() && url.port() != 443)
        builder.appendff(":{}", url.port());
    builder.append("\r\n");

    // 5. and 6. Connection Upgrade
    builder.append("Upgrade: websocket\r\n");
    builder.append("Connection: Upgrade\r\n");

    // 7. 16-byte nonce encoded as Base64
    u8 nonce_data[16];
    fill_with_random(nonce_data, 16);
    m_websocket_key = encode_base64(ReadonlyBytes(nonce_data, 16));
    builder.appendff("Sec-WebSocket-Key: {}\r\n", m_websocket_key);

    // 8. Origin (optional field)
    if (!m_connection.origin().is_empty()) {
        builder.appendff("Origin: {}\r\n", m_connection.origin());
    }

    // 9. Websocket version
    builder.append("Sec-WebSocket-Version: 13\r\n");

    // 10. Websocket protocol (optional field)
    if (!m_connection.protocols().is_empty()) {
        builder.append("Sec-WebSocket-Protocol: ");
        builder.join(",", m_connection.protocols());
        builder.append("\r\n");
    }

    // 11. Websocket extensions (optional field)
    if (!m_connection.extensions().is_empty()) {
        builder.append("Sec-WebSocket-Extensions: ");
        builder.join(",", m_connection.extensions());
        builder.append("\r\n");
    }

    // 12. Additional headers
    for (auto& header : m_connection.headers()) {
        builder.appendff("{}: {}\r\n", header.name, header.value);
    }

    builder.append("\r\n");

    m_state = WebSocket::InternalState::WaitingForServerHandshake;
    auto success = m_impl->send(builder.to_string().bytes());
    VERIFY(success);
}

// The server handshake message is defined in the third list of section 4.1
void WebSocket::read_server_handshake()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::WaitingForServerHandshake);
    // Read the server handshake
    if (!m_impl->can_read_line())
        return;

    if (!m_has_read_server_handshake_first_line) {
        auto header = m_impl->read_line(PAGE_SIZE);
        auto parts = header.split(' ');
        if (parts.size() < 2) {
            dbgln("WebSocket: Server HTTP Handshake contained HTTP header was malformed");
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            discard_connection();
            return;
        }
        if (parts[0] != "HTTP/1.1") {
            dbgln("WebSocket: Server HTTP Handshake contained HTTP header {} which isn't supported", parts[0]);
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            discard_connection();
            return;
        }
        if (parts[1] != "101") {
            // 1. If the status code is not 101, handle as per HTTP procedures.
            // FIXME : This could be a redirect or a 401 authentication request, which we do not handle.
            dbgln("WebSocket: Server HTTP Handshake return status {} which isn't supported", parts[1]);
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            return;
        }
        m_has_read_server_handshake_first_line = true;
    }

    // Read the rest of the reply until we find an empty line
    while (m_impl->can_read_line()) {
        auto line = m_impl->read_line(PAGE_SIZE);
        if (line.is_whitespace()) {
            // We're done with the HTTP headers.
            // Fail the connection if we're missing any of the following:
            if (!m_has_read_server_handshake_upgrade) {
                // 2. |Upgrade| should be present
                dbgln("WebSocket: Server HTTP Handshake didn't contain an |Upgrade| header");
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }
            if (!m_has_read_server_handshake_connection) {
                // 2. |Connection| should be present
                dbgln("WebSocket: Server HTTP Handshake didn't contain a |Connection| header");
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }
            if (!m_has_read_server_handshake_accept) {
                // 2. |Sec-WebSocket-Accept| should be present
                dbgln("WebSocket: Server HTTP Handshake didn't contain a |Sec-WebSocket-Accept| header");
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }

            m_state = WebSocket::InternalState::Open;
            notify_open();
            return;
        }

        auto parts = line.split(':');
        if (parts.size() < 2) {
            // The header field is not valid
            dbgln("WebSocket: Got invalid header line {} in the Server HTTP handshake", line);
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            return;
        }

        auto header_name = parts[0];

        if (header_name.equals_ignoring_case("Upgrade")) {
            // 2. |Upgrade| should be case-insensitive "websocket"
            if (!parts[1].trim_whitespace().equals_ignoring_case("websocket")) {
                dbgln("WebSocket: Server HTTP Handshake Header |Upgrade| should be 'websocket', got '{}'. Failing connection.", parts[1]);
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }

            m_has_read_server_handshake_upgrade = true;
            continue;
        }

        if (header_name.equals_ignoring_case("Connection")) {
            // 3. |Connection| should be case-insensitive "Upgrade"
            if (!parts[1].trim_whitespace().equals_ignoring_case("Upgrade")) {
                dbgln("WebSocket: Server HTTP Handshake Header |Connection| should be 'Upgrade', got '{}'. Failing connection.", parts[1]);
                return;
            }

            m_has_read_server_handshake_connection = true;
            continue;
        }

        if (header_name.equals_ignoring_case("Sec-WebSocket-Accept")) {
            // 4. |Sec-WebSocket-Accept| should be base64(SHA1(|Sec-WebSocket-Key| + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
            auto expected_content = String::formatted("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", m_websocket_key);

            Crypto::Hash::Manager hash;
            hash.initialize(Crypto::Hash::HashKind::SHA1);
            hash.update(expected_content);
            auto expected_sha1 = hash.digest();
            auto expected_sha1_string = encode_base64(ReadonlyBytes(expected_sha1.immutable_data(), expected_sha1.data_length()));
            if (!parts[1].trim_whitespace().equals_ignoring_case(expected_sha1_string)) {
                dbgln("WebSocket: Server HTTP Handshake Header |Sec-Websocket-Accept| should be '{}', got '{}'. Failing connection.", expected_sha1_string, parts[1]);
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }

            m_has_read_server_handshake_accept = true;
            continue;
        }

        if (header_name.equals_ignoring_case("Sec-WebSocket-Extensions")) {
            // 5. |Sec-WebSocket-Extensions| should not contain an extension that doesn't appear in m_connection->extensions()
            auto server_extensions = parts[1].split(',');
            for (auto extension : server_extensions) {
                auto trimmed_extension = extension.trim_whitespace();
                bool found_extension = false;
                for (auto supported_extension : m_connection.extensions()) {
                    if (trimmed_extension.equals_ignoring_case(supported_extension)) {
                        found_extension = true;
                    }
                }
                if (!found_extension) {
                    dbgln("WebSocket: Server HTTP Handshake Header |Sec-WebSocket-Extensions| contains '{}', which is not supported by the client. Failing connection.", trimmed_extension);
                    fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                    return;
                }
            }
            continue;
        }

        if (header_name.equals_ignoring_case("Sec-WebSocket-Protocol")) {
            // 6. |Sec-WebSocket-Protocol| should not contain an extension that doesn't appear in m_connection->protocols()
            auto server_protocols = parts[1].split(',');
            for (auto protocol : server_protocols) {
                auto trimmed_protocol = protocol.trim_whitespace();
                bool found_protocol = false;
                for (auto supported_protocol : m_connection.protocols()) {
                    if (trimmed_protocol.equals_ignoring_case(supported_protocol)) {
                        found_protocol = true;
                    }
                }
                if (!found_protocol) {
                    dbgln("WebSocket: Server HTTP Handshake Header |Sec-WebSocket-Protocol| contains '{}', which is not supported by the client. Failing connection.", trimmed_protocol);
                    fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                    return;
                }
            }
            continue;
        }
    }

    // If needed, we will keep reading the header on the next drain_read call
}

void WebSocket::read_frame()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::Open || m_state == WebSocket::InternalState::Closing);

    auto head_bytes = m_impl->read(2);
    if (head_bytes.size() == 0) {
        // The connection got closed.
        m_state = WebSocket::InternalState::Closed;
        notify_close(m_last_close_code, m_last_close_message, true);
        discard_connection();
        return;
    }
    VERIFY(head_bytes.size() == 2);

    bool is_final_frame = head_bytes[0] & 0x80;
    if (!is_final_frame) {
        // FIXME: Support fragmented frames
        TODO();
    }

    auto op_code = (WebSocket::OpCode)(head_bytes[0] & 0x0f);
    bool is_masked = head_bytes[1] & 0x80;

    // Parse the payload length.
    size_t payload_length;
    auto payload_length_bits = head_bytes[1] & 0x7f;
    if (payload_length_bits == 127) {
        // A code of 127 means that the next 8 bytes contains the payload length
        auto actual_bytes = m_impl->read(8);
        VERIFY(actual_bytes.size() == 8);
        u64 full_payload_length = (u64)((u64)(actual_bytes[0] & 0xff) << 56)
            | (u64)((u64)(actual_bytes[1] & 0xff) << 48)
            | (u64)((u64)(actual_bytes[2] & 0xff) << 40)
            | (u64)((u64)(actual_bytes[3] & 0xff) << 32)
            | (u64)((u64)(actual_bytes[4] & 0xff) << 24)
            | (u64)((u64)(actual_bytes[5] & 0xff) << 16)
            | (u64)((u64)(actual_bytes[6] & 0xff) << 8)
            | (u64)((u64)(actual_bytes[7] & 0xff) << 0);
        VERIFY(full_payload_length <= NumericLimits<size_t>::max());
        payload_length = (size_t)full_payload_length;
    } else if (payload_length_bits == 126) {
        // A code of 126 means that the next 2 bytes contains the payload length
        auto actual_bytes = m_impl->read(2);
        VERIFY(actual_bytes.size() == 2);
        payload_length = (size_t)((size_t)(actual_bytes[0] & 0xff) << 8)
            | (size_t)((size_t)(actual_bytes[1] & 0xff) << 0);
    } else {
        payload_length = (size_t)payload_length_bits;
    }

    // Parse the mask, if it exists.
    // Note : this is technically non-conformant with Section 5.1 :
    // > A server MUST NOT mask any frames that it sends to the client.
    // > A client MUST close a connection if it detects a masked frame.
    // > (These rules might be relaxed in a future specification.)
    // But because it doesn't cost much, we can support receiving masked frames anyways.
    u8 masking_key[4];
    if (is_masked) {
        auto masking_key_data = m_impl->read(4);
        VERIFY(masking_key_data.size() == 4);
        masking_key[0] = masking_key_data[0];
        masking_key[1] = masking_key_data[1];
        masking_key[2] = masking_key_data[2];
        masking_key[3] = masking_key_data[3];
    }

    auto payload = ByteBuffer::create_uninitialized(payload_length);
    u64 read_length = 0;
    while (read_length < payload_length) {
        auto payload_part = m_impl->read(payload_length - read_length);
        if (payload_part.size() == 0) {
            // We got disconnected, somehow.
            dbgln("Websocket: Server disconnected while sending payload ({} bytes read out of {})", read_length, payload_length);
            fatal_error(WebSocket::Error::ServerClosedSocket);
            return;
        }
        // We read at most "actual_length - read" bytes, so this is safe to do.
        payload.overwrite(read_length, payload_part.data(), payload_part.size());
        read_length -= payload_part.size();
    }

    if (is_masked) {
        // Unmask the payload
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = payload[i] ^ (masking_key[i % 4]);
        }
    }

    if (op_code == WebSocket::OpCode::ConnectionClose) {
        if (payload.size() > 1) {
            m_last_close_code = (((u16)(payload[0] & 0xff) << 8) | ((u16)(payload[1] & 0xff)));
            m_last_close_message = String(ReadonlyBytes(payload.offset_pointer(2), payload.size() - 2));
        }
        m_state = WebSocket::InternalState::Closing;
        return;
    }
    if (op_code == WebSocket::OpCode::Ping) {
        // Immediately send a pong frame as a reply, with the given payload.
        send_frame(WebSocket::OpCode::Pong, payload, true);
        return;
    }
    if (op_code == WebSocket::OpCode::Pong) {
        // We can safely ignore the pong
        return;
    }
    if (op_code == WebSocket::OpCode::Continuation) {
        // FIXME: Support fragmented frames
        TODO();
        return;
    }
    if (op_code == WebSocket::OpCode::Text) {
        notify_message(Message(payload, true));
        return;
    }
    if (op_code == WebSocket::OpCode::Binary) {
        notify_message(Message(payload, false));
        return;
    }
    dbgln("Websocket: Found unknown opcode {}", (u8)op_code);
}

void WebSocket::send_frame(WebSocket::OpCode op_code, ReadonlyBytes payload, bool is_final)
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::Open);
    u8 frame_head[1] = { (u8)((is_final ? 0x80 : 0x00) | ((u8)(op_code)&0xf)) };
    m_impl->send(ReadonlyBytes(frame_head, 1));
    // Section 5.1 : a client MUST mask all frames that it sends to the server
    bool has_mask = true;
    if (payload.size() > NumericLimits<u64>::max()) {
        // FIXME: We can technically stream this via non-final packets.
        TODO();
    } else if (payload.size() > NumericLimits<u16>::max()) {
        // Send (the 'mask' flag + 127) + the 8-byte payload length
        if constexpr (sizeof(size_t) >= 64) {
            u8 payload_length[9] = {
                (u8)((has_mask ? 0x80 : 0x00) | 127),
                (u8)((payload.size() >> 56) & 0xff),
                (u8)((payload.size() >> 48) & 0xff),
                (u8)((payload.size() >> 40) & 0xff),
                (u8)((payload.size() >> 32) & 0xff),
                (u8)((payload.size() >> 24) & 0xff),
                (u8)((payload.size() >> 16) & 0xff),
                (u8)((payload.size() >> 8) & 0xff),
                (u8)((payload.size() >> 0) & 0xff),
            };
            m_impl->send(ReadonlyBytes(payload_length, 9));
        } else {
            u8 payload_length[9] = {
                (u8)((has_mask ? 0x80 : 0x00) | 127),
                0,
                0,
                0,
                0,
                (u8)((payload.size() >> 24) & 0xff),
                (u8)((payload.size() >> 16) & 0xff),
                (u8)((payload.size() >> 8) & 0xff),
                (u8)((payload.size() >> 0) & 0xff),
            };
            m_impl->send(ReadonlyBytes(payload_length, 9));
        }
    } else if (payload.size() >= 126) {
        // Send (the 'mask' flag + 126) + the 2-byte payload length
        u8 payload_length[3] = {
            (u8)((has_mask ? 0x80 : 0x00) | 126),
            (u8)((payload.size() >> 8) & 0xff),
            (u8)((payload.size() >> 0) & 0xff),
        };
        m_impl->send(ReadonlyBytes(payload_length, 3));
    } else {
        // Send the mask flag + the payload in a single byte
        u8 payload_length[1] = {
            (u8)((has_mask ? 0x80 : 0x00) | (u8)(payload.size() & 0x7f)),
        };
        m_impl->send(ReadonlyBytes(payload_length, 1));
    }
    if (has_mask) {
        // Section 10.3 :
        // > Clients MUST choose a new masking key for each frame, using an algorithm
        // > that cannot be predicted by end applications that provide data
        u8 masking_key[4];
        fill_with_random(masking_key, 4);
        m_impl->send(ReadonlyBytes(masking_key, 4));
        // Mask the payload
        auto masked_payload = ByteBuffer::create_uninitialized(payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            masked_payload[i] = payload[i] ^ (masking_key[i % 4]);
        }
        m_impl->send(masked_payload);
    } else {
        m_impl->send(payload);
    }
}

void WebSocket::fatal_error(WebSocket::Error error)
{
    m_state = WebSocket::InternalState::Errored;
    notify_error(error);
    discard_connection();
}

void WebSocket::discard_connection()
{
    VERIFY(m_impl);
    m_impl->discard_connection();
    m_impl->on_connection_error = nullptr;
    m_impl->on_connected = nullptr;
    m_impl->on_ready_to_read = nullptr;
    m_impl = nullptr;
}

void WebSocket::notify_open()
{
    if (!on_open)
        return;
    on_open();
}

void WebSocket::notify_close(u16 code, String reason, bool was_clean)
{
    if (!on_close)
        return;
    on_close(code, reason, was_clean);
}

void WebSocket::notify_error(WebSocket::Error error)
{
    if (!on_error)
        return;
    on_error(error);
}

void WebSocket::notify_message(Message message)
{
    if (!on_message)
        return;
    on_message(message);
}

}
