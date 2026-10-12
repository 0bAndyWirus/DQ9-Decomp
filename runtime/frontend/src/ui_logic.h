#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dsr::ui_detail {

// A stable focus cursor for keyboard, mouse and controller driven screens.
class FocusModel {
public:
    explicit FocusModel(std::size_t count = 0, std::size_t selected = 0);
    void reset(std::size_t count, std::size_t selected = 0);
    void move(int delta);
    std::size_t selected() const { return selected_; }
    std::size_t count() const { return count_; }
private:
    std::size_t count_ = 0;
    std::size_t selected_ = 0;
};

// Bounded UTF-8 byte buffer. Cursor edits preserve complete code points; the
// SDL bitmap font renders unsupported glyphs as a visible replacement glyph.
class TextBuffer {
public:
    explicit TextBuffer(std::size_t max_bytes = 1024);
    void set(std::string_view value);
    void insert(std::string_view value);
    void backspace();
    void erase_forward();
    void move_cursor(int delta);
    void home();
    void end();
    const std::string& value() const { return value_; }
    std::size_t cursor() const { return cursor_; }
private:
    std::size_t previous_boundary(std::size_t at) const;
    std::size_t next_boundary(std::size_t at) const;
    std::size_t max_bytes_;
    std::string value_;
    std::size_t cursor_ = 0;
};

// Fit single-line text to a glyph-cell budget without splitting UTF-8 code points.
std::string elide_middle(std::string_view text, std::size_t max_bytes);

// Timestamp is YYYYmmdd-HHMMSS and sequence is one-based. The result is safe
// as one path component and remains deterministic for unit tests.
std::string bundle_component(std::string_view timestamp, unsigned sequence, std::string_view comment);
std::string config_escape(std::string_view value);
std::string config_unescape(std::string_view value);

} // namespace dsr::ui_detail
