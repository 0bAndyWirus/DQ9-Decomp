# Linux -> Windows x64 cross compilation without a system MinGW installation.
# Install .venv/bin/python -m pip install ziglang, then pass this toolchain to CMake.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(DSR_ZIG_TOOLCHAIN ON CACHE INTERNAL "Using Zig's bundled Windows sysroot")

if(CMAKE_HOST_WIN32)
  set(_DSR_ZIG_WRAPPER_EXT cmd)
else()
  set(_DSR_ZIG_WRAPPER_EXT sh)
endif()
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cc.${_DSR_ZIG_WRAPPER_EXT}")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cxx.${_DSR_ZIG_WRAPPER_EXT}")
set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.${_DSR_ZIG_WRAPPER_EXT}")
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.${_DSR_ZIG_WRAPPER_EXT}")
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")

# CMake's executable link rules otherwise request -lgcc/-lgcc_s. Zig supplies
# compiler-rt and a static libc++ instead, including Windows thread support.
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "Zig supplies its runtime" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "Zig supplies its runtime" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Explicit SDK prefixes (SDL2_DIR) are searched; never fall back to Linux SDL.
if(DEFINED DQ9_WINDOWS_SDL_ROOT)
  list(APPEND CMAKE_FIND_ROOT_PATH "${DQ9_WINDOWS_SDL_ROOT}")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DQ9_WINDOWS_SDL_ROOT)

find_program(DSR_WINE_EXECUTABLE NAMES wine64 wine)
if(DSR_WINE_EXECUTABLE AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${DSR_WINE_EXECUTABLE}" CACHE STRING
      "Optional Wine runner for Windows runtime tests")
endif()
