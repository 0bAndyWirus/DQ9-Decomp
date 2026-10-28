add_library(dsr_spu STATIC src/spu.cpp)
target_link_libraries(dsr_spu PUBLIC dsr_headers)
dsr_target_compile_options(dsr_spu -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

add_executable(dsr_spu_tests tests/spu_tests.cpp)
target_link_libraries(dsr_spu_tests PRIVATE dsr_spu)
dsr_target_compile_options(dsr_spu_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_spu_tests COMMAND dsr_spu_tests)

add_executable(dsr_sound_tables_tests tests/sound_tables_tests.cpp)
dsr_target_compile_options(dsr_sound_tables_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_sound_tables_tests COMMAND dsr_sound_tables_tests)
