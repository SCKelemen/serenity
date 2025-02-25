/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AK/ByteBuffer.h"
#include <AK/Checked.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <LibCore/File.h>
#include <LibTTF/Cmap.h>
#include <LibTTF/Font.h>
#include <LibTTF/Glyf.h>
#include <LibTTF/Tables.h>
#include <LibTextCodec/Decoder.h>
#include <math.h>

namespace TTF {

u16 be_u16(const u8* ptr);
u32 be_u32(const u8* ptr);
i16 be_i16(const u8* ptr);
float be_fword(const u8* ptr);
u32 tag_from_str(const char* str);

u16 be_u16(const u8* ptr)
{
    return (((u16)ptr[0]) << 8) | ((u16)ptr[1]);
}

u32 be_u32(const u8* ptr)
{
    return (((u32)ptr[0]) << 24) | (((u32)ptr[1]) << 16) | (((u32)ptr[2]) << 8) | ((u32)ptr[3]);
}

i16 be_i16(const u8* ptr)
{
    return (((i16)ptr[0]) << 8) | ((i16)ptr[1]);
}

float be_fword(const u8* ptr)
{
    return (float)be_i16(ptr) / (float)(1 << 14);
}

u32 tag_from_str(const char* str)
{
    return be_u32((const u8*)str);
}

Optional<Head> Head::from_slice(const ReadonlyBytes& slice)
{
    if (slice.size() < (size_t)Sizes::Table) {
        return {};
    }
    return Head(slice);
}

u16 Head::units_per_em() const
{
    return be_u16(m_slice.offset_pointer((u32)Offsets::UnitsPerEM));
}

i16 Head::xmin() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::XMin));
}

i16 Head::ymin() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::YMin));
}

i16 Head::xmax() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::XMax));
}

i16 Head::ymax() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::YMax));
}

u16 Head::lowest_recommended_ppem() const
{
    return be_u16(m_slice.offset_pointer((u32)Offsets::LowestRecPPEM));
}

IndexToLocFormat Head::index_to_loc_format() const
{
    i16 raw = be_i16(m_slice.offset_pointer((u32)Offsets::IndexToLocFormat));
    switch (raw) {
    case 0:
        return IndexToLocFormat::Offset16;
    case 1:
        return IndexToLocFormat::Offset32;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<Hhea> Hhea::from_slice(const ReadonlyBytes& slice)
{
    if (slice.size() < (size_t)Sizes::Table) {
        return {};
    }
    return Hhea(slice);
}

i16 Hhea::ascender() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::Ascender));
}

i16 Hhea::descender() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::Descender));
}

i16 Hhea::line_gap() const
{
    return be_i16(m_slice.offset_pointer((u32)Offsets::LineGap));
}

u16 Hhea::advance_width_max() const
{
    return be_u16(m_slice.offset_pointer((u32)Offsets::AdvanceWidthMax));
}

u16 Hhea::number_of_h_metrics() const
{
    return be_u16(m_slice.offset_pointer((u32)Offsets::NumberOfHMetrics));
}

Optional<Maxp> Maxp::from_slice(const ReadonlyBytes& slice)
{
    if (slice.size() < (size_t)Sizes::TableV0p5) {
        return {};
    }
    return Maxp(slice);
}

u16 Maxp::num_glyphs() const
{
    return be_u16(m_slice.offset_pointer((u32)Offsets::NumGlyphs));
}

Optional<Hmtx> Hmtx::from_slice(const ReadonlyBytes& slice, u32 num_glyphs, u32 number_of_h_metrics)
{
    if (slice.size() < number_of_h_metrics * (u32)Sizes::LongHorMetric + (num_glyphs - number_of_h_metrics) * (u32)Sizes::LeftSideBearing) {
        return {};
    }
    return Hmtx(slice, num_glyphs, number_of_h_metrics);
}

Optional<Name> Name::from_slice(const ReadonlyBytes& slice)
{
    return Name(slice);
}

