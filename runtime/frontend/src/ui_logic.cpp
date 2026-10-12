#include "ui_logic.h"

#include <algorithm>
#include <cctype>

namespace dsr::ui_detail {

FocusModel::FocusModel(std::size_t count, std::size_t selected) { reset(count, selected); }
void FocusModel::reset(std::size_t count, std::size_t selected) {
    count_ = count;
    selected_ = count == 0 ? 0 : std::min(selected, count - 1);
}
void FocusModel::move(int delta) {
    if (count_ == 0 || delta == 0) return;
    const auto amount = static_cast<std::size_t>(delta > 0 ? delta : -static_cast<long long>(delta)) % count_;
    selected_ = delta > 0 ? (selected_ + amount) % count_ : (selected_ + count_ - amount) % count_;
}

TextBuffer::TextBuffer(std::size_t max_bytes) : max_bytes_(max_bytes) {}
std::size_t TextBuffer::previous_boundary(std::size_t at) const {
    if (at == 0) return 0;
    --at;
    while (at > 0 && (static_cast<unsigned char>(value_[at]) & 0xc0U) == 0x80U) --at;
    return at;
}
std::size_t TextBuffer::next_boundary(std::size_t at) const {
    if (at >= value_.size()) return value_.size();
    ++at;
    while (at < value_.size() && (static_cast<unsigned char>(value_[at]) & 0xc0U) == 0x80U) ++at;
    return at;
}
void TextBuffer::set(std::string_view value) {
    value_.clear();
    cursor_ = 0;
    insert(value);
}
void TextBuffer::insert(std::string_view value) {
    const std::size_t room = max_bytes_ > value_.size() ? max_bytes_ - value_.size() : 0;
    const std::size_t amount = std::min(room, value.size());
    if (amount == 0) return;
    std::size_t valid = amount;
    if (amount < value.size()) {
        while (valid > 0 && (static_cast<unsigned char>(value[valid]) & 0xc0U) == 0x80U) --valid;
    }
    if (valid == 0) return;
    value_.insert(cursor_, value.data(), valid);
    cursor_ += valid;
}
void TextBuffer::backspace() {
    if (cursor_ == 0) return;
    const std::size_t begin = previous_boundary(cursor_);
    value_.erase(begin, cursor_ - begin);
    cursor_ = begin;
}
void TextBuffer::erase_forward() {
    if (cursor_ >= value_.size()) return;
    const std::size_t finish = next_boundary(cursor_);
    value_.erase(cursor_, finish - cursor_);
}
void TextBuffer::move_cursor(int delta) {
    while (delta < 0 && cursor_ != 0) { cursor_ = previous_boundary(cursor_); ++delta; }
    while (delta > 0 && cursor_ < value_.size()) { cursor_ = next_boundary(cursor_); --delta; }
}
void TextBuffer::home() { cursor_ = 0; }
void TextBuffer::end() { cursor_ = value_.size(); }

std::string elide_middle(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    if (max_bytes <= 3) return std::string(max_bytes, '.');
    std::size_t prefix = (max_bytes - 3) / 2;
    std::size_t suffix_start = text.size() - (max_bytes - 3 - prefix);
    while (prefix > 0 && (static_cast<unsigned char>(text[prefix]) & 0xc0U) == 0x80U) --prefix;
    while (suffix_start < text.size() &&
           (static_cast<unsigned char>(text[suffix_start]) & 0xc0U) == 0x80U) ++suffix_start;
    std::string result;
    result.reserve(prefix + 3 + text.size() - suffix_start);
    result.append(text.substr(0, prefix));
    result.append("...");
    result.append(text.substr(suffix_start));
    return result;
}

std::string bundle_component(std::string_view timestamp, unsigned sequence, std::string_view comment) {
    std::string slug;
    slug.reserve(48);
    bool separator = false;
    for (unsigned char ch : comment) {
        if (std::isalnum(ch)) {
            if (separator && !slug.empty() && slug.back() != '-') slug.push_back('-');
            separator = false;
            if (slug.size() < 40) slug.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            separator = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "note";
    return std::string(timestamp) + "-" + std::to_string(sequence) + "-" + slug;
}

std::string config_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (c == '%' || c == '\n' || c == '\r') {
            static constexpr char hex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        } else out.push_back(static_cast<char>(c));
    }
    return out;
}
std::string config_unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex(value[i + 1]), lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) { out.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue; }
        }
        out.push_back(value[i]);
    }
    return out;
}

} // namespace dsr::ui_detail
