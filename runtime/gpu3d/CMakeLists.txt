add_library(dsr_gpu3d STATIC
  src/gpu3d.cpp
  src/rasterizer.cpp
  src/gpu2d_adapter.cpp
  src/texture.cpp
  src/texture_rgba.cpp
)
target_link_libraries(dsr_gpu3d PUBLIC dsr_headers dsr_gpu2d)
dsr_target_compile_options(dsr_gpu3d -Wall -Wextra -Wpedantic)
target_include_directories(dsr_gpu3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(dsr_gpu3d_tests tests/test_gpu3d.cpp)
target_link_libraries(dsr_gpu3d_tests PRIVATE dsr_gpu3d)
target_include_directories(dsr_gpu3d_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dsr_target_compile_options(dsr_gpu3d_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_gpu3d_tests COMMAND dsr_gpu3d_tests)

# Compatibility snapshots captured from the pre-refactor software renderer.
add_executable(dsr_raster_golden_tests tests/fixtures/test_raster_goldens.cpp)
target_link_libraries(dsr_raster_golden_tests PRIVATE dsr_gpu3d)
target_include_directories(dsr_raster_golden_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dsr_target_compile_options(dsr_raster_golden_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_raster_golden_tests COMMAND dsr_raster_golden_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/raster_goldens.txt)

add_executable(dsr_raster_boundary_tests tests/fixtures/test_raster_boundaries.cpp)
target_link_libraries(dsr_raster_boundary_tests PRIVATE dsr_gpu3d)
target_include_directories(dsr_raster_boundary_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dsr_target_compile_options(dsr_raster_boundary_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_raster_boundary_tests COMMAND dsr_raster_boundary_tests)

# The compatibility raster snapshots require separate float operations, not FMA.
# Windows already sets the corresponding strict floating-point option in dsr_headers.
if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(target dsr_gpu3d dsr_gpu3d_tests dsr_raster_golden_tests dsr_raster_boundary_tests)
    target_compile_options(${target} PRIVATE -ffp-contract=off)
  endforeach()
endif()

# BandPool is header-only and can also be used by the 2D renderer.
find_package(Threads REQUIRED)
target_link_libraries(dsr_gpu3d PUBLIC Threads::Threads)
add_executable(dsr_band_pool_tests tests/test_band_pool.cpp)
target_link_libraries(dsr_band_pool_tests PRIVATE Threads::Threads)
target_include_directories(dsr_band_pool_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dsr_target_compile_options(dsr_band_pool_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_band_pool_tests COMMAND dsr_band_pool_tests)
set_tests_properties(dsr_band_pool_tests PROPERTIES TIMEOUT 60)
add_executable(dsr_texture_rgba_tests tests/test_texture_rgba.cpp)
target_link_libraries(dsr_texture_rgba_tests PRIVATE dsr_gpu3d)
target_include_directories(dsr_texture_rgba_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dsr_target_compile_options(dsr_texture_rgba_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_texture_rgba_tests COMMAND dsr_texture_rgba_tests)

add_executable(dsr_texture_cache_tests tests/test_texture_cache.cpp)
target_link_libraries(dsr_texture_cache_tests PRIVATE dsr_core)
target_include_directories(dsr_texture_cache_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dsr_target_compile_options(dsr_texture_cache_tests -Wall -Wextra -Wpedantic)
add_test(NAME dsr_texture_cache_tests COMMAND dsr_texture_cache_tests)
