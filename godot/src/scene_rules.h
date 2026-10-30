#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Owner-thread counterpart of the presentation rules. The state is constant
// size apart from the current cartridge basename, never a queue of frames.
class SceneRules {
public:
    struct Result {
        const char* scene;
        int main_screen;
        bool top_dark;
        std::uint64_t main_key_generation;
    };

    Result advance(std::int64_t frame, int machine_scene, std::string_view path,
                   std::int64_t read_frame, const std::uint32_t* top,
                   const std::uint32_t* bottom) {
        if (frame < 0) {
            *this = SceneRules{};
            return {"other", -1, false, 0};
        }
        if (last_frame_ >= 0 && frame < last_frame_) *this = SceneRules{};
        last_frame_ = frame;
        if (machine_scene == 1) {
            battle_seen_ = true;
            if (hint_ == IntroMap) hint_ = None;
        }
        if (read_frame >= 0 && read_frame <= frame && read_frame > observed_file_frame_) {
            observed_file_frame_ = read_frame;
            if (field_map(path) || town_map(path)) {
                hint_ = field_map(path) ? Field : Town;
                hint_frame_ = read_frame;
            } else if (battle_map(path)) {
                hint_ = None;
            } else if (intro_map(path)) {
                hint_ = battle_seen_ ? None : IntroMap;
                hint_frame_ = read_frame;
                field_context_ = Other;
            } else if (path == title_movie || path == title_file) {
                hint_ = TitleHint;
                hint_frame_ = read_frame;
                field_context_ = Other;
            } else if (starts(path, "data/movie/") && ends(path, ".mods")) {
                hint_ = IntroMovie;
                hint_frame_ = read_frame;
                field_context_ = Other;
            } else if (starts(path, "data/map/") && !starts(path, "data/map/ats_")) {
                hint_ = None;
                field_context_ = Other;
            }
        }
        Scene next = Other;
        switch (machine_scene) {
        case 1: next = Battle; break;
        case 2: next = CharacterCreation; break;
        case 3: next = Intro; break;
        case 4: next = Title; break;
        }
        if (next == Other) {
            const auto age = frame - hint_frame_;
            if (hint_ == Field || hint_ == Town) next = hint_ == Field ? FieldScene : TownScene;
            else if (hint_ == TitleHint && (path == title_file || age < 900)) next = Title;
            else if (hint_ == IntroMap || (hint_ == IntroMovie && age < 900)) next = Intro;
            else if (field_context_ != Other && battle_map(path)) next = field_context_;
        } else if (next != Battle) {
            hint_ = None;
            field_context_ = Other;
        }
        if (next != candidate_) { candidate_ = next; candidate_since_ = frame; }
        if (next != scene_ && frame - candidate_since_ >= 6) scene_ = next;
        if (scene_ == next && (next == FieldScene || next == TownScene)) field_context_ = next;

        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of('/');
        const auto basename = dot != path.npos && (slash == path.npos || dot > slash) ? path.substr(0, dot) : path;
        if (!main_key_set_ || scene_ != main_key_scene_ || basename != std::string_view(main_key_path_)) {
            main_key_set_ = true;
            main_key_scene_ = scene_;
            if (basename.empty()) main_key_path_.clear();
            else main_key_path_.assign(basename.data(), basename.size());
            ++main_key_generation_;
        }
        const bool top_dark = (scene_ == Intro || scene_ == FieldScene) && image_is_dark(top);
        const bool map_bottom = scene_ == Other && bottom_map(path);
        const bool dark_bottom = top_dark && !image_is_dark(bottom);
        const bool wanted = map_bottom || dark_bottom;
        if (wanted != main_candidate_) { main_candidate_ = wanted; main_since_ = frame; }
        if (main_since_ < 0) main_since_ = frame;
        const int delay = dark_bottom && !map_bottom ? 90 : 6;
        if (wanted != main_bottom_ && frame - main_since_ >= delay) main_bottom_ = wanted;
        return {scene_name(scene_), main_bottom_ ? 1 : -1, top_dark, main_key_generation_};
    }

    // get_pixel() sees RGB8 channels normalized by 255; > 0.04 means >= 11.
    static bool image_is_dark(const std::uint32_t* pixels) {
        if (!pixels) return false;
        for (int y = 12; y < 192; y += 24)
            for (int x = 16; x < 256; x += 32) {
                const auto pixel = pixels[y * 256 + x];
                if (((pixel >> 16) & 255) >= 11 || ((pixel >> 8) & 255) >= 11 || (pixel & 255) >= 11)
                    return false;
            }
        return true;
    }
private:
    enum Scene { Other, Battle, CharacterCreation, Intro, Title, FieldScene, TownScene };
    enum Hint { None, Field, Town, IntroMap, IntroMovie, TitleHint };
    static constexpr std::string_view title_movie = "data/movie/TD_nouhin_DS_form.mods";
    static constexpr std::string_view title_file = "data/menu/bg_title_under.pac";
    static bool starts(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
    static bool ends(std::string_view s, std::string_view suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }
    static bool pair(std::string_view path, std::string_view base) {
        return starts(path, base) && (path.substr(base.size()) == ".ambl" || path.substr(base.size()) == ".amdj");
    }
    static bool field_map(std::string_view p) { return pair(p, "data/map/F01") || pair(p, "data/map/F01M01"); }
    static bool town_map(std::string_view p) { return pair(p, "data/map/M01M06") || pair(p, "data/map/M01"); }
    static bool intro_map(std::string_view p) { return pair(p, "data/map/M01M12"); }
    static bool battle_map(std::string_view p) { return pair(p, "data/map/B01M16"); }
    static bool bottom_map(std::string_view p) { return pair(p, "data/map/X01M05"); }
    static const char* scene_name(Scene s) {
        switch (s) {
        case Battle: return "battle";
        case CharacterCreation: return "character_creation";
        case Intro: return "intro";
        case Title: return "title";
        case FieldScene: return "field";
        case TownScene: return "town";
        default: return "other";
        }
    }
    Scene scene_ = Other, candidate_ = Other, field_context_ = Other, main_key_scene_ = Other;
    Hint hint_ = None;
    std::int64_t candidate_since_ = -1, last_frame_ = -1, observed_file_frame_ = -1;
    std::int64_t hint_frame_ = -1, main_since_ = -1;
    bool battle_seen_ = false, main_key_set_ = false, main_candidate_ = false, main_bottom_ = false;
    std::string main_key_path_;
    std::uint64_t main_key_generation_ = 0;
};
