#include "ui_screenshot_capture.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ui_screenshot_capture_test OUTPUT_DIR\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    if (!dsr::ui_detail::capture_launcher_screenshots(directory) ||
        !dsr::ui_detail::capture_debug_screenshots(directory)) {
        std::cerr << "failed to render one or more offscreen UI screenshots\n";
        return 1;
    }
    std::cout << "offscreen launcher and debug screenshots written to " << directory << '\n';
    return 0;
}
