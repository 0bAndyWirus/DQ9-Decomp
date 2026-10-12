#include "debug_workflow_test.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <utility>

namespace dq9::workflow_test {
namespace {

bool push_event(SDL_Event& event) {
    return SDL_PushEvent(&event) == 1;
}
bool push_key_event(SDL_Scancode key, Uint32 type, bool repeat = false,
                    SDL_Keymod modifiers = KMOD_NONE) {
    SDL_Event event{};
    event.type = type;
    event.key.type = type;
    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = repeat ? 1 : 0;
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
    event.key.keysym.mod = modifiers;
    return push_event(event);
}

bool push_text_event(std::string_view text) {
    if (text.size() >= sizeof(SDL_TextInputEvent::text)) return false;
    SDL_Event event{};
    event.type = SDL_TEXTINPUT;
    event.text.type = SDL_TEXTINPUT;
    std::copy(text.begin(), text.end(), event.text.text);
    event.text.text[text.size()] = '\0';
    return push_event(event);
}


std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) return fields;
        begin = end + 1;
    }
}

bool metadata_value(const std::string& text, std::string_view key, std::uint64_t& value) {
    std::istringstream in(text);
    std::string line;
    const std::string prefix = std::string(key) + ": ";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0) continue;
        try {
            value = std::stoull(line.substr(prefix.size()));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool ppm_matches(const std::filesystem::path& path, const std::vector<std::uint32_t>& expected) {
    if (expected.size() != 256u * 192u) return false;
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    unsigned width = 0, height = 0, maximum = 0;
    if (!(in >> magic >> width >> height >> maximum) || magic != "P6" || width != 256 || height != 192 || maximum != 255)
        return false;
    if (in.get() != '\n') return false;
    std::array<unsigned char, 3> rgb{};
    for (const std::uint32_t pixel : expected) {
        if (!in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()))) return false;
        if (rgb[0] != ((pixel >> 16) & 0xffu) || rgb[1] != ((pixel >> 8) & 0xffu) || rgb[2] != (pixel & 0xffu))
            return false;
    }
    return in.peek() == std::char_traits<char>::eof();
}

} // namespace
bool push_key_event_one(SDL_Scancode key, Uint32 type, bool repeat, SDL_Keymod modifiers) {
    return push_key_event(key, type, repeat, modifiers);
}

bool push_text_event_one(std::string_view text) {
    return push_text_event(text);
}

bool attach_virtual_controller(VirtualController& controller, std::string& error) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        error = std::string("SDL joystick init failed: ") + SDL_GetError();
        return false;
    }
    controller.device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0,
                                                         SDL_CONTROLLER_BUTTON_MAX, 1);
    if (controller.device_index < 0) {
        error = std::string("SDL virtual controller attach failed: ") + SDL_GetError();
        return false;
    }
    char guid[64]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(controller.device_index), guid, sizeof(guid));
    controller.guid = guid;
    static constexpr std::array<const char*, 11> names{
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder"};
    std::ostringstream mapping;
    mapping << controller.guid << ",DQ9 Workflow Virtual";
    for (std::size_t button = 0; button < names.size(); ++button)
        mapping << ',' << names[button] << ":b" << button;
    mapping << ",dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,";
    if (SDL_GameControllerAddMapping(mapping.str().c_str()) < 0) {
        error = std::string("SDL virtual controller mapping failed: ") + SDL_GetError();
        return false;
    }
    if (!SDL_IsGameController(controller.device_index)) {
        error = "SDL did not recognize the virtual device as a GameController";
        return false;
    }
    controller.joystick = SDL_JoystickOpen(controller.device_index);
    if (!controller.joystick) {
        error = std::string("SDL virtual joystick open failed: ") + SDL_GetError();
        return false;
    }
    return true;
#else
    (void)controller;
    error = "SDL 2.0.14 or newer is required for a virtual GameController";
    return false;
#endif
}

void close_virtual_controller(VirtualController& controller) {
    if (controller.joystick) SDL_JoystickClose(controller.joystick);
    controller.joystick = nullptr;
}

bool push_key(SDL_Scancode key) {
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.type = SDL_KEYDOWN;
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
    return push_event(event);
}

bool push_key_tap(SDL_Scancode key, SDL_Keymod modifiers) {
    bool success = true;
    for (const Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
        SDL_Event event{};
        event.type = type;
        event.key.type = type;
        event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.scancode = key;
        event.key.keysym.sym = SDL_GetKeyFromScancode(key);
        event.key.keysym.mod = modifiers;
        if (!push_event(event)) success = false;
    }
    return success;
}

bool push_text(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n' || c == '\r') {
            if (!push_key_tap(SDL_SCANCODE_RETURN)) return false;
            ++i;
            continue;
        }
        std::size_t bytes = 1;
        if ((c & 0xe0) == 0xc0) bytes = 2;
        else if ((c & 0xf0) == 0xe0) bytes = 3;
        else if ((c & 0xf8) == 0xf0) bytes = 4;
        bytes = std::min(bytes, text.size() - i);
        const std::string_view character = text.substr(i, bytes);
        if (c >= 0x80) {
            if (!push_text_event(character)) return false;
            i += bytes;
            continue;
        }
        SDL_Keycode keycode = static_cast<SDL_Keycode>(c);
        SDL_Keymod modifiers = KMOD_NONE;
        if (c >= 'A' && c <= 'Z') {
            keycode = static_cast<SDL_Keycode>(c + ('a' - 'A'));
            modifiers = KMOD_SHIFT;
        }
        const SDL_Scancode scancode = SDL_GetScancodeFromKey(keycode);
        if (scancode == SDL_SCANCODE_UNKNOWN) {
            if (!push_text_event(character)) return false;
            ++i;
            continue;
        }
        if (!push_key_event(scancode, SDL_KEYDOWN, false, modifiers) ||
            !push_text_event(character) ||
            !push_key_event(scancode, SDL_KEYUP, false, modifiers))
            return false;
        ++i;
    }
    return true;
}

