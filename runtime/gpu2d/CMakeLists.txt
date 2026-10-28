add_library(dsr_gpu2d STATIC src/gpu2d.cpp)
target_link_libraries(dsr_gpu2d PUBLIC dsr_headers)
dsr_target_compile_options(dsr_gpu2d -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(dsr_gpu2d_tests tests/gpu2d_tests.cpp)
target_link_libraries(dsr_gpu2d_tests PRIVATE dsr_gpu2d)
dsr_target_compile_options(dsr_gpu2d_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_gpu2d_tests COMMAND dsr_gpu2d_tests)

add_executable(dsr_ordinary_2d_tests tests/ordinary_2d_tests.cpp)
dsr_target_compile_options(dsr_ordinary_2d_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_ordinary_2d_tests COMMAND dsr_ordinary_2d_tests)

add_executable(dsr_vram_tests tests/vram_tests.cpp)
target_link_libraries(dsr_vram_tests PRIVATE dsr_gpu3d)
dsr_target_compile_options(dsr_vram_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_vram_tests COMMAND dsr_vram_tests)
