# gbrecomp, Gamebuino META edition: the recompiler plus the gbl target.
# Root CMakeLists.txt of the release archive (targets/meta/package/
# make_package.sh puts it at the top); the repository's own root builds
# the desktop runtime and every test.
cmake_minimum_required(VERSION 3.20)
# The version is stamped into every firmware ("gbrecomp-meta x.y.z");
# compile.sh offers to rebuild firmwares made by another version.
project(gbrecomp-meta VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(recompiler)

option(BUILD_TESTS "Build the META target tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GBL_META_DIR "${PROJECT_SOURCE_DIR}/targets/meta")
    file(GLOB GBL_META_RUNTIME_SOURCES "${GBL_META_DIR}/runtime/gbl_*.c")

    add_executable(test_meta_ppu_reference "${GBL_META_DIR}/tests/ppu_reference.c" ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_ppu_reference PRIVATE "${GBL_META_DIR}/runtime" "${GBL_META_DIR}/tests/config")
    add_test(NAME meta_ppu_matches_per_pixel_reference COMMAND test_meta_ppu_reference)

    add_executable(test_meta_ppu_reference_pal32 "${GBL_META_DIR}/tests/ppu_reference.c" ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_ppu_reference_pal32 PRIVATE "${GBL_META_DIR}/runtime" "${GBL_META_DIR}/tests/config")
    target_compile_definitions(test_meta_ppu_reference_pal32 PRIVATE GBL_PPU_PAL32=1 GBL_PPU_BATCH=1)
    add_test(NAME meta_ppu_pal32_matches_per_pixel_reference COMMAND test_meta_ppu_reference_pal32)

    add_executable(test_meta_lcd_queue "${GBL_META_DIR}/tests/lcd_queue.c")
    target_include_directories(test_meta_lcd_queue PRIVATE "${GBL_META_DIR}/platform/src")
    add_test(NAME meta_lcd_queue_is_fifo_and_lossless COMMAND test_meta_lcd_queue)

    add_executable(test_meta_display_sim "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/src/meta_display.c" ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform/src" "${GBL_META_DIR}/tests")
    add_test(NAME meta_display_panel_matches_reference COMMAND test_meta_display_sim)

    add_test(NAME meta_target_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_meta_target_e2e.py"
                --gbrecomp "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_target_end_to_end PROPERTIES TIMEOUT 300)

    add_executable(test_meta_fat_check "${GBL_META_DIR}/tests/fat_check.c"
        "${GBL_META_DIR}/platform/src/meta_fat.c")
    target_include_directories(test_meta_fat_check PRIVATE "${GBL_META_DIR}/platform/src")
    add_test(NAME meta_fat_finds_streamed_rom
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_meta_fat.py"
                --fat-check "$<TARGET_FILE:test_meta_fat_check>")
endif()
