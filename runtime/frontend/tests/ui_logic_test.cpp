#include "ui_logic.h"

#include <dsr/debug_editor.h>
#include <array>
#include <cassert>
#include <climits>
#include <initializer_list>
#include <iostream>
#include <string_view>

int main() {
    using namespace dsr::ui_detail;
    FocusModel focus(4, 3);
    assert(focus.selected() == 3);
    focus.move(1);
    assert(focus.selected() == 0);
    focus.move(-1);
    assert(focus.selected() == 3);
    focus.reset(0);
    focus.move(7);
    assert(focus.count() == 0 && focus.selected() == 0);

    TextBuffer text(32);
    text.insert("abcd");
    text.move_cursor(-2);
    text.insert("XY");
    assert(text.value() == "abXYcd");
    text.backspace();
    assert(text.value() == "abXcd");
    text.erase_forward();
    assert(text.value() == "abXd");
    text.home();
    text.move_cursor(2);
    text.erase_forward();
    assert(text.value() == "abd");
    text.end();
    text.insert("!");
    assert(text.value() == "abd!");

    TextBuffer utf8(2);
    utf8.insert("a\xc3\xa9"); // Do not leave a partial UTF-8 code point at the byte limit.
    assert(utf8.value() == "a");
    TextBuffer utf8_full(8);
    utf8_full.set("a\xc3\xa9");
    utf8_full.move_cursor(-1);
    utf8_full.backspace();
    assert(utf8_full.value() == "\xc3\xa9");

    const auto text_event = [](std::string_view value) {
        dsr::EditorEvent event;
        assert(value.size() <= event.text.size());
        event.text_size = static_cast<std::uint8_t>(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) event.text[i] = value[i];
        return event;
    };
    const auto editor_event = [](dsr::EditorEventType type) {
        dsr::EditorEvent event;
        event.type = type;
        return event;
    };
    dsr::AnnotationEditor ordered;
    const auto apply = [&](dsr::EditorEventType type) { return ordered.apply(editor_event(type)); };
    const auto run = [&](std::initializer_list<dsr::EditorEvent> events) {
        for (const auto& event : events) ordered.apply(event);
    };

    // Ordered input is applied immediately: cursor edits between text events
    // affect insertion position rather than being coalesced or reordered.
    run({text_event("ab"), editor_event(dsr::EditorEventType::Left), text_event("x")});
    assert(ordered.text() == "axb");
    assert(ordered.cursor() == 2);

    ordered.clear();
    run({text_event("a"), editor_event(dsr::EditorEventType::Backspace), text_event("b")});
    assert(ordered.text() == "b");
    ordered.clear();
    run({text_event("first"), text_event(" "), text_event("second"),
         editor_event(dsr::EditorEventType::Left), editor_event(dsr::EditorEventType::Delete)});
    assert(ordered.text() == "first secon");
    ordered.clear();
    run({text_event("abcd"), editor_event(dsr::EditorEventType::Home), text_event("!"),
         editor_event(dsr::EditorEventType::End), editor_event(dsr::EditorEventType::Newline),
         text_event("tail")});
    assert(ordered.text() == "!abcd\ntail");
    assert(ordered.cursor() == ordered.size());
    assert(apply(dsr::EditorEventType::Save) == dsr::EditorResult::Save);
    assert(ordered.text() == "!abcd\ntail");
    assert(apply(dsr::EditorEventType::Cancel) == dsr::EditorResult::Cancel);


    ordered.clear();
    run({text_event("ab"), editor_event(dsr::EditorEventType::Home),
         editor_event(dsr::EditorEventType::Right), text_event("x")});
    assert(ordered.text() == "axb" && ordered.cursor() == 2);
    ordered.clear();
    run({text_event("one\ntwo\nthree"), editor_event(dsr::EditorEventType::Home),
         editor_event(dsr::EditorEventType::Up)});
    assert(ordered.cursor() == 4);
    apply(dsr::EditorEventType::Down);
    apply(dsr::EditorEventType::Up);
    apply(dsr::EditorEventType::End);
    assert(ordered.cursor() == 7);
    apply(dsr::EditorEventType::Up);
    assert(ordered.cursor() == 3);

    ordered.clear();
    run({text_event("a\xc3\xa9" "b"), editor_event(dsr::EditorEventType::Left)});
    assert(ordered.cursor() == 3);
    apply(dsr::EditorEventType::Backspace);
    assert(ordered.text() == "ab" && ordered.cursor() == 1);
    ordered.clear();
    run({text_event("a\xc3\xa9" "b"), editor_event(dsr::EditorEventType::Home),
         editor_event(dsr::EditorEventType::Delete)});
    assert(ordered.text() == "\xc3\xa9" "b" && ordered.cursor() == 0);

    dsr::AnnotationEditor bounded(4);
    bounded.insert("abc");
    bounded.apply(editor_event(dsr::EditorEventType::Home));
    bounded.apply(text_event("XY"));
    assert(bounded.text() == "Xabc" && bounded.cursor() == 1);
    bounded.apply(text_event("\xc3\xa9"));
    assert(bounded.text() == "Xabc" && bounded.cursor() == 1);

    constexpr std::array<dsr::DebugMenuItem, dsr::kDebugMenuRowCount> menu_items = {
        dsr::DebugMenuItem::PauseResume, dsr::DebugMenuItem::Step,
        dsr::DebugMenuItem::SlowHalf, dsr::DebugMenuItem::SlowQuarter,
        dsr::DebugMenuItem::FastForward, dsr::DebugMenuItem::AddNote,
        dsr::DebugMenuItem::Marker, dsr::DebugMenuItem::QuickBundle,
        dsr::DebugMenuItem::FrameGraph, dsr::DebugMenuItem::AudioBar,
        dsr::DebugMenuItem::CpuPc, dsr::DebugMenuItem::TextureSlots,
        dsr::DebugMenuItem::AudioMixer, dsr::DebugMenuItem::TextureInspector,
        dsr::DebugMenuItem::Screenshot, dsr::DebugMenuItem::Folder,
        dsr::DebugMenuItem::Help, dsr::DebugMenuItem::Close,
    };
    dsr::DebugMenuModel menu;
    for (std::size_t row = 0; row < menu_items.size(); ++row) {
        menu.select(static_cast<int>(row));
        assert(menu.selected() == menu_items[row]);
        assert(menu.activate() == menu_items[row]);
        assert(!dsr::debug_menu_hotkey(menu_items[row]).empty());
    }
    assert(dsr::debug_menu_hotkey(dsr::DebugMenuItem::PauseResume) == "Space / Pause");
    assert(dsr::debug_menu_hotkey(dsr::DebugMenuItem::Step) == "F8");
    assert(dsr::debug_menu_label(dsr::DebugMenuItem::Count).empty());
    assert(dsr::debug_menu_hotkey(dsr::DebugMenuItem::Count).empty());
    menu.select(0);
    assert(menu.selected() == dsr::DebugMenuItem::PauseResume);
    assert(menu.activate() == dsr::DebugMenuItem::PauseResume);
    assert(menu.selected() == dsr::DebugMenuItem::PauseResume);
    menu.select(static_cast<int>(menu_items.size() - 1));
    menu.move(1);
    assert(menu.selected() == dsr::DebugMenuItem::PauseResume);
    menu.move(-1);
    assert(menu.selected() == dsr::DebugMenuItem::Close);
    menu.move(INT_MAX);
    menu.move(INT_MIN);
    assert(static_cast<std::size_t>(menu.selected()) < dsr::kDebugMenuRowCount);
    menu.select(-1);
    assert(menu.selected() == dsr::DebugMenuItem::Close);
    assert(elide_middle("/tmp/dq9-test-home/images/very-long-synthetic-name.nds", 16).size() <= 16);
    assert(elide_middle("abcdefghijklmno", 9) == "abc...mno");
    assert(elide_middle("a\xc3\xa9" "bcdef", 6) == "a...ef");

    assert(bundle_component("20261010-123456", 2, "Audio: pops, then lag!") ==
           "20261010-123456-2-audio-pops-then-lag");
    assert(bundle_component("t", 1, "!!!") == "t-1-note");
    assert(config_escape("a%=b\n") == "a%25=b%0A");
    assert(config_unescape("a%25=b%0A") == "a%=b\n");
    std::cout << "UI pure logic tests passed\n";
}
