#include "scene_rules.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace {
constexpr auto intro = "data/map/M01M12.ambl";
constexpr auto intro_companion = "data/map/M01M12.amdj";
constexpr auto title = "data/menu/bg_title_under.pac";
constexpr auto title_movie = "data/movie/TD_nouhin_DS_form.mods";
constexpr auto movie = "data/movie/opening.mods";
constexpr auto town = "data/map/M01.amdj";
constexpr auto field = "data/map/F01M01.amdj";
constexpr auto battle = "data/map/B01M16.amdj";
constexpr auto observatory = "data/map/X01M05.amdj";

void require(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "scene_rules: %s\n", label);
        std::exit(1);
    }
}

SceneRules::Result step(SceneRules& rules, std::int64_t frame, int machine = 0,
                        std::string_view path = {}, std::int64_t read_frame = -1,
                        const std::uint32_t* top = nullptr, const std::uint32_t* bottom = nullptr) {
    return rules.advance(frame, machine, path, read_frame, top, bottom);
}
void scene(SceneRules::Result result, std::string_view expected, const char* label) {
    if (result.scene != expected) {
        std::fprintf(stderr, "scene_rules: %s: expected %.*s, got %s\n", label,
                     static_cast<int>(expected.size()), expected.data(), result.scene);
        std::exit(1);
    }
}

void test_machine_and_hysteresis() {
    for (const auto& [machine, expected] : std::array<std::pair<int, const char*>, 6>{{
             {0, "other"}, {1, "battle"}, {2, "character_creation"},
             {3, "intro"}, {4, "title"}, {99, "other"}}}) {
        SceneRules rules;
        scene(step(rules, 0, machine), "other", "machine entry pending");
        scene(step(rules, 6, machine), expected, "machine entry confirmed");
    }
    SceneRules rules;
    scene(step(rules, 0, 0, intro, 0), "other", "intro pending");
    scene(step(rules, 0, 0, intro, 0), "other", "duplicate does not progress");
    scene(step(rules, 5, 0, intro, 0), "other", "before hysteresis boundary");
    scene(step(rules, 6, 0, intro, 0), "intro", "entry confirmed");
    scene(step(rules, 7, 1, intro, 0), "intro", "battle pending");
    scene(step(rules, 8, 3, intro, 0), "intro", "transient rejected");
    scene(step(rules, 9, 1, intro, 0), "intro", "battle restarted");
    scene(step(rules, 14, 1, intro, 0), "intro", "restarted boundary");
    scene(step(rules, 15, 1, intro, 0), "battle", "battle confirmed");
    scene(step(rules, 1), "other", "rewind resets scene");
    scene(step(rules, -1), "other", "invalid frame resets scene");
    scene(step(rules, 2, 0, intro, 2), "other", "reset intro pending");
    scene(step(rules, 8, 0, intro, 2), "intro", "reset permits intro");
}

void test_title_and_movies() {
    SceneRules rules;
    scene(step(rules, 10, 0, title, 10), "other", "title pending");
    scene(step(rules, 16, 0, title, 10), "title", "title confirmed");
    scene(step(rules, 1000, 0, title, 10), "title", "title file persists");
    scene(step(rules, 1001, 0, intro, 1001), "title", "intro after title pending");
    scene(step(rules, 1007, 0, intro, 1001), "intro", "intro after title confirmed");

    SceneRules attract;
    scene(step(attract, 100, 0, title_movie, 100), "other", "attract pending");
    scene(step(attract, 106, 0, title_movie, 100), "title", "attract is title");
    scene(step(attract, 999, 0, title_movie, 100), "title", "attract before expiry");
    scene(step(attract, 1000, 0, title_movie, 100), "title", "attract expiry pending");
    scene(step(attract, 1006, 0, title_movie, 100), "other", "attract expired");
    scene(step(attract, 1007, 4, title_movie, 100), "other", "machine title pending");
    scene(step(attract, 1013, 4, title_movie, 100), "title", "machine title overrides expiry");

    SceneRules opening;
    scene(step(opening, 10, 0, movie, 10), "other", "opening movie pending");
    scene(step(opening, 16, 0, movie, 10), "intro", "opening movie is intro");
    scene(step(opening, 910, 0, movie, 10), "intro", "movie expiry pending");
    scene(step(opening, 916, 0, movie, 10), "other", "movie expired");
    scene(step(opening, 917, 0, "data/movie/not.mods.bak", 917), "other", "movie suffix exact");
    scene(step(opening, 930, 0, intro, 931), "other", "future read ignored");
    scene(step(opening, 931, 0, intro, 931), "other", "future read becomes eligible");
    scene(step(opening, 937, 0, intro_companion, 931), "intro", "intro companion shares hint");
}

