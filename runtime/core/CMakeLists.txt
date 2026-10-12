add_library(dsr_core STATIC
  src/abi_config.cpp
  src/machine.cpp
  src/render_frame.cpp
  src/cpu.cpp
  src/bus.cpp
  src/io.cpp
  src/timing.cpp
  src/scheduler.cpp
  src/runtime.cpp
  src/swi.cpp
  src/thread.cpp
  src/fiber.cpp
)
if(NOT WIN32)
  enable_language(ASM)
  target_sources(dsr_core PRIVATE src/fiber_context.S)
endif()
target_link_libraries(dsr_core PUBLIC
  dsr_headers
  dsr_gpu2d
  dsr_gpu3d
  dsr_spu
  dsr_periph
)
target_include_directories(dsr_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(dsr_core_tests
  tests/core_tests.cpp
  tests/thread_tests.cpp
  tests/abi_config_tests.cpp
)
target_link_libraries(dsr_core_tests PRIVATE dsr_core)
target_include_directories(dsr_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME dsr_core_tests COMMAND dsr_core_tests)
add_executable(dsr_swi_diff_tests tests/swi_diff_tests.cpp)
target_link_libraries(dsr_swi_diff_tests PRIVATE dsr_core)
target_include_directories(dsr_swi_diff_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(_dsr_bios_oracle "${DQ9_BIOS_REFERENCE_SOURCE}")
if(WIN32 AND CMAKE_CROSSCOMPILING AND _dsr_bios_oracle)
  # Wine's default Z: mapping exposes a caller-supplied host file.
  set(_dsr_bios_oracle "Z:${_dsr_bios_oracle}")
endif()
target_compile_definitions(dsr_swi_diff_tests PRIVATE
  DSR_BIOS_REFERENCE_SOURCE="${_dsr_bios_oracle}")
add_test(NAME dsr_swi_diff_tests COMMAND dsr_swi_diff_tests)

add_executable(dsr_render_frame_tests tests/render_frame_tests.cpp)
target_link_libraries(dsr_render_frame_tests PRIVATE dsr_core)
add_test(NAME dsr_render_frame_tests COMMAND dsr_render_frame_tests)

add_executable(dsr_render_timing_tests tests/render_timing_tests.cpp)
target_link_libraries(dsr_render_timing_tests PRIVATE dsr_core)
add_test(NAME dsr_render_timing_tests COMMAND dsr_render_timing_tests)
