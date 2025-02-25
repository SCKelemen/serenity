/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HexEditorWidget.h"
#include "FindDialog.h"
#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibGUI/Action.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Toolbar.h>
#include <stdio.h>
#include <string.h>

HexEditorWidget::HexEditorWidget()
{
    set_fill_with_background_color(true);
    set_layout<GUI::VerticalBoxLayout>();
    layout()->set_spacing(2);

    m_editor = add<HexEditor>();

    m_editor->on_status_change = [this](int position, HexEditor::EditMode edit_mode, int selection_start, int selection_end) {
        m_statusbar->set_text(0, String::formatted("Offset: {:#08X}", position));
        m_statusbar->set_text(1, String::formatted("Edit Mode: {}", edit_mode == HexEditor::EditMode::Hex ? "Hex" : "Text"));
        m_statusbar->set_text(2, String::formatted("Selection Start: {}", selection_start));
        m_statusbar->set_text(3, String::formatted("Selection End: {}", selection_end));
        m_statusbar->set_text(4, String::formatted("Selected Bytes: {}", abs(selection_end - selection_start) + 1));
    };

    m_editor->on_change = [this] {
        bool was_dirty = m_document_dirty;
        m_document_dirty = true;
        if (!was_dirty)
            update_title();
    };

    m_statusbar = add<GUI::Statusbar>(5);

    m_new_action = GUI::Action::create("New", { Mod_Ctrl, Key_N }, Gfx::Bitmap::load_from_file("/res/icons/16x16/new.png"), [this](const GUI::Action&) {
        if (m_document_dirty) {
            if (GUI::MessageBox::show(window(), "Save changes to current file first?", "Warning", GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::OKCancel) != GUI::Dialog::ExecResult::ExecOK)
                return;
            m_save_action->activate();
        }

        String value;
        if (GUI::InputBox::show(window(), value, "Enter new file size:", "New file size") == GUI::InputBox::ExecOK && !value.is_empty()) {
            auto file_size = value.to_int();
            if (file_size.has_value() && file_size.value() > 0) {
                m_document_dirty = false;
                m_editor->set_buffer(ByteBuffer::create_zeroed(file_size.value()));
                set_path(LexicalPath());
                update_title();
            } else {
                GUI::MessageBox::show(window(), "Invalid file size entered.", "Error", GUI::MessageBox::Type::Error);
            }
        }
    });

    m_open_action = GUI::CommonActions::make_open_action([this](auto&) {
        Optional<String> open_path = GUI::FilePicker::get_open_filepath(window());

        if (!open_path.has_value())
            return;

        open_file(open_path.value());
    });

    m_save_action = GUI::CommonActions::make_save_action([&](auto&) {
        if (!m_path.is_empty()) {
            if (!m_editor->write_to_file(m_path)) {
                GUI::MessageBox::show(window(), "Unable to save file.\n", "Error", GUI::MessageBox::Type::Error);
            } else {
                m_document_dirty = false;
                update_title();
            }
            return;
        }

        m_save_as_action->activate();
    });

    m_save_as_action = GUI::CommonActions::make_save_as_action([&](auto&) {
        Optional<String> save_path = GUI::FilePicker::get_save_filepath(window(), m_name.is_null() ? "Untitled" : m_name, m_extension.is_null() ? "bin" : m_extension);
        if (!save_path.has_value())
            return;

        if (!m_editor->write_to_file(save_path.value())) {
            GUI::MessageBox::show(window(), "Unable to save file.\n", "Error", GUI::MessageBox::Type::Error);
            return;
        }

        m_document_dirty = false;
        set_path(LexicalPath(save_path.value()));
        dbgln("Wrote document to {}", save_path.value());
    });

    m_editor->set_focus(true);
}

HexEditorWidget::~HexEditorWidget()
{
}

