/*
 * Copyright (c) 2021, Valtteri Koskivuori <vkoskiv@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MappedFile.h>
#include <AK/Vector.h>
#include <LibCompress/Gzip.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/FileStream.h>
#include <LibCore/MimeData.h>
#include <LibGfx/ImageDecoder.h>
#include <stdio.h>
#include <unistd.h>

static Optional<String> description_only(String description, [[maybe_unused]] const String& path)
{
    return description;
}

// FIXME: Ideally Gfx::ImageDecoder could tell us the image type directly.
static Optional<String> image_details(const String& description, const String& path)
{
    auto file_or_error = MappedFile::map(path);
    if (file_or_error.is_error())
        return {};

    auto& mapped_file = *file_or_error.value();
    auto image_decoder = Gfx::ImageDecoder::create((const u8*)mapped_file.data(), mapped_file.size());

    if (!image_decoder->is_valid())
        return {};

    return String::formatted("{}, {} x {}", description, image_decoder->width(), image_decoder->height());
}

static Optional<String> gzip_details(String description, const String& path)
{
    auto file_or_error = MappedFile::map(path);
    if (file_or_error.is_error())
        return {};

    auto& mapped_file = *file_or_error.value();
    if (!Compress::GzipDecompressor::is_likely_compressed(mapped_file.bytes()))
        return {};

    auto gzip_details = Compress::GzipDecompressor::describe_header(mapped_file.bytes());
    if (!gzip_details.has_value())
        return {};

    return String::formatted("{}, {}", description, gzip_details.value());
}

#define ENUMERATE_MIME_TYPE_DESCRIPTIONS                                                               \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("application/javascript", "JavaScript source", description_only) \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("application/json", "JSON data", description_only)               \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("extra/gzip", "gzip compressed data", gzip_details)              \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/bmp", "BMP image data", image_details)                    \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/gif", "GIF image data", image_details)                    \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/jpeg", "JPEG image data", image_details)                  \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/png", "PNG image data", image_details)                    \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/x-portable-bitmap", "PBM image data", image_details)      \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/x-portable-graymap", "PGM image data", image_details)     \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("image/x-portable-pixmap", "PPM image data", image_details)      \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("text/markdown", "Markdown document", description_only)          \
    __ENUMERATE_MIME_TYPE_DESCRIPTION("text/x-shellscript", "POSIX shell script text executable", description_only)

static Optional<String> get_description_from_mime_type(const String& mime, const String& path)
{
#define __ENUMERATE_MIME_TYPE_DESCRIPTION(mime_type, description, details) \
    if (String(mime_type) == mime)                                         \
        return details(String(description), path);
    ENUMERATE_MIME_TYPE_DESCRIPTIONS;
#undef __ENUMERATE_MIME_TYPE_DESCRIPTION
    return {};
}

int main(int argc, char** argv)
{
    if (pledge("stdio rpath", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    Vector<const char*> paths;
    bool flag_mime_only = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("Determine type of files");
    args_parser.add_option(flag_mime_only, "Only print mime type", "mime-type", 'I');
    args_parser.add_positional_argument(paths, "Files to identify", "files", Core::ArgsParser::Required::Yes);
    args_parser.parse(argc, argv);

    bool all_ok = true;

    for (auto path : paths) {
        auto file = Core::File::construct(path);
        if (!file->open(Core::OpenMode::ReadOnly)) {
            perror(path);
            all_ok = false;
            continue;
        }
        auto bytes = file->read(25);
        auto file_name_guess = Core::guess_mime_type_based_on_filename(path);
        auto mime_type = Core::guess_mime_type_based_on_sniffed_bytes(bytes.bytes()).value_or(file_name_guess);
        auto human_readable_description = get_description_from_mime_type(mime_type, String(path)).value_or(mime_type);
        outln("{}: {}", path, flag_mime_only ? mime_type : human_readable_description);
    }

    return all_ok ? 0 : 1;
}
