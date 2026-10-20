# The game executable links a separately supplied generated-code directory
# (never committed) with the native runtime.
if(NOT DQ9_GENERATED_DIR)
  message(STATUS "DQ9_GENERATED_DIR not set: skipping the dq9 game executable")
  return()
endif()

foreach(cpu arm9 arm7)
  if(NOT EXISTS ${DQ9_GENERATED_DIR}/${cpu}/fn_table.cpp)
    message(FATAL_ERROR "Missing ${DQ9_GENERATED_DIR}/${cpu}/fn_table.cpp; set DQ9_GENERATED_DIR to a separately supplied generated-code directory")
  endif()
  file(GLOB gen_${cpu}_sources CONFIGURE_DEPENDS ${DQ9_GENERATED_DIR}/${cpu}/*.cpp)
  add_library(dq9_gen_${cpu} STATIC ${gen_${cpu}_sources})
  target_link_libraries(dq9_gen_${cpu} PRIVATE dsr_headers)
  target_include_directories(dq9_gen_${cpu} PRIVATE ${DQ9_GENERATED_DIR}/${cpu})
  # Generated code is goto-heavy and huge; -O2 gives the best runtime/compile
  # trade-off, and the warnings are not actionable in generated sources.
  if(MSVC)
    # MSVC does not enable strict-aliasing optimizations.
    target_compile_options(dq9_gen_${cpu} PRIVATE /O2 /w /bigobj)
  else()
    target_compile_options(dq9_gen_${cpu} PRIVATE -O2 -w -fno-strict-aliasing)
  endif()
  set_target_properties(dq9_gen_${cpu} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()
target_link_libraries(dsr_session_c PRIVATE dq9_gen_arm9 dq9_gen_arm7)
add_executable(dsr_renderframe_dump "${PROJECT_SOURCE_DIR}/tools/dsr_renderframe_dump.cpp")
target_link_libraries(dsr_renderframe_dump PRIVATE dsr_session dq9_gen_arm9 dq9_gen_arm7)
target_link_libraries(dsr_session_tests PRIVATE dq9_gen_arm9 dq9_gen_arm7)
if(NOT DQ9_BUILD_SDL)
  return()
endif()

find_package(Threads REQUIRED)
add_executable(dq9 main.cpp debug_tool.cpp debug_workflow_test.cpp)
if(WIN32)
  set_target_properties(dq9 PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(NOT WIN32)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(DQ9_SDL2 QUIET IMPORTED_TARGET sdl2)
  endif()
endif()
if(TARGET PkgConfig::DQ9_SDL2)
  set(DQ9_SDL2_TARGET PkgConfig::DQ9_SDL2)
else()
  find_package(SDL2 REQUIRED)
  set(DQ9_SDL2_TARGET SDL2::SDL2)
endif()
add_test(NAME debug_workflow_selftest COMMAND dq9 --debug-workflow-selftest)
set_tests_properties(debug_workflow_selftest PROPERTIES
  SKIP_RETURN_CODE 77
  TIMEOUT 180
  WORKING_DIRECTORY "${DQ9_TEST_DATA_ROOT}"
  ENVIRONMENT "DQ9_ROM=${DQ9_TEST_ROM};DQ9_WORKFLOW_SAVE=${DQ9_TEST_SAVE};DQ9_WORKFLOW_INPUT=${DQ9_TEST_INPUT};SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy;XDG_DATA_HOME=${PROJECT_BINARY_DIR}/debug-workflow-data;XDG_CONFIG_HOME=${PROJECT_BINARY_DIR}/debug-workflow-config")
if(WIN32)
  set_property(TEST debug_workflow_selftest APPEND PROPERTY ENVIRONMENT
    "APPDATA=${PROJECT_BINARY_DIR}/debug-workflow-data"
    "LOCALAPPDATA=${PROJECT_BINARY_DIR}/debug-workflow-local")
endif()
# Cross-built PE tests use CMAKE_CROSSCOMPILING_EMULATOR, but these Python
# integration tests need a native interpreter that can load the session DLL.
if(NOT CMAKE_CROSSCOMPILING)
if(WIN32 AND EXISTS "${PROJECT_SOURCE_DIR}/.venv/Scripts/python.exe")
  set(DQ9_TEST_PYTHON "${PROJECT_SOURCE_DIR}/.venv/Scripts/python.exe")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/.venv/bin/python")
  set(DQ9_TEST_PYTHON "${PROJECT_SOURCE_DIR}/.venv/bin/python")
else()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(DQ9_TEST_PYTHON "${Python3_EXECUTABLE}")
endif()
add_test(NAME debug_replay_parity COMMAND "${DQ9_TEST_PYTHON}"
  "${PROJECT_SOURCE_DIR}/tests/debug_replay_parity.py"
  --dq9 "$<TARGET_FILE:dq9>"
  --rom "${DQ9_TEST_ROM}"
  --abi-config "${DQ9_GENERATED_DIR}/abi.ini"
  --backup-type eeprom64k
  --input "${DQ9_TEST_INPUT}"
  --work-dir "${PROJECT_BINARY_DIR}/debug-replay-parity")
set_tests_properties(debug_replay_parity PROPERTIES
  SKIP_RETURN_CODE 77
  TIMEOUT 240
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME dsr_session_ctypes_smoke COMMAND "${DQ9_TEST_PYTHON}"
  "${PROJECT_SOURCE_DIR}/tools/session_ctypes_smoke.py"
  --library "$<TARGET_FILE:dsr_session_c>"
  --dq9 "$<TARGET_FILE:dq9>"
  --rom "${DQ9_TEST_ROM}"
  --save "${DQ9_TEST_SAVE}"
  --script "${DQ9_TEST_INPUT}"
  --frames 300)
endif()
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE DQ9_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT DQ9_BUILD_ID)
  set(DQ9_BUILD_ID "unknown")
endif()
target_compile_definitions(dq9 PRIVATE DQ9_BUILD_ID="${DQ9_BUILD_ID}")
target_link_libraries(dq9 PRIVATE dsr_session dsr_core dsr_frontend dq9_gen_arm9 dq9_gen_arm7 Threads::Threads ${DQ9_SDL2_TARGET})
dsr_target_compile_options(dq9 -Wall -Wextra)
if(WIN32)
  if(NOT DSR_SDL2_RUNTIME_DLL OR NOT EXISTS "${DSR_SDL2_RUNTIME_DLL}")
    message(FATAL_ERROR "Set DSR_SDL2_RUNTIME_DLL to the SDL2.dll that accompanies the Windows build")
  endif()
  install(TARGETS dq9 RUNTIME DESTINATION .)
  install(FILES "${DSR_SDL2_RUNTIME_DLL}" DESTINATION .)
  install(FILES
    "${PROJECT_SOURCE_DIR}/tools/run_game.bat"
    "${PROJECT_SOURCE_DIR}/tools/run_game.ps1"
    "${PROJECT_SOURCE_DIR}/packaging/windows/README-WINDOWS.txt"
    "${PROJECT_SOURCE_DIR}/docs/BUILDING_WINDOWS.md"
    DESTINATION .)
else()
  install(TARGETS dq9 RUNTIME DESTINATION bin)
endif()
dsr_copy_sdl2_runtime(dq9)
