/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Maciej Zygmanowski <sppmacd@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Tab.h"
#include "BookmarksBarWidget.h"
#include "Browser.h"
#include "ConsoleWidget.h"
#include "DownloadWidget.h"
#include "InspectorWidget.h"
#include "WindowActions.h"
#include <AK/StringBuilder.h>
#include <Applications/Browser/TabGML.h>
#include <LibCore/ConfigFile.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TabWidget.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Toolbar.h>
#include <LibGUI/ToolbarContainer.h>
#include <LibGUI/Window.h>
#include <LibJS/Interpreter.h>
#include <LibWeb/Dump.h>
#include <LibWeb/InProcessWebView.h>
#include <LibWeb/Layout/BlockBox.h>
#include <LibWeb/Layout/InitialContainingBlockBox.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/OutOfProcessWebView.h>
#include <LibWeb/Page/Frame.h>

namespace Browser {

String g_search_engine = {};

URL url_from_user_input(const String& input)
{
    if (input.starts_with("?") && !g_search_engine.is_null()) {
        return URL(String::formatted(g_search_engine, urlencode(input.substring(1))));
    }

    auto url = URL(input);
    if (url.is_valid())
        return url;

    StringBuilder builder;
    builder.append("http://");
    builder.append(input);
    return URL(builder.build());
}

void Tab::start_download(const URL& url)
{
    auto window = GUI::Window::construct(this->window());
    window->resize(300, 150);
    window->set_title(String::formatted("0% of {}", url.basename()));
    window->set_resizable(false);
    window->set_main_widget<DownloadWidget>(url);
    window->show();
    [[maybe_unused]] auto& unused = window.leak_ref();
}

void Tab::view_source(const URL& url, const String& source)
{
    auto window = GUI::Window::construct(this->window());
    auto& editor = window->set_main_widget<GUI::TextEditor>();
    editor.set_text(source);
    editor.set_mode(GUI::TextEditor::ReadOnly);
    editor.set_ruler_visible(true);
    window->resize(640, 480);
    window->set_title(url.to_string());
    window->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/filetype-text.png"));
    window->show();
    [[maybe_unused]] auto& unused = window.leak_ref();
}

Tab::Tab(Type type)
    : m_type(type)
{
    load_from_gml(tab_gml);

    m_toolbar_container = *find_descendant_of_type_named<GUI::ToolbarContainer>("toolbar_container");
    auto& toolbar = *find_descendant_of_type_named<GUI::Toolbar>("toolbar");

    auto& webview_container = *find_descendant_of_type_named<GUI::Widget>("webview_container");

    if (m_type == Type::InProcessWebView)
        m_page_view = webview_container.add<Web::InProcessWebView>();
    else
        m_web_content_view = webview_container.add<Web::OutOfProcessWebView>();

    m_go_back_action = GUI::CommonActions::make_go_back_action([this](auto&) { go_back(); }, this);
    m_go_forward_action = GUI::CommonActions::make_go_forward_action([this](auto&) { go_forward(); }, this);
    m_go_home_action = GUI::CommonActions::make_go_home_action([this](auto&) { load(g_home_url); }, this);
    m_go_home_action->set_status_tip("Go to home page");

    toolbar.add_action(*m_go_back_action);
    toolbar.add_action(*m_go_forward_action);
    toolbar.add_action(*m_go_home_action);

    m_reload_action = GUI::CommonActions::make_reload_action([this](auto&) { reload(); }, this);
    m_reload_action->set_status_tip("Reload current page");

    toolbar.add_action(*m_reload_action);

    m_location_box = toolbar.add<GUI::TextBox>();
    m_location_box->set_placeholder("Address");

    m_location_box->on_return_pressed = [this] {
        auto url = url_from_user_input(m_location_box->text());
        load(url);
        view().set_focus(true);
    };

    m_location_box->add_custom_context_menu_action(GUI::Action::create("Paste & Go", [this](auto&) {
        m_location_box->set_text(GUI::Clipboard::the().data());
        m_location_box->on_return_pressed();
    }));

    m_bookmark_button = toolbar.add<GUI::Button>();
    m_bookmark_button->set_button_style(Gfx::ButtonStyle::Coolbar);
    m_bookmark_button->set_focus_policy(GUI::FocusPolicy::TabFocus);
    m_bookmark_button->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/bookmark-contour.png"));
    m_bookmark_button->set_fixed_size(22, 22);

    m_bookmark_button->on_click = [this](auto) {
        auto url = this->url().to_string();
        if (BookmarksBarWidget::the().contains_bookmark(url)) {
            BookmarksBarWidget::the().remove_bookmark(url);
        } else {
            BookmarksBarWidget::the().add_bookmark(url, m_title);
        }
        update_bookmark_button(url);
    };

    hooks().on_load_start = [this](auto& url) {
        m_location_box->set_icon(nullptr);
        m_location_box->set_text(url.to_string());

        // don't add to history if back or forward is pressed
        if (!m_is_history_navigation)
            m_history.push(url);
        m_is_history_navigation = false;

        update_actions();
        update_bookmark_button(url.to_string());
    };

    hooks().on_link_click = [this](auto& url, auto& target, unsigned modifiers) {
        if (target == "_blank" || modifiers == Mod_Ctrl) {
            on_tab_open_request(url);
        } else {
            load(url);
        }
    };

    m_link_context_menu = GUI::Menu::construct();
    auto link_default_action = GUI::Action::create("&Open", [this](auto&) {
        hooks().on_link_click(m_link_context_menu_url, "", 0);
    });
    m_link_context_menu->add_action(link_default_action);
    m_link_context_menu_default_action = link_default_action;
    m_link_context_menu->add_action(GUI::Action::create("Open in New &Tab", [this](auto&) {
        hooks().on_link_click(m_link_context_menu_url, "_blank", 0);
    }));
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(GUI::Action::create("&Copy URL", [this](auto&) {
        GUI::Clipboard::the().set_plain_text(m_link_context_menu_url.to_string());
    }));
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(GUI::Action::create("&Download", [this](auto&) {
        start_download(m_link_context_menu_url);
    }));

    hooks().on_link_context_menu_request = [this](auto& url, auto& screen_position) {
        m_link_context_menu_url = url;
        m_link_context_menu->popup(screen_position, m_link_context_menu_default_action);
    };

    m_image_context_menu = GUI::Menu::construct();
    m_image_context_menu->add_action(GUI::Action::create("&Open Image", [this](auto&) {
        hooks().on_link_click(m_image_context_menu_url, "", 0);
    }));
    m_image_context_menu->add_action(GUI::Action::create("Open Image in New &Tab", [this](auto&) {
        hooks().on_link_click(m_image_context_menu_url, "_blank", 0);
    }));
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(GUI::Action::create("&Copy Image", [this](auto&) {
        if (m_image_context_menu_bitmap.is_valid())
            GUI::Clipboard::the().set_bitmap(*m_image_context_menu_bitmap.bitmap());
    }));
    m_image_context_menu->add_action(GUI::Action::create("Copy Image &URL", [this](auto&) {
        GUI::Clipboard::the().set_plain_text(m_image_context_menu_url.to_string());
    }));
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(GUI::Action::create("&Download", [this](auto&) {
        start_download(m_image_context_menu_url);
    }));

    hooks().on_image_context_menu_request = [this](auto& image_url, auto& screen_position, const Gfx::ShareableBitmap& shareable_bitmap) {
        m_image_context_menu_url = image_url;
        m_image_context_menu_bitmap = shareable_bitmap;
        m_image_context_menu->popup(screen_position);
    };

    hooks().on_link_middle_click = [this](auto& href, auto&, auto) {
        hooks().on_link_click(href, "_blank", 0);
    };

    hooks().on_title_change = [this](auto& title) {
        if (title.is_null()) {
            m_title = url().to_string();
        } else {
            m_title = title;
        }
        if (on_title_change)
            on_title_change(m_title);
    };

    hooks().on_favicon_change = [this](auto& icon) {
        m_icon = icon;
        m_location_box->set_icon(&icon);
        if (on_favicon_change)
            on_favicon_change(icon);
    };

    hooks().on_get_cookie = [this](auto& url, auto source) -> String {
        if (on_get_cookie)
            return on_get_cookie(url, source);
        return {};
    };

    hooks().on_set_cookie = [this](auto& url, auto& cookie, auto source) {
        if (on_set_cookie)
            on_set_cookie(url, cookie, source);
    };

    hooks().on_get_source = [this](auto& url, auto& source) {
        view_source(url, source);
    };

    hooks().on_js_console_output = [this](auto& method, auto& line) {
        if (m_console_window) {
            auto* console_widget = static_cast<ConsoleWidget*>(m_console_window->main_widget());
            console_widget->handle_js_console_output(method, line);
        }
    };

    if (m_type == Type::InProcessWebView) {
        hooks().on_set_document = [this](auto* document) {
            if (document && m_console_window) {
                auto* console_widget = static_cast<ConsoleWidget*>(m_console_window->main_widget());
                console_widget->set_interpreter(document->interpreter().make_weak_ptr());
            }
        };
    }

    auto focus_location_box_action = GUI::Action::create(
        "Focus location box", { Mod_Ctrl, Key_L }, [this](auto&) {
            m_location_box->select_all();
            m_location_box->set_focus(true);
        },
        this);

    m_statusbar = *find_descendant_of_type_named<GUI::Statusbar>("statusbar");

    hooks().on_link_hover = [this](auto& url) {
        if (url.is_valid())
            m_statusbar->set_text(url.to_string());
        else
            m_statusbar->set_text("");
    };

    hooks().on_url_drop = [this](auto& url) {
        load(url);
    };

    m_menubar = GUI::Menubar::construct();

    auto& file_menu = m_menubar->add_menu("&File");
    file_menu.add_action(WindowActions::the().create_new_tab_action());

    auto close_tab_action = GUI::Action::create(
        "&Close Tab", { Mod_Ctrl, Key_W }, Gfx::Bitmap::load_from_file("/res/icons/16x16/close-tab.png"), [this](auto&) {
            on_tab_close_request(*this);
        },
        this);
    close_tab_action->set_status_tip("Close current tab");
    file_menu.add_action(close_tab_action);

    file_menu.add_separator();
    file_menu.add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the()->quit();
    }));

    auto& view_menu = m_menubar->add_menu("&View");
    view_menu.add_action(WindowActions::the().show_bookmarks_bar_action());
    view_menu.add_separator();
    view_menu.add_action(GUI::CommonActions::make_fullscreen_action(
        [this](auto&) {
            window()->set_fullscreen(!window()->is_fullscreen());

            auto is_fullscreen = window()->is_fullscreen();
            auto* tab_widget = static_cast<GUI::TabWidget*>(parent_widget());
            tab_widget->set_bar_visible(!is_fullscreen && tab_widget->children().size() > 1);
            m_toolbar_container->set_visible(!is_fullscreen);
            m_statusbar->set_visible(!is_fullscreen);

            if (is_fullscreen) {
                view().set_frame_thickness(0);
            } else {
                view().set_frame_thickness(2);
            }
        },
        this));

    auto& go_menu = m_menubar->add_menu("&Go");
    go_menu.add_action(*m_go_back_action);
    go_menu.add_action(*m_go_forward_action);
    go_menu.add_action(*m_go_home_action);
    go_menu.add_separator();
    go_menu.add_action(*m_reload_action);

    auto view_source_action = GUI::Action::create(
        "View &Source", { Mod_Ctrl, Key_U }, [this](auto&) {
            if (m_type == Type::InProcessWebView) {
                VERIFY(m_page_view->document());
                auto url = m_page_view->document()->url();
                auto source = m_page_view->document()->source();
                view_source(url, source);
            } else {
                m_web_content_view->get_source();
            }
        },
        this);
    view_source_action->set_status_tip("View source code of the current page");

    auto inspect_dom_tree_action = GUI::Action::create(
        "Inspect &DOM Tree", { Mod_None, Key_F12 }, [this](auto&) {
            if (m_type == Type::InProcessWebView) {
                if (!m_dom_inspector_window) {
                    m_dom_inspector_window = GUI::Window::construct(window());
                    m_dom_inspector_window->resize(300, 500);
                    m_dom_inspector_window->set_title("DOM inspector");
                    m_dom_inspector_window->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/inspector-object.png"));
                    m_dom_inspector_window->set_main_widget<InspectorWidget>();
                }
                auto* inspector_widget = static_cast<InspectorWidget*>(m_dom_inspector_window->main_widget());
                inspector_widget->set_document(m_page_view->document());
                m_dom_inspector_window->show();
                m_dom_inspector_window->move_to_front();
            } else {
                TODO();
            }
        },
        this);
    inspect_dom_tree_action->set_status_tip("Open DOM inspector window for this page");

    auto& inspect_menu = m_menubar->add_menu("&Inspect");
    inspect_menu.add_action(*view_source_action);
    inspect_menu.add_action(*inspect_dom_tree_action);

    auto js_console_action = GUI::Action::create(
        "Open &JS Console", { Mod_Ctrl, Key_I }, [this](auto&) {
            if (m_type == Type::InProcessWebView) {
                if (!m_console_window) {
                    m_console_window = GUI::Window::construct(window());
                    m_console_window->resize(500, 300);
                    m_console_window->set_title("JS Console");
                    m_console_window->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/filetype-javascript.png"));
                    m_console_window->set_main_widget<ConsoleWidget>();
                }
                auto* console_widget = static_cast<ConsoleWidget*>(m_console_window->main_widget());
                console_widget->set_interpreter(m_page_view->document()->interpreter().make_weak_ptr());
                m_console_window->show();
                m_console_window->move_to_front();
            } else {
                if (!m_console_window) {
                    m_console_window = GUI::Window::construct(window());
                    m_console_window->resize(500, 300);
                    m_console_window->set_title("JS Console");
                    m_console_window->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/filetype-javascript.png"));
                    m_console_window->set_main_widget<ConsoleWidget>();
                }
                auto* console_widget = static_cast<ConsoleWidget*>(m_console_window->main_widget());
                console_widget->on_js_input = [this](const String& js_source) {
                    m_web_content_view->js_console_input(js_source);
                };
                console_widget->clear_output();
                m_web_content_view->js_console_initialize();
                m_console_window->show();
                m_console_window->move_to_front();
            }
        },
        this);
    js_console_action->set_status_tip("Open JavaScript console for this page");
    inspect_menu.add_action(js_console_action);

    auto& settings_menu = m_menubar->add_menu("&Settings");

    m_search_engine_actions.set_exclusive(true);
    auto& search_engine_menu = settings_menu.add_submenu("&Search Engine");

    auto add_search_engine = [&](auto& name, auto& url_format) {
        auto action = GUI::Action::create_checkable(
            name, [&](auto&) {
                g_search_engine = url_format;
                auto m_config = Core::ConfigFile::get_for_app("Browser");
                m_config->write_entry("Preferences", "SearchEngine", g_search_engine);
            },
            this);
        search_engine_menu.add_action(action);
        m_search_engine_actions.add_action(action);

        if (g_search_engine == url_format) {
            action->set_checked(true);
        }
        action->set_status_tip(url_format);
    };

    auto disable_search_engine_action = GUI::Action::create_checkable(
        "Disable", [this](auto&) {
            g_search_engine = {};
            auto m_config = Core::ConfigFile::get_for_app("Browser");
            m_config->write_entry("Preferences", "SearchEngine", g_search_engine);
        },
        this);
    search_engine_menu.add_action(disable_search_engine_action);
    m_search_engine_actions.add_action(disable_search_engine_action);
    disable_search_engine_action->set_checked(true);

    // FIXME: Support adding custom search engines
    add_search_engine("Bing", "https://www.bing.com/search?q={}");
    add_search_engine("DuckDuckGo", "https://duckduckgo.com/?q={}");
    add_search_engine("FrogFind", "http://frogfind.com/?q={}");
    add_search_engine("GitHub", "https://github.com/search?q={}");
    add_search_engine("Google", "https://google.com/search?q={}");
    add_search_engine("Yandex", "https://yandex.com/search/?text={}");

    auto& debug_menu = m_menubar->add_menu("&Debug");
    debug_menu.add_action(GUI::Action::create(
        "Dump &DOM Tree", [this](auto&) {
            if (m_type == Type::InProcessWebView) {
                Web::dump_tree(*m_page_view->document());
            } else {
                m_web_content_view->debug_request("dump-dom-tree");
            }
        },
        this));
    debug_menu.add_action(GUI::Action::create(
        "Dump &Layout Tree", [this](auto&) {
            if (m_type == Type::InProcessWebView) {
                Web::dump_tree(*m_page_view->document()->layout_node());
            } else {
                m_web_content_view->debug_request("dump-layout-tree");
            }
        },
        this));
    debug_menu.add_action(GUI::Action::create(
        "Dump &Style Sheets", [this](auto&) {
            if (m_type == Type::InProcessWebView) {
                for (auto& sheet : m_page_view->document()->style_sheets().sheets()) {
                    Web::dump_sheet(sheet);
                }
            } else {
                m_web_content_view->debug_request("dump-style-sheets");
            }
        },
        this));
    debug_menu.add_action(GUI::Action::create("Dump &History", { Mod_Ctrl, Key_H }, [&](auto&) {
        m_history.dump();
    }));
    debug_menu.add_action(GUI::Action::create("Dump C&ookies", [&](auto&) {
        if (on_dump_cookies)
            on_dump_cookies();
    }));
    debug_menu.add_separator();
    auto line_box_borders_action = GUI::Action::create_checkable(
        "&Line Box Borders", [this](auto& action) {
            if (m_type == Type::InProcessWebView) {
                m_page_view->set_should_show_line_box_borders(action.is_checked());
                m_page_view->update();
            } else {
                m_web_content_view->debug_request("set-line-box-borders", action.is_checked() ? "on" : "off");
            }
        },
        this);
    line_box_borders_action->set_checked(false);
    debug_menu.add_action(line_box_borders_action);

    debug_menu.add_separator();
    debug_menu.add_action(GUI::Action::create("Collect &Garbage", { Mod_Ctrl | Mod_Shift, Key_G }, [this](auto&) {
        if (m_type == Type::InProcessWebView) {
            if (auto* document = m_page_view->document()) {
                document->interpreter().heap().collect_garbage(JS::Heap::CollectionType::CollectGarbage, true);
            }
        } else {
            m_web_content_view->debug_request("collect-garbage");
        }
    }));
    debug_menu.add_action(GUI::Action::create("Clear &Cache", { Mod_Ctrl | Mod_Shift, Key_C }, [this](auto&) {
        if (m_type == Type::InProcessWebView) {
            Web::ResourceLoader::the().clear_cache();
        } else {
            m_web_content_view->debug_request("clear-cache");
        }
    }));

    m_user_agent_spoof_actions.set_exclusive(true);
    auto& spoof_user_agent_menu = debug_menu.add_submenu("Spoof User Agent");
    m_disable_user_agent_spoofing = GUI::Action::create_checkable("Disabled", [&](auto&) {
        if (m_type == Type::InProcessWebView) {
            Web::ResourceLoader::the().set_user_agent(Web::default_user_agent);
        } else {
            m_web_content_view->debug_request("spoof-user-agent", Web::default_user_agent);
        }
    });
    m_disable_user_agent_spoofing->set_status_tip(Web::default_user_agent);
    spoof_user_agent_menu.add_action(*m_disable_user_agent_spoofing);
    m_user_agent_spoof_actions.add_action(*m_disable_user_agent_spoofing);
    m_disable_user_agent_spoofing->set_checked(true);

    auto add_user_agent = [&](auto& name, auto& user_agent) {
        auto action = GUI::Action::create_checkable(name, [&](auto&) {
            if (m_type == Type::InProcessWebView) {
                Web::ResourceLoader::the().set_user_agent(user_agent);
            } else {
                m_web_content_view->debug_request("spoof-user-agent", user_agent);
            }
        });
        action->set_status_tip(user_agent);
        spoof_user_agent_menu.add_action(action);
        m_user_agent_spoof_actions.add_action(action);
    };
    add_user_agent("Chrome Linux Desktop", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.4389.128 Safari/537.36");
    add_user_agent("Firefox Linux Desktop", "Mozilla/5.0 (X11; Linux i686; rv:87.0) Gecko/20100101 Firefox/87.0");
    add_user_agent("Safari macOS Desktop", "Mozilla/5.0 (Macintosh; Intel Mac OS X 11_2_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/14.0.3 Safari/605.1.15");
    add_user_agent("Chrome Android Mobile", "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/90.0.4430.66 Mobile Safari/537.36");
    add_user_agent("Firefox Android Mobile", "Mozilla/5.0 (Android 11; Mobile; rv:68.0) Gecko/68.0 Firefox/86.0");
    add_user_agent("Safari iOS Mobile", "Mozilla/5.0 (iPhone; CPU iPhone OS 14_4_2 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/14.0 Mobile/15E148 Safari/604.1");

    auto custom_user_agent = GUI::Action::create_checkable("Custom", [&](auto& action) {
        String user_agent;
        if (GUI::InputBox::show(window(), user_agent, "Enter User Agent:", "Custom User Agent") != GUI::InputBox::ExecOK || user_agent.is_empty() || user_agent.is_null()) {
            m_disable_user_agent_spoofing->activate();
            return;
        }
        if (m_type == Type::InProcessWebView) {
            Web::ResourceLoader::the().set_user_agent(user_agent);
        } else {
            m_web_content_view->debug_request("spoof-user-agent", user_agent);
        }
        action.set_status_tip(user_agent);
    });
    spoof_user_agent_menu.add_action(custom_user_agent);
    m_user_agent_spoof_actions.add_action(custom_user_agent);

    auto& help_menu = m_menubar->add_menu("&Help");
    help_menu.add_action(WindowActions::the().about_action());

    m_tab_context_menu = GUI::Menu::construct();
    m_tab_context_menu->add_action(GUI::Action::create("&Reload Tab", [this](auto&) {
        m_reload_action->activate();
    }));
    m_tab_context_menu->add_action(GUI::Action::create("&Close Tab", [this](auto&) {
        on_tab_close_request(*this);
    }));

    m_page_context_menu = GUI::Menu::construct();
    m_page_context_menu->add_action(*m_go_back_action);
    m_page_context_menu->add_action(*m_go_forward_action);
    m_page_context_menu->add_action(*m_reload_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*view_source_action);
    m_page_context_menu->add_action(*inspect_dom_tree_action);
    hooks().on_context_menu_request = [&](auto& screen_position) {
        m_page_context_menu->popup(screen_position);
    };
}