String Name::string_for_id(NameId id) const
{
    auto num_entries = be_u16(m_slice.offset_pointer(2));
    auto string_offset = be_u16(m_slice.offset_pointer(4));

    for (int i = 0; i < num_entries; ++i) {
        auto this_id = be_u16(m_slice.offset_pointer(6 + i * 12 + 6));
        if (this_id != (u16)id)
            continue;

        auto platform = be_u16(m_slice.offset_pointer(6 + i * 12 + 0));
        auto length = be_u16(m_slice.offset_pointer(6 + i * 12 + 8));
        auto offset = be_u16(m_slice.offset_pointer(6 + i * 12 + 10));

        if (platform == (u16)Platform::Windows) {
            static auto& decoder = *TextCodec::decoder_for("utf-16be");
            return decoder.to_utf8(StringView { (const char*)m_slice.offset_pointer(string_offset + offset), length });
        }

        return String((const char*)m_slice.offset_pointer(string_offset + offset), length);
    }

    return String::empty();
}

GlyphHorizontalMetrics Hmtx::get_glyph_horizontal_metrics(u32 glyph_id) const
{
    VERIFY(glyph_id < m_num_glyphs);
    if (glyph_id < m_number_of_h_metrics) {
        auto offset = glyph_id * (u32)Sizes::LongHorMetric;
        u16 advance_width = be_u16(m_slice.offset_pointer(offset));
        i16 left_side_bearing = be_i16(m_slice.offset_pointer(offset + 2));
        return GlyphHorizontalMetrics {
            .advance_width = advance_width,
            .left_side_bearing = left_side_bearing,
        };
    }
    auto offset = m_number_of_h_metrics * (u32)Sizes::LongHorMetric + (glyph_id - m_number_of_h_metrics) * (u32)Sizes::LeftSideBearing;
    u16 advance_width = be_u16(m_slice.offset_pointer((m_number_of_h_metrics - 1) * (u32)Sizes::LongHorMetric));
    i16 left_side_bearing = be_i16(m_slice.offset_pointer(offset));
    return GlyphHorizontalMetrics {
        .advance_width = advance_width,
        .left_side_bearing = left_side_bearing,
    };
}

RefPtr<Font> Font::load_from_file(String path, unsigned index)
{
    auto file_or_error = Core::File::open(move(path), Core::OpenMode::ReadOnly);
    if (file_or_error.is_error()) {
        dbgln("Could not open file: {}", file_or_error.error());
        return nullptr;
    }
    auto file = file_or_error.value();
    if (!file->open(Core::OpenMode::ReadOnly)) {
        dbgln("Could not open file");
        return nullptr;
    }
    auto buffer = file->read_all();
    return load_from_memory(buffer, index);
}

RefPtr<Font> Font::load_from_memory(ByteBuffer& buffer, unsigned index)
{
    if (buffer.size() < 4) {
        dbgln("Font file too small");
        return nullptr;
    }
    u32 tag = be_u32(buffer.data());
    if (tag == tag_from_str("ttcf")) {
        // It's a font collection
        if (buffer.size() < (u32)Sizes::TTCHeaderV1 + sizeof(u32) * (index + 1)) {
            dbgln("Font file too small");
            return nullptr;
        }
        u32 offset = be_u32(buffer.offset_pointer((u32)Sizes::TTCHeaderV1 + sizeof(u32) * index));
        return load_from_offset(move(buffer), offset);
    }
    if (tag == tag_from_str("OTTO")) {
        dbgln("CFF fonts not supported yet");
        return nullptr;
    }
    if (tag != 0x00010000) {
        dbgln("Not a valid font");
        return nullptr;
    }
    return load_from_offset(move(buffer), 0);
}