bool push_repeated_text_key(SDL_Scancode key, std::string_view text) {
    return push_key_event(key, SDL_KEYDOWN) &&
           push_text_event(text) &&
           push_key_event(key, SDL_KEYDOWN, true) &&
           push_text_event(text) &&
           push_key_event(key, SDL_KEYUP);
}

bool set_controller_button(VirtualController& controller, SDL_GameControllerButton button, bool down) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (!controller.joystick || controller.device_index < 0) return false;
    const int raw_button = static_cast<int>(button);
    const bool hat_button = button >= SDL_CONTROLLER_BUTTON_DPAD_UP && button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    if (hat_button) {
        Uint8 value = SDL_HAT_CENTERED;
        if (down) {
            switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: value = SDL_HAT_UP; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: value = SDL_HAT_DOWN; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: value = SDL_HAT_LEFT; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: value = SDL_HAT_RIGHT; break;
            default: break;
            }
        }
        if (SDL_JoystickSetVirtualHat(controller.joystick, 0, value) != 0) return false;
    } else if (SDL_JoystickSetVirtualButton(controller.joystick, raw_button, down ? 1 : 0) != 0) return false;
    SDL_JoystickUpdate();
    SDL_GameController* gamecontroller = SDL_GameControllerOpen(controller.device_index);
    if (!gamecontroller) return false;
    const bool observed = SDL_GameControllerGetButton(gamecontroller, button) != 0;
    if (observed != down) {
        char* mapping = SDL_GameControllerMapping(gamecontroller);
        std::fprintf(stderr, "[workflow-test] mapping=%s\n", mapping ? mapping : "(null)");
        SDL_free(mapping);
        std::fprintf(stderr, "[workflow-test] virtual button %d state was %d, expected %d\n",
                     raw_button, observed ? 1 : 0, down ? 1 : 0);
        SDL_GameControllerClose(gamecontroller);
        return false;
    }
    SDL_GameControllerClose(gamecontroller);
    SDL_Event event{};
    event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.cbutton.type = event.type;
    event.cbutton.timestamp = SDL_GetTicks();
    event.cbutton.which = SDL_JoystickInstanceID(controller.joystick);
    event.cbutton.button = static_cast<Uint8>(button);
    event.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
    return push_event(event);
#else
    (void)controller;
    (void)button;
    (void)down;
    return false;
#endif
}

bool verify_latest_bundle(const std::filesystem::path& debug_root, std::string_view expected_comment,
                          std::uint64_t expected_frame, std::uint64_t expected_ticks,
                          const std::vector<std::uint32_t>& expected_top,
                          const std::vector<std::uint32_t>& expected_bottom,
                          bool require_workflow_controller, std::string& error) {
    std::ifstream index(debug_root / "index.tsv", std::ios::binary);
    if (!index) { error = "debug index.tsv is missing"; return false; }
    std::string line, last;
    while (std::getline(index, line)) if (!line.empty()) last = line;
    const auto fields = split_tabs(last);
    if (fields.size() != 4) { error = "last index.tsv row does not have four fields"; return false; }
    std::string preview(expected_comment.substr(0, 80));
    for (char& c : preview) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (fields[2] != preview) { error = "index.tsv comment does not match the note"; return false; }
    try {
        if (std::stoull(fields[3]) != expected_frame) { error = "index.tsv frame does not match the paused frame"; return false; }
    } catch (...) {
        error = "index.tsv frame is not numeric";
        return false;
    }
    const std::filesystem::path bundle = debug_root / fields[1];
    const std::string comment = read_file(bundle / "comment.txt");
    const std::size_t note_start = comment.starts_with("audio_debug: ") ? comment.find('\n') + 1 : 0;
    const std::string_view metadata_marker = "\ntimestamp: ";
    const std::size_t metadata_start = comment.find(metadata_marker);
    if (metadata_start == std::string::npos || note_start > metadata_start ||
        std::string_view(comment.data() + note_start, metadata_start - note_start) != expected_comment) {
        error = "comment.txt user note does not match exactly";
        return false;
    }
    std::uint64_t saved_frame = 0, saved_ticks = 0;
    if (!metadata_value(comment, "frame", saved_frame) || saved_frame != expected_frame) {
        error = "comment.txt frame metadata does not match";
        return false;
    }
    if (!metadata_value(comment, "emulated_ticks", saved_ticks) || saved_ticks != expected_ticks) {
        error = "comment.txt emulated_ticks metadata does not match";
        return false;
    }
    if (comment.find("timestamp: ") == std::string::npos) { error = "comment.txt timestamp metadata is missing"; return false; }
    if (!ppm_matches(bundle / "screenshot_top.ppm", expected_top)) {
        error = "screenshot_top.ppm does not match the paused top framebuffer";
        return false;
    }
    if (!ppm_matches(bundle / "screenshot_bottom.ppm", expected_bottom)) {
        error = "screenshot_bottom.ppm does not match the paused bottom framebuffer";
        return false;
    }
    if (require_workflow_controller && read_file(bundle / "env.txt").find("controller_name: DQ9 Workflow Virtual") == std::string::npos) {
        error = "controller bundle did not record the synthetic GameController";
        return false;
    }
    return true;
}

} // namespace dq9::workflow_test