Tab::~Tab()
{
}

void Tab::load(const URL& url, LoadType load_type)
{
    m_is_history_navigation = (load_type == LoadType::HistoryNavigation);

    if (m_type == Type::InProcessWebView)
        m_page_view->load(url);
    else
        m_web_content_view->load(url);
}

URL Tab::url() const
{
    if (m_type == Type::InProcessWebView)
        return m_page_view->url();
    return m_web_content_view->url();
}

void Tab::reload()
{
    load(url());
}

void Tab::go_back()
{
    m_history.go_back();
    update_actions();
    load(m_history.current(), LoadType::HistoryNavigation);
}

void Tab::go_forward()
{
    m_history.go_forward();
    update_actions();
    load(m_history.current(), LoadType::HistoryNavigation);
}

void Tab::update_actions()
{
    m_go_back_action->set_enabled(m_history.can_go_back());
    m_go_forward_action->set_enabled(m_history.can_go_forward());
}

void Tab::update_bookmark_button(const String& url)
{
    if (BookmarksBarWidget::the().contains_bookmark(url)) {
        m_bookmark_button->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/bookmark-filled.png"));
        m_bookmark_button->set_tooltip("Remove Bookmark");
    } else {
        m_bookmark_button->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/bookmark-contour.png"));
        m_bookmark_button->set_tooltip("Add Bookmark");
    }
}