// FIXME: "loca" and "glyf" are not available for CFF fonts.
RefPtr<Font> Font::load_from_offset(ByteBuffer&& buffer, u32 offset)
{
    if (Checked<u32>::addition_would_overflow(offset, (u32)Sizes::OffsetTable)) {
        dbgln("Invalid offset in font header");
        return nullptr;
    }

    if (buffer.size() < offset + (u32)Sizes::OffsetTable) {
        dbgln("Font file too small");
        return nullptr;
    }

    Optional<ReadonlyBytes> opt_head_slice = {};
    Optional<ReadonlyBytes> opt_name_slice = {};
    Optional<ReadonlyBytes> opt_hhea_slice = {};
    Optional<ReadonlyBytes> opt_maxp_slice = {};
    Optional<ReadonlyBytes> opt_hmtx_slice = {};
    Optional<ReadonlyBytes> opt_cmap_slice = {};
    Optional<ReadonlyBytes> opt_loca_slice = {};
    Optional<ReadonlyBytes> opt_glyf_slice = {};

    Optional<Head> opt_head = {};
    Optional<Name> opt_name = {};
    Optional<Hhea> opt_hhea = {};
    Optional<Maxp> opt_maxp = {};
    Optional<Hmtx> opt_hmtx = {};
    Optional<Cmap> opt_cmap = {};
    Optional<Loca> opt_loca = {};

    auto num_tables = be_u16(buffer.offset_pointer(offset + (u32)Offsets::NumTables));
    if (buffer.size() < offset + (u32)Sizes::OffsetTable + num_tables * (u32)Sizes::TableRecord) {
        dbgln("Font file too small");
        return nullptr;
    }

    for (auto i = 0; i < num_tables; i++) {
        u32 record_offset = offset + (u32)Sizes::OffsetTable + i * (u32)Sizes::TableRecord;
        u32 tag = be_u32(buffer.offset_pointer(record_offset));
        u32 table_offset = be_u32(buffer.offset_pointer(record_offset + (u32)Offsets::TableRecord_Offset));
        u32 table_length = be_u32(buffer.offset_pointer(record_offset + (u32)Offsets::TableRecord_Length));

        if (Checked<u32>::addition_would_overflow(table_offset, table_length)) {
            dbgln("Invalid table offset/length in font.");
            return nullptr;
        }

        if (buffer.size() < table_offset + table_length) {
            dbgln("Font file too small");
            return nullptr;
        }
        auto buffer_here = ReadonlyBytes(buffer.offset_pointer(table_offset), table_length);

        // Get the table offsets we need.
        if (tag == tag_from_str("head")) {
            opt_head_slice = buffer_here;
        } else if (tag == tag_from_str("name")) {
            opt_name_slice = buffer_here;
        } else if (tag == tag_from_str("hhea")) {
            opt_hhea_slice = buffer_here;
        } else if (tag == tag_from_str("maxp")) {
            opt_maxp_slice = buffer_here;
        } else if (tag == tag_from_str("hmtx")) {
            opt_hmtx_slice = buffer_here;
        } else if (tag == tag_from_str("cmap")) {
            opt_cmap_slice = buffer_here;
        } else if (tag == tag_from_str("loca")) {
            opt_loca_slice = buffer_here;
        } else if (tag == tag_from_str("glyf")) {
            opt_glyf_slice = buffer_here;
        }
    }

    if (!opt_head_slice.has_value() || !(opt_head = Head::from_slice(opt_head_slice.value())).has_value()) {
        dbgln("Could not load Head");
        return nullptr;
    }
    auto head = opt_head.value();

    if (!opt_name_slice.has_value() || !(opt_name = Name::from_slice(opt_name_slice.value())).has_value()) {
        dbgln("Could not load Name");
        return nullptr;
    }
    auto name = opt_name.value();

    if (!opt_hhea_slice.has_value() || !(opt_hhea = Hhea::from_slice(opt_hhea_slice.value())).has_value()) {
        dbgln("Could not load Hhea");
        return nullptr;
    }
    auto hhea = opt_hhea.value();

    if (!opt_maxp_slice.has_value() || !(opt_maxp = Maxp::from_slice(opt_maxp_slice.value())).has_value()) {
        dbgln("Could not load Maxp");
        return nullptr;
    }
    auto maxp = opt_maxp.value();

    if (!opt_hmtx_slice.has_value() || !(opt_hmtx = Hmtx::from_slice(opt_hmtx_slice.value(), maxp.num_glyphs(), hhea.number_of_h_metrics())).has_value()) {
        dbgln("Could not load Hmtx");
        return nullptr;
    }
    auto hmtx = opt_hmtx.value();

    if (!opt_cmap_slice.has_value() || !(opt_cmap = Cmap::from_slice(opt_cmap_slice.value())).has_value()) {
        dbgln("Could not load Cmap");
        return nullptr;
    }
    auto cmap = opt_cmap.value();

    if (!opt_loca_slice.has_value() || !(opt_loca = Loca::from_slice(opt_loca_slice.value(), maxp.num_glyphs(), head.index_to_loc_format())).has_value()) {
        dbgln("Could not load Loca");
        return nullptr;
    }
    auto loca = opt_loca.value();

    if (!opt_glyf_slice.has_value()) {
        dbgln("Could not load Glyf");
        return nullptr;
    }
    auto glyf = Glyf(opt_glyf_slice.value());

    // Select cmap table. FIXME: Do this better. Right now, just looks for platform "Windows"
    // and corresponding encoding "Unicode full repertoire", or failing that, "Unicode BMP"
    for (u32 i = 0; i < cmap.num_subtables(); i++) {
        auto opt_subtable = cmap.subtable(i);
        if (!opt_subtable.has_value()) {
            continue;
        }
        auto subtable = opt_subtable.value();
        if (subtable.platform_id() == Cmap::Subtable::Platform::Windows) {
            if (subtable.encoding_id() == (u16)Cmap::Subtable::WindowsEncoding::UnicodeFullRepertoire) {
                cmap.set_active_index(i);
                break;
            }
            if (subtable.encoding_id() == (u16)Cmap::Subtable::WindowsEncoding::UnicodeBMP) {
                cmap.set_active_index(i);
                break;
            }
        }
    }

    return adopt_ref(*new Font(move(buffer), move(head), move(name), move(hhea), move(maxp), move(hmtx), move(cmap), move(loca), move(glyf)));
}

