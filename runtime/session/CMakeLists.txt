add_library(dsr_session STATIC session.cpp session_rom.cpp)
target_link_libraries(dsr_session PUBLIC dsr_core)
target_include_directories(dsr_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
set_target_properties(dsr_session PROPERTIES POSITION_INDEPENDENT_CODE ON)

# A runnable session DLL needs the real generated tables. PE rejects the
# unresolved symbols that an ELF runtime-only shared-library build tolerated.
if(DQ9_GENERATED_DIR)
add_library(dsr_session_c SHARED session_c.cpp)
target_link_libraries(dsr_session_c PRIVATE dsr_session)
target_include_directories(dsr_session_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(dsr_session_c PRIVATE DSR_SESSION_BUILD=1)
set_target_properties(dsr_session_c PROPERTIES OUTPUT_NAME dsr_session)
if(WIN32)
  set_target_properties(dsr_session_c PROPERTIES PREFIX "")
endif()
endif()

add_executable(dsr_session_tests session_tests.cpp)
target_link_libraries(dsr_session_tests PRIVATE dsr_session)
add_test(NAME dsr_session_tests COMMAND dsr_session_tests)
