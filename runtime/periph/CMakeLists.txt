add_library(dsr_periph STATIC src/periph.cpp)
target_link_libraries(dsr_periph PUBLIC dsr_headers)
dsr_target_compile_options(dsr_periph -Wall -Wextra -Wpedantic)

add_executable(dsr_periph_tests tests/periph_tests.cpp)
target_link_libraries(dsr_periph_tests PRIVATE dsr_periph)
dsr_target_compile_options(dsr_periph_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_periph_tests COMMAND dsr_periph_tests)