ScaledFontMetrics Font::metrics(float x_scale, float y_scale) const
{
    auto ascender = m_hhea.ascender() * y_scale;
    auto descender = m_hhea.descender() * y_scale;
    auto line_gap = m_hhea.line_gap() * y_scale;
    auto advance_width_max = m_hhea.advance_width_max() * x_scale;
    return ScaledFontMetrics {
        .ascender = (int)roundf(ascender),
        .descender = (int)roundf(descender),
        .line_gap = (int)roundf(line_gap),
        .advance_width_max = (int)roundf(advance_width_max),
    };
}

// FIXME: "loca" and "glyf" are not available for CFF fonts.
ScaledGlyphMetrics Font::glyph_metrics(u32 glyph_id, float x_scale, float y_scale) const
{
    if (glyph_id >= glyph_count()) {
        glyph_id = 0;
    }
    auto horizontal_metrics = m_hmtx.get_glyph_horizontal_metrics(glyph_id);
    auto glyph_offset = m_loca.get_glyph_offset(glyph_id);
    auto glyph = m_glyf.glyph(glyph_offset);
    int ascender = glyph.ascender();
    int descender = glyph.descender();
    return ScaledGlyphMetrics {
        .ascender = (int)roundf(ascender * y_scale),
        .descender = (int)roundf(descender * y_scale),
        .advance_width = (int)roundf(horizontal_metrics.advance_width * x_scale),
        .left_side_bearing = (int)roundf(horizontal_metrics.left_side_bearing * x_scale),
    };
}

// FIXME: "loca" and "glyf" are not available for CFF fonts.
RefPtr<Gfx::Bitmap> Font::raster_glyph(u32 glyph_id, float x_scale, float y_scale) const
{
    if (glyph_id >= glyph_count()) {
        glyph_id = 0;
    }
    auto glyph_offset = m_loca.get_glyph_offset(glyph_id);
    auto glyph = m_glyf.glyph(glyph_offset);
    return glyph.raster(x_scale, y_scale, [&](u16 glyph_id) {
        if (glyph_id >= glyph_count()) {
            glyph_id = 0;
        }
        auto glyph_offset = m_loca.get_glyph_offset(glyph_id);
        return m_glyf.glyph(glyph_offset);
    });
}