void test_route_and_battle_context() {
    SceneRules rules;
    scene(step(rules, 0, 0, intro, 0), "other", "intro route pending");
    scene(step(rules, 6, 0, intro, 0), "intro", "intro route confirmed");
    scene(step(rules, 10000, 0, intro, 0), "intro", "intro map persists");
    scene(step(rules, 10001, 0, town, 10001), "intro", "town pending");
    scene(step(rules, 10007, 0, town, 10001), "town", "town confirmed");
    scene(step(rules, 10008, 0, field, 10008), "town", "field pending");
    scene(step(rules, 10014, 0, field, 10008), "field", "field confirmed");
    scene(step(rules, 10015, 0, intro, 10000), "field", "older file read ignored");
    scene(step(rules, 12000, 0, field, 10008), "field", "field persists without reads");
    scene(step(rules, 12001, 0, battle, 12001), "field", "battle map retains field");
    scene(step(rules, 12002, 1, battle, 12001), "field", "battle pending");
    scene(step(rules, 12008, 1, battle, 12001), "battle", "battle confirmed");
    scene(step(rules, 12009, 0, battle, 12001), "battle", "field return pending");
    scene(step(rules, 12015, 0, battle, 12001), "field", "field return confirmed");
    scene(step(rules, 12016, 0, "data/map/unknown.amdj", 12016), "field", "unknown pending");
    scene(step(rules, 12022, 0, "data/map/unknown.amdj", 12016), "other", "unknown clears context");
    scene(step(rules, 12023, 0, battle, 12023), "other", "battle map cannot restore cleared context");
    scene(step(rules, 12024, 0, intro, 12024), "other", "post-battle intro map rejected");
    scene(step(rules, 12030, 0, intro, 12024), "other", "post-battle intro stays rejected");
    scene(step(rules, 1, 0, intro, 1), "other", "rewind intro pending");
    scene(step(rules, 7, 0, intro, 1), "intro", "rewind clears battle history");

    SceneRules skipped;
    scene(step(skipped, 10, 1, battle, 10), "other", "unobserved battle pending");
    scene(step(skipped, 11, 0, intro, 11), "other", "even unconfirmed battle blocks intro");
    scene(step(skipped, 17, 0, intro, 11), "other", "post-battle map remains other");
    scene(step(skipped, 18, 0, "data/map/F01M01.ambl", 18), "other", "field companion pending");
    scene(step(skipped, 24, 0, field, 24), "field", "field companion confirms");
}

