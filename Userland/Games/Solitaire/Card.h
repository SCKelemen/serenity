/*
 * Copyright (c) 2020, Till Mayer <till.mayer@web.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Format.h>
#include <LibCore/Object.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CharacterBitmap.h>
#include <LibGfx/Rect.h>
#include <ctype.h>

namespace Solitaire {

class Card final : public Core::Object {
    C_OBJECT(Card)
public:
    static constexpr int width = 80;
    static constexpr int height = 100;
    static constexpr int card_count = 13;
    static constexpr Array<StringView, card_count> labels = {
        "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"
    };

    enum Type {
        Clubs,
        Diamonds,
        Hearts,
        Spades,
        __Count
    };

    virtual ~Card() override;

    Gfx::IntRect& rect() { return m_rect; }
    Gfx::IntPoint position() const { return m_rect.location(); }
    const Gfx::IntPoint& old_positon() const { return m_old_position; }
    uint8_t value() const { return m_value; };
    Type type() const { return m_type; }

    bool is_old_position_valid() const { return m_old_position_valid; }
    bool is_moving() const { return m_moving; }
    bool is_upside_down() const { return m_upside_down; }
    Gfx::Color color() const { return (m_type == Diamonds || m_type == Hearts) ? Color::Red : Color::Black; }

    void set_position(const Gfx::IntPoint p) { m_rect.set_location(p); }
    void set_moving(bool moving) { m_moving = moving; }
    void set_upside_down(bool upside_down) { m_upside_down = upside_down; }

    void save_old_position();

    void draw(GUI::Painter&) const;
    void clear(GUI::Painter&, const Color& background_color) const;
    void clear_and_draw(GUI::Painter&, const Color& background_color);

private:
    Card(Type type, uint8_t value);

    Gfx::IntRect m_rect;
    NonnullRefPtr<Gfx::Bitmap> m_front;
    Gfx::IntPoint m_old_position;
    Type m_type;
    uint8_t m_value;
    bool m_old_position_valid { false };
    bool m_moving { false };
    bool m_upside_down { false };
};

}

template<>
struct AK::Formatter<Solitaire::Card> : Formatter<FormatString> {
    void format(FormatBuilder& builder, const Solitaire::Card& card)
    {
        StringView type;

        switch (card.type()) {
        case Solitaire::Card::Type::Clubs:
            type = "C"sv;
            break;
        case Solitaire::Card::Type::Diamonds:
            type = "D"sv;
            break;
        case Solitaire::Card::Type::Hearts:
            type = "H"sv;
            break;
        case Solitaire::Card::Type::Spades:
            type = "S"sv;
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        Formatter<FormatString>::format(builder, "{:>2}{}", Solitaire::Card::labels[card.value()], type);
    }
};
