cmake_minimum_required(VERSION 3.20)
project(dq9_godot_extension LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DQ9_BUILD_GODOT)
  message(FATAL_ERROR "Configure from the repository root with -DDQ9_BUILD_GODOT=ON")
endif()
if(NOT GODOT_CPP_DIR OR NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Set GODOT_CPP_DIR to an external godot-cpp 4.5 checkout")
endif()
if(NOT TARGET dsr_session_c)
  message(FATAL_ERROR "The root build must define dsr_session_c before godot/")
endif()
# dsr_session is a shared library using the host C++ runtime. Mixing it with
# godot-cpp's default static runtime interposes incompatible locale/vtable data.
# In particular, formatting input history through std::ostringstream crashes.
set(GODOTCPP_USE_STATIC_CPP OFF)
if(WIN32)
  set(GODOTCPP_TARGET template_release CACHE STRING "Godot binding target")
  set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "Expose Godot headers as system headers")
endif()
add_subdirectory("${GODOT_CPP_DIR}" "${CMAKE_BINARY_DIR}/godot-cpp")

add_library(dq9_godot SHARED
  src/dq_machine.cpp
  src/dq_gpu_scene.cpp
  src/register_types.cpp
)
option(DQ9_GPU_SCENE_TESTS "Build the ROM-free alternating-GX Godot test fixture" OFF)
if(DQ9_GPU_SCENE_TESTS)
  target_sources(dq9_godot PRIVATE tests/native/gpu_scene_fixture.cpp)
  target_include_directories(dq9_godot PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_definitions(dq9_godot PRIVATE DQ9_GPU_SCENE_TESTS=1)
  # Owned polygon IDs are only needed by the same-snapshot crack regression.
  target_compile_definitions(dsr_gpu3d PUBLIC DSR_GPU_CAPTURE_IDS=1)
endif()
option(DQ9_AUDIO_TESTS "Enable diagnostic unresampled PCM capture in DqMachine" OFF)
if(DQ9_AUDIO_TESTS)
  target_compile_definitions(dq9_godot PRIVATE DQ9_AUDIO_TESTS=1)
endif()
add_executable(dq9_audio_ring_tests tests/native/audio_ring_test.cpp)
target_include_directories(dq9_audio_ring_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME dq9_audio_ring_tests COMMAND dq9_audio_ring_tests)
target_include_directories(dq9_godot PRIVATE
  "${PROJECT_SOURCE_DIR}/../runtime/include"
  "${PROJECT_SOURCE_DIR}/../runtime/gpu3d/src"
)
target_link_libraries(dq9_godot PRIVATE godot-cpp dsr_session_c dsr_gpu3d)
set_target_properties(dq9_godot PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "dq9_godot"
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
if(WIN32)
  set_target_properties(dq9_godot PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dq9_godot.windows.template_release.x86_64"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PROJECT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${PROJECT_SOURCE_DIR}/bin")
  add_custom_command(TARGET dq9_godot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:dsr_session_c>"
            "$<TARGET_FILE_DIR:dq9_godot>"
    VERBATIM)
endif()
