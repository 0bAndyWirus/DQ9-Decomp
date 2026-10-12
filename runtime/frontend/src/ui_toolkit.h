#pragma once

#include "ui_logic.h"

#include <SDL.h>

#include <string_view>

#include <filesystem>


namespace dsr::ui_detail {
bool save_renderer_png(SDL_Renderer* renderer, const std::filesystem::path& path, int width, int height);

struct UiColor { Uint8 r, g, b, a; };
struct UiRect { int x, y, w, h; };

class UiCanvas {
public:
    explicit UiCanvas(SDL_Renderer* renderer) : renderer_(renderer) {}
    void clear(UiColor color);
    void fill(UiRect rect, UiColor color);
    void outline(UiRect rect, UiColor color);
    void text(int x, int y, std::string_view value, UiColor color, int scale = 2);
    int text_width(std::string_view value, int scale = 2) const;
    void button(UiRect rect, std::string_view label, bool focused, bool enabled = true);
    void panel(UiRect rect);
    SDL_Renderer* renderer() const { return renderer_; }
private:
    SDL_Renderer* renderer_;
};

} // namespace dsr::ui_detail
