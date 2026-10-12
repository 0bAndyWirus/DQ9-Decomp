#include <dsr/debug_editor.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace dsr {
namespace {

bool is_continuation(unsigned char byte) { return (byte & 0xc0U) == 0x80U; }

std::size_t codepoint_count(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char byte : text)
        if (!is_continuation(byte)) ++count;
    return count;
}

std::size_t byte_offset(std::string_view text, std::size_t codepoints) {
    std::size_t offset = 0;
    while (offset < text.size() && codepoints != 0) {
        ++offset;
        while (offset < text.size() && is_continuation(static_cast<unsigned char>(text[offset])))
            ++offset;
        --codepoints;
    }
    return offset;
}

} // namespace

void DebugMenuModel::move(int amount) {
    constexpr int count = static_cast<int>(kDebugMenuRowCount);
    const int delta = amount % count;
    const int row = static_cast<int>(selected_);
    const int remainder = row + delta;
    selected_ = static_cast<std::size_t>(remainder < 0 ? remainder + count : remainder >= count ? remainder - count : remainder);
}

void DebugMenuModel::select(int row) {
    constexpr int count = static_cast<int>(kDebugMenuRowCount);
    int normalized = row % count;
    if (normalized < 0) normalized += count;
    selected_ = static_cast<std::size_t>(normalized);
}

std::string_view debug_menu_label(DebugMenuItem item) {
    constexpr std::array<std::string_view, kDebugMenuRowCount> labels = {
        "Pause / Resume", "Step 1 frame", "Slow 0.5x", "Slow 0.25x", "Fast-forward",
        "Add note", "Drop marker", "Quick bundle now", "Frame-time graph", "Audio bar",
        "CPU PC", "Texture slots", "Audio mixer", "Texture inspector", "Screenshot only",
        "Show debug folder path", "Help", "Close",
    };
    const auto index = static_cast<std::size_t>(item);
    return index < labels.size() ? labels[index] : std::string_view{};
}

std::string_view debug_menu_hotkey(DebugMenuItem item) {
    constexpr std::array<std::string_view, kDebugMenuRowCount> hotkeys = {
        "Space / Pause", "F8", "F2", "F2", "F3", "F9 / Ctrl+Shift+N", "F4", "Menu",
        "G", "B", "C", "V", "M", "T", "S", "O", "F1", "Esc / F10",
    };
    const auto index = static_cast<std::size_t>(item);
    return index < hotkeys.size() ? hotkeys[index] : std::string_view{};
}

AnnotationEditor::AnnotationEditor(std::size_t max_bytes) : max_bytes_(max_bytes) {}

void AnnotationEditor::clear() {
    text_.clear();
    cursor_ = 0;
    preferred_column_ = 0;
    preferred_column_valid_ = false;
}
void AnnotationEditor::insert(std::string_view text) {
    while (!text.empty()) {
        EditorEvent event;
        const std::size_t capacity = event.text.size();
        std::size_t amount = std::min(capacity, text.size());
        if (amount < text.size()) {
            while (amount > 0 && is_continuation(static_cast<unsigned char>(text[amount]))) --amount;
        }
        if (amount == 0) return;
        std::memcpy(event.text.data(), text.data(), amount);
        event.text_size = static_cast<std::uint8_t>(amount);
        apply(event);
        text.remove_prefix(amount);
    }
}

std::size_t AnnotationEditor::previous_boundary(std::size_t at) const {
    if (at == 0) return 0;
    --at;
    while (at > 0 && is_continuation(static_cast<unsigned char>(text_[at]))) --at;
    return at;
}

std::size_t AnnotationEditor::next_boundary(std::size_t at) const {
    if (at >= text_.size()) return text_.size();
    ++at;
    while (at < text_.size() && is_continuation(static_cast<unsigned char>(text_[at]))) ++at;
    return at;
}

void AnnotationEditor::move_vertical(bool down) {
    std::size_t line_start = 0;
    if (cursor_ > 0) {
        const std::size_t previous_newline = text_.rfind('\n', cursor_ - 1);
        if (previous_newline != std::string::npos) line_start = previous_newline + 1;
    }
    if (!preferred_column_valid_) {
        preferred_column_ = codepoint_count(std::string_view(text_).substr(line_start, cursor_ - line_start));
        preferred_column_valid_ = true;
    }
    if (down) {
        const std::size_t current_end = text_.find('\n', cursor_);
        if (current_end == std::string::npos) return;
        const std::size_t next_start = current_end + 1;
        const std::size_t next_end = text_.find('\n', next_start);
        const std::size_t end = next_end == std::string::npos ? text_.size() : next_end;
        const std::string_view next_line(text_.data() + next_start, end - next_start);
        cursor_ = next_start + byte_offset(next_line, std::min(preferred_column_, codepoint_count(next_line)));
    } else {
        if (line_start == 0) return;
        const std::size_t previous_end = line_start - 1;
        std::size_t previous_start = 0;
        if (previous_end > 0) {
            const std::size_t previous_newline = text_.rfind('\n', previous_end - 1);
            if (previous_newline != std::string::npos) previous_start = previous_newline + 1;
        }
        const std::string_view previous_line(text_.data() + previous_start, previous_end - previous_start);
        cursor_ = previous_start + byte_offset(previous_line,
            std::min(preferred_column_, codepoint_count(previous_line)));
    }
}

EditorResult AnnotationEditor::apply(const EditorEvent& event) {
    switch (event.type) {
    case EditorEventType::Text: {
        const std::size_t input_size = std::min<std::size_t>(event.text_size, event.text.size());
        const std::size_t room = max_bytes_ > text_.size() ? max_bytes_ - text_.size() : 0;
        std::size_t amount = std::min(room, input_size);
        if (amount < input_size) {
            while (amount > 0 && is_continuation(static_cast<unsigned char>(event.text[amount]))) --amount;
        }
        if (amount != 0) {
            text_.insert(cursor_, event.text.data(), amount);
            cursor_ += amount;
        }
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    }
    case EditorEventType::Backspace:
        if (cursor_ > 0) {
            const std::size_t begin = previous_boundary(cursor_);
            text_.erase(begin, cursor_ - begin);
            cursor_ = begin;
        }
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    case EditorEventType::Delete:
        if (cursor_ < text_.size()) {
            const std::size_t end = next_boundary(cursor_);
            text_.erase(cursor_, end - cursor_);
        }
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    case EditorEventType::Left:
        cursor_ = previous_boundary(cursor_);
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    case EditorEventType::Right:
        cursor_ = next_boundary(cursor_);
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    case EditorEventType::Up:
        move_vertical(false);
        return EditorResult::Continue;
    case EditorEventType::Down:
        move_vertical(true);
        return EditorResult::Continue;
    case EditorEventType::Home: {
        const std::size_t newline = cursor_ == 0 ? std::string::npos : text_.rfind('\n', cursor_ - 1);
        cursor_ = newline == std::string::npos ? 0 : newline + 1;
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    }
    case EditorEventType::End: {
        const std::size_t newline = text_.find('\n', cursor_);
        cursor_ = newline == std::string::npos ? text_.size() : newline;
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    }
    case EditorEventType::Newline:
        if (text_.size() < max_bytes_) {
            text_.insert(cursor_, 1, '\n');
            ++cursor_;
        }
        preferred_column_valid_ = false;
        return EditorResult::Continue;
    case EditorEventType::Save:
        return EditorResult::Save;
    case EditorEventType::Cancel:
        return EditorResult::Cancel;
    }
    return EditorResult::Continue;
}

} // namespace dsr
