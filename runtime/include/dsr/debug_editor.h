#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dsr {

enum class EditorEventType : std::uint8_t {
    Text,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Newline,
    Save,
    Cancel,
};

struct EditorEvent {
    EditorEventType type = EditorEventType::Text;
    std::array<char, 32> text{};
    std::uint8_t text_size = 0;
};

enum class EditorResult : std::uint8_t { Continue, Save, Cancel };

enum class DebugMenuItem : std::uint8_t {
    PauseResume,
    Step,
    SlowHalf,
    SlowQuarter,
    FastForward,
    AddNote,
    Marker,
    QuickBundle,
    FrameGraph,
    AudioBar,
    CpuPc,
    TextureSlots,
    AudioMixer,
    TextureInspector,
    Screenshot,
    Folder,
    Help,
    Close,
    Count,
};

constexpr std::size_t kDebugMenuRowCount = static_cast<std::size_t>(DebugMenuItem::Count);

class DebugMenuModel {
public:
    DebugMenuItem selected() const { return static_cast<DebugMenuItem>(selected_); }
    void move(int amount);
    void select(int row);
    DebugMenuItem activate() const { return selected(); }

private:
    std::size_t selected_ = 0;
};

std::string_view debug_menu_label(DebugMenuItem item);
std::string_view debug_menu_hotkey(DebugMenuItem item);

// A bounded UTF-8 annotation buffer. Events are applied in poll order, so text
// insertion and cursor edits never collapse into unordered action flags.
class AnnotationEditor {
public:
    explicit AnnotationEditor(std::size_t max_bytes = 4096);

    void clear();
    void insert(std::string_view text);
    EditorResult apply(const EditorEvent& event);
    const std::string& text() const { return text_; }
    std::size_t size() const { return text_.size(); }
    std::size_t cursor() const { return cursor_; }

private:
    std::size_t previous_boundary(std::size_t at) const;
    std::size_t next_boundary(std::size_t at) const;
    void move_vertical(bool down);
    std::size_t max_bytes_;
    std::string text_;
    std::size_t cursor_ = 0;
    std::size_t preferred_column_ = 0;
    bool preferred_column_valid_ = false;
};

} // namespace dsr