void test_darkness_and_main_screen() {
    std::array<std::uint32_t, 256 * 192> pixels{};
    require(!SceneRules::image_is_dark(nullptr), "null image is not dark");
    require(SceneRules::image_is_dark(pixels.data()), "black image is dark");
    pixels[0] = 0xffffffff;
    require(SceneRules::image_is_dark(pixels.data()), "unsampled bright pixel ignored");
    const auto sample = 12 * 256 + 16;
    for (int shift : {0, 8, 16}) {
        pixels[sample] = 10u << shift;
        require(SceneRules::image_is_dark(pixels.data()), "channel 10 is dark");
        pixels[sample] = 11u << shift;
        require(!SceneRules::image_is_dark(pixels.data()), "channel 11 is bright");
    }
    pixels[sample] = 0xff000000;
    require(SceneRules::image_is_dark(pixels.data()), "alpha ignored");
    pixels[180 * 256 + 240] = 11;
    require(!SceneRules::image_is_dark(pixels.data()), "last grid sample included");
    pixels[180 * 256 + 240] = 0;
    std::array<std::uint32_t, 256 * 192> bright{};
    bright.fill(0x00ffffff);
    SceneRules rules;
    step(rules, 100, 0, intro, 100, pixels.data(), bright.data());
    auto r = step(rules, 106, 0, intro, 100, pixels.data(), bright.data());
    require(r.top_dark && r.main_screen == -1, "confirmed intro starts dark timer");
    r = step(rules, 195, 0, intro, 100, pixels.data(), bright.data());
    require(r.main_screen == -1, "dark timer before 90 frames");
    r = step(rules, 196, 0, intro, 100, pixels.data(), bright.data());
    require(r.main_screen == 1, "dark timer reaches 90 frames");
    r = step(rules, 197, 0, field, 197, bright.data(), bright.data());
    require(!r.top_dark && r.main_screen == 1, "bright top starts return timer");
    r = step(rules, 203, 0, field, 197, bright.data(), bright.data());
    require(r.main_screen == -1, "bright top returns after six frames");

    SceneRules map;
    r = step(map, 300, 0, observatory, 300, bright.data(), bright.data());
    require(r.main_screen == -1, "bottom map pending");
    r = step(map, 306, 0, observatory, 300, bright.data(), bright.data());
    require(r.main_screen == 1, "bottom map confirmed");
    r = step(map, 307, 1, observatory, 300, bright.data(), bright.data());
    require(r.main_screen == 1, "battle transition retains bottom temporarily");
    r = step(map, 313, 1, observatory, 300, bright.data(), bright.data());
    require(r.main_screen == 1, "battle scene change starts bottom return");
    r = step(map, 319, 1, observatory, 300, bright.data(), bright.data());
    require(r.main_screen == -1, "battle exits bottom map after six frames");
}

void test_coalescing_and_key_generation() {
    SceneRules per_frame, coalesced;
    SceneRules::Result dense{};
    for (int frame = 0; frame <= 100; ++frame)
        dense = step(per_frame, frame, 0, intro, 0);
    step(coalesced, 0, 0, intro, 0);
    const auto sparse = step(coalesced, 100, 0, intro, 0);
    scene(sparse, dense.scene, "coalesced frames reach same stable scene");
    require(sparse.main_screen == dense.main_screen && sparse.main_key_generation == dense.main_key_generation,
            "coalesced frames preserve stable key and screen");

    SceneRules keys;
    auto a = step(keys, 0, 0, intro, 0);
    auto b = step(keys, 1, 0, intro_companion, 1);
    require(a.main_key_generation == 1 && b.main_key_generation == a.main_key_generation,
            "companion extensions share key");
    auto c = step(keys, 2, 0, field, 2);
    auto d = step(keys, 3, 0, intro, 3);
    require(c.main_key_generation == b.main_key_generation + 1 &&
            d.main_key_generation == c.main_key_generation + 1,
            "path key changes and changes back are both counted");
    auto e = step(keys, 9, 0, intro, 3);
    require(e.main_key_generation == d.main_key_generation + 1,
            "confirmed scene change increments key");
    auto f = step(keys, 10, 1, intro, 3);
    auto g = step(keys, 16, 1, intro, 3);
    require(f.main_key_generation == e.main_key_generation &&
            g.main_key_generation == f.main_key_generation + 1,
            "pending scene keeps key until confirmation");
    step(keys, 17, 3, intro, 3);
    auto h = step(keys, 23, 3, intro, 3);
    require(h.main_key_generation == g.main_key_generation + 1,
            "scene changing back increments key again");
    auto reset = step(keys, -1);
    require(reset.main_key_generation == 0, "invalid frame clears key generation");
    require(step(keys, 0).main_key_generation == 1, "new session restarts generation");
}
} // namespace

int main() {
    test_machine_and_hysteresis();
    test_title_and_movies();
    test_route_and_battle_context();
    test_darkness_and_main_screen();
    test_coalescing_and_key_generation();
}