u32 Font::glyph_count() const
{
    return m_maxp.num_glyphs();
}

u16 Font::units_per_em() const
{
    return m_head.units_per_em();
}

String Font::family() const
{
    auto string = m_name.typographic_family_name();
    if (!string.is_empty())
        return string;
    return m_name.family_name();
}

String Font::variant() const
{
    auto string = m_name.typographic_subfamily_name();
    if (!string.is_empty())
        return string;
    return m_name.subfamily_name();
}

u16 Font::weight() const
{
    // FIXME: This is pretty naive, read weight from the actual font table(s)
    auto variant_name = variant();

    if (variant_name == "Thin")
        return 100;
    if (variant_name == "Extra Light")
        return 200;
    if (variant_name == "Light")
        return 300;
    if (variant_name == "Regular")
        return 400;
    if (variant_name == "Medium")
        return 500;
    if (variant_name == "Semi Bold")
        return 600;
    if (variant_name == "Bold")
        return 700;
    if (variant_name == "Extra Bold")
        return 800;
    if (variant_name == "Black")
        return 900;
    if (variant_name == "Extra Black")
        return 950;

    return 400;
}

bool Font::is_fixed_width() const
{
    // FIXME: Read this information from the font file itself.
    // FIXME: Although, it appears some application do similar hacks
    return glyph_metrics(glyph_id_for_codepoint('.'), 1, 1).advance_width == glyph_metrics(glyph_id_for_codepoint('X'), 1, 1).advance_width;
}

int ScaledFont::width(const StringView& string) const
{
    Utf8View utf8 { string };
    return width(utf8);
}

int ScaledFont::width(const Utf8View& utf8) const
{
    int width = 0;
    for (u32 codepoint : utf8) {
        u32 glyph_id = glyph_id_for_codepoint(codepoint);
        auto metrics = glyph_metrics(glyph_id);
        width += metrics.advance_width;
    }
    return width;
}

int ScaledFont::width(const Utf32View& utf32) const
{
    int width = 0;
    for (size_t i = 0; i < utf32.length(); i++) {
        u32 glyph_id = glyph_id_for_codepoint(utf32.code_points()[i]);
        auto metrics = glyph_metrics(glyph_id);
        width += metrics.advance_width;
    }
    return width;
}

RefPtr<Gfx::Bitmap> ScaledFont::raster_glyph(u32 glyph_id) const
{
    auto glyph_iterator = m_cached_glyph_bitmaps.find(glyph_id);
    if (glyph_iterator != m_cached_glyph_bitmaps.end())
        return glyph_iterator->value;

    auto glyph_bitmap = m_font->raster_glyph(glyph_id, m_x_scale, m_y_scale);
    m_cached_glyph_bitmaps.set(glyph_id, glyph_bitmap);
    return glyph_bitmap;
}

Gfx::Glyph ScaledFont::glyph(u32 code_point) const
{
    auto id = glyph_id_for_codepoint(code_point);
    auto bitmap = raster_glyph(id);
    auto metrics = glyph_metrics(id);
    return Gfx::Glyph(bitmap, metrics.left_side_bearing, metrics.advance_width, metrics.ascender);
}

u8 ScaledFont::glyph_width(size_t code_point) const
{
    auto id = glyph_id_for_codepoint(code_point);
    auto metrics = glyph_metrics(id);
    return metrics.advance_width;
}

int ScaledFont::glyph_or_emoji_width(u32 code_point) const
{
    auto id = glyph_id_for_codepoint(code_point);
    auto metrics = glyph_metrics(id);
    return metrics.advance_width;
}

u8 ScaledFont::glyph_fixed_width() const
{
    return glyph_metrics(glyph_id_for_codepoint(' ')).advance_width;
}

}