void Tab::did_become_active()
{
    if (m_type == Type::InProcessWebView) {
        Web::ResourceLoader::the().on_load_counter_change = [this] {
            if (Web::ResourceLoader::the().pending_loads() == 0) {
                m_statusbar->set_text("");
                return;
            }
            m_statusbar->set_text(String::formatted("Loading ({} pending resources...)", Web::ResourceLoader::the().pending_loads()));
        };
    }

    BookmarksBarWidget::the().on_bookmark_click = [this](auto& url, unsigned modifiers) {
        if (modifiers & Mod_Ctrl)
            on_tab_open_request(url);
        else
            load(url);
    };

    BookmarksBarWidget::the().on_bookmark_hover = [this](auto&, auto& url) {
        m_statusbar->set_text(url);
    };

    BookmarksBarWidget::the().remove_from_parent();
    m_toolbar_container->add_child(BookmarksBarWidget::the());

    auto is_fullscreen = window()->is_fullscreen();
    m_toolbar_container->set_visible(!is_fullscreen);
    m_statusbar->set_visible(!is_fullscreen);

    window()->set_menubar(m_menubar);
}

void Tab::context_menu_requested(const Gfx::IntPoint& screen_position)
{
    m_tab_context_menu->popup(screen_position);
}

GUI::AbstractScrollableWidget& Tab::view()
{
    if (m_type == Type::InProcessWebView)
        return *m_page_view;
    return *m_web_content_view;
}

Web::WebViewHooks& Tab::hooks()
{
    if (m_type == Type::InProcessWebView)
        return *m_page_view;
    return *m_web_content_view;
}

void Tab::action_entered(GUI::Action& action)
{
    m_statusbar->set_override_text(action.status_tip());
}

void Tab::action_left(GUI::Action&)
{
    m_statusbar->set_override_text({});
}

}