void HexEditorWidget::initialize_menubar(GUI::Menubar& menubar)
{
    auto& file_menu = menubar.add_menu("&File");
    file_menu.add_action(*m_new_action);
    file_menu.add_action(*m_open_action);
    file_menu.add_action(*m_save_action);
    file_menu.add_action(*m_save_as_action);
    file_menu.add_separator();
    file_menu.add_action(GUI::CommonActions::make_quit_action([this](auto&) {
        if (!request_close())
            return;
        GUI::Application::the()->quit();
    }));

    m_goto_decimal_offset_action = GUI::Action::create("Go to Offset (&Decimal)...", { Mod_Ctrl | Mod_Shift, Key_G }, Gfx::Bitmap::load_from_file("/res/icons/16x16/go-forward.png"), [this](const GUI::Action&) {
        String value;
        if (GUI::InputBox::show(window(), value, "Enter decimal offset:", "Go to Offset") == GUI::InputBox::ExecOK && !value.is_empty()) {
            auto new_offset = value.to_int();
            if (new_offset.has_value())
                m_editor->set_position(new_offset.value());
        }
    });

    m_goto_hex_offset_action = GUI::Action::create("Go to Offset (&Hex)...", { Mod_Ctrl, Key_G }, Gfx::Bitmap::load_from_file("/res/icons/16x16/go-forward.png"), [this](const GUI::Action&) {
        String value;
        if (GUI::InputBox::show(window(), value, "Enter hexadecimal offset:", "Go to Offset") == GUI::InputBox::ExecOK && !value.is_empty()) {
            auto new_offset = strtol(value.characters(), nullptr, 16);
            m_editor->set_position(new_offset);
        }
    });

    auto& edit_menu = menubar.add_menu("&Edit");
    edit_menu.add_action(GUI::Action::create("&Fill Selection...", { Mod_Ctrl, Key_B }, [&](const GUI::Action&) {
        String value;
        if (GUI::InputBox::show(window(), value, "Fill byte (hex):", "Fill Selection") == GUI::InputBox::ExecOK && !value.is_empty()) {
            auto fill_byte = strtol(value.characters(), nullptr, 16);
            m_editor->fill_selection(fill_byte);
        }
    }));
    edit_menu.add_separator();
    edit_menu.add_action(*m_goto_decimal_offset_action);
    edit_menu.add_action(*m_goto_hex_offset_action);
    edit_menu.add_separator();
    edit_menu.add_action(GUI::Action::create("Copy &Hex", { Mod_Ctrl, Key_C }, [&](const GUI::Action&) {
        m_editor->copy_selected_hex_to_clipboard();
    }));
    edit_menu.add_action(GUI::Action::create("Copy &Text", { Mod_Ctrl | Mod_Shift, Key_C }, Gfx::Bitmap::load_from_file("/res/icons/16x16/edit-copy.png"), [&](const GUI::Action&) {
        m_editor->copy_selected_text_to_clipboard();
    }));
    edit_menu.add_action(GUI::Action::create("Copy as &C Code", { Mod_Alt | Mod_Shift, Key_C }, [&](const GUI::Action&) {
        m_editor->copy_selected_hex_to_clipboard_as_c_code();
    }));
    edit_menu.add_separator();
    edit_menu.add_action(GUI::Action::create("&Find", { Mod_Ctrl, Key_F }, Gfx::Bitmap::load_from_file("/res/icons/16x16/find.png"), [&](const GUI::Action&) {
        auto old_buffer = m_search_buffer;
        if (FindDialog::show(window(), m_search_text, m_search_buffer) == GUI::InputBox::ExecOK) {

            bool same_buffers = false;
            if (old_buffer.size() == m_search_buffer.size()) {
                if (memcmp(old_buffer.data(), m_search_buffer.data(), old_buffer.size()) == 0)
                    same_buffers = true;
            }

            auto result = m_editor->find_and_highlight(m_search_buffer, same_buffers ? last_found_index() : 0);

            if (result == -1) {
                GUI::MessageBox::show(window(), String::formatted("Pattern \"{}\" not found in this file", m_search_text), "Not found", GUI::MessageBox::Type::Warning);
                return;
            }
            m_last_found_index = result;
        }
    }));

    edit_menu.add_action(GUI::Action::create("Find &Next", { Mod_None, Key_F3 }, Gfx::Bitmap::load_from_file("/res/icons/16x16/find-next.png"), [&](const GUI::Action&) {
        if (m_search_text.is_empty() || m_search_buffer.is_empty()) {
            GUI::MessageBox::show(window(), "Nothing to search for", "Not found", GUI::MessageBox::Type::Warning);
            return;
        }

        auto result = m_editor->find_and_highlight(m_search_buffer, last_found_index());
        if (!result) {
            GUI::MessageBox::show(window(), String::formatted("No more matches for \"{}\" found in this file", m_search_text), "Not found", GUI::MessageBox::Type::Warning);
            return;
        }
        m_editor->update();
        m_last_found_index = result;
    }));

    auto& view_menu = menubar.add_menu("&View");
    m_bytes_per_row_actions.set_exclusive(true);
    auto& bytes_per_row_menu = view_menu.add_submenu("Bytes per &Row");
    for (int i = 8; i <= 32; i += 8) {
        auto action = GUI::Action::create_checkable(String::number(i), [this, i](auto&) {
            m_editor->set_bytes_per_row(i);
            m_editor->update();
        });
        m_bytes_per_row_actions.add_action(action);
        bytes_per_row_menu.add_action(action);
        if (i == 16)
            action->set_checked(true);
    }

    auto& help_menu = menubar.add_menu("&Help");
    help_menu.add_action(GUI::CommonActions::make_about_action("Hex Editor", GUI::Icon::default_icon("app-hex-editor"), window()));
}

void HexEditorWidget::set_path(const LexicalPath& lexical_path)
{
    m_path = lexical_path.string();
    m_name = lexical_path.title();
    m_extension = lexical_path.extension();
    update_title();
}

void HexEditorWidget::update_title()
{
    StringBuilder builder;
    builder.append(m_path);
    if (m_document_dirty)
        builder.append(" (*)");
    builder.append(" - Hex Editor");
    window()->set_title(builder.to_string());
}

void HexEditorWidget::open_file(const String& path)
{
    auto file = Core::File::construct(path);
    if (!file->open(Core::OpenMode::ReadOnly)) {
        GUI::MessageBox::show(window(), String::formatted("Opening \"{}\" failed: {}", path, strerror(errno)), "Error", GUI::MessageBox::Type::Error);
        return;
    }

    m_document_dirty = false;
    m_editor->set_buffer(file->read_all()); // FIXME: On really huge files, this is never going to work. Should really create a framework to fetch data from the file on-demand.
    set_path(LexicalPath(path));
}

bool HexEditorWidget::request_close()
{
    if (!m_document_dirty)
        return true;
    auto result = GUI::MessageBox::show(window(), "The file has been modified. Quit without saving?", "Quit without saving?", GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::OKCancel);
    return result == GUI::MessageBox::ExecOK;
}
