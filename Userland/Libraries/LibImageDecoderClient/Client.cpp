/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibImageDecoderClient/Client.h>

namespace ImageDecoderClient {

Client::Client()
    : IPC::ServerConnection<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint>(*this, "/tmp/portal/image")
{
    handshake();
}

void Client::die()
{
    if (on_death)
        on_death();
}

void Client::handshake()
{
    greet();
}

void Client::dummy()
{
}

Optional<DecodedImage> Client::decode_image(const ByteBuffer& encoded_data)
{
    if (encoded_data.is_empty())
        return {};

    auto encoded_buffer = Core::AnonymousBuffer::create_with_size(encoded_data.size());
    if (!encoded_buffer.is_valid()) {
        dbgln("Could not allocate encoded buffer");
        return {};
    }

    memcpy(encoded_buffer.data<void>(), encoded_data.data(), encoded_data.size());
    auto response_or_error = try_decode_image(move(encoded_buffer));

    if (response_or_error.is_error()) {
        dbgln("ImageDecoder died heroically");
        return {};
    }

    auto& response = response_or_error.value();

    if (response.bitmaps().is_empty())
        return {};

    DecodedImage image;
    image.is_animated = response.is_animated();
    image.loop_count = response.loop_count();
    image.frames.resize(response.bitmaps().size());
    for (size_t i = 0; i < image.frames.size(); ++i) {
        auto& frame = image.frames[i];
        frame.bitmap = response.bitmaps()[i].bitmap();
        frame.duration = response.durations()[i];
    }
    return image;
}

}
