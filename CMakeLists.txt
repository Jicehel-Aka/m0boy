# m0boy: gbrecomp (the recompiler) and the Cortex-M0+ targets, Gamebuino
# META and Pokitto.  `cmake -G Ninja -B build . && ninja -C build` gives
# build/bin/gbrecomp, which compile.sh uses.
cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_SOURCE_DIR}/targets/meta/host/prefer_compiler.cmake")
m0boy_select_host_compiler(C CXX)
# The version (targets/meta/VERSION) is stamped into every firmware
# ("gbrecomp-meta x.y.z"); compile.sh offers to rebuild firmwares made by
# another version.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/targets/meta/VERSION" GBRECOMP_META_VERSION_FILE LIMIT_COUNT 1)
project(m0boy VERSION ${GBRECOMP_META_VERSION_FILE} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(recompiler)

add_library(m0boy_z80_runtime STATIC targets/sg1000/runtime/z80.c)
target_include_directories(m0boy_z80_runtime PUBLIC targets/sg1000/runtime)
add_library(m0boy_sg1000_runtime STATIC targets/sg1000/runtime/sg1000.c targets/sg1000/runtime/sg_audio.c targets/sg1000/runtime/tms9918.c)
target_link_libraries(m0boy_sg1000_runtime PUBLIC m0boy_z80_runtime)
add_executable(sg1000_host targets/sg1000/host/sg_host.c)
target_link_libraries(sg1000_host PRIVATE m0boy_sg1000_runtime)
# Desktop play is optional; firmware and the reference harness remain headless.
find_package(SDL2 QUIET)
if(TARGET SDL2::SDL2)
    add_executable(sg1000_play targets/sg1000/host/sg_player.c)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(sg1000_play PRIVATE SDL2::SDL2main)
    endif()
    target_link_libraries(sg1000_play PRIVATE m0boy_sg1000_runtime SDL2::SDL2)
endif()


option(BUILD_TESTS "Build the META and Pokitto target tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME compile_batch_source_systems
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_compile_batch.py")
    add_executable(test_z80_execution tests/test_z80_execution.cpp)
    target_link_libraries(test_z80_execution PRIVATE m0boy_z80_runtime m0boy_cpu_z80)
    add_test(NAME z80_execution_reference COMMAND test_z80_execution)
    add_executable(test_sg1000_runtime tests/test_sg1000_runtime.c)
    target_link_libraries(test_sg1000_runtime PRIVATE m0boy_sg1000_runtime)
    add_test(NAME sg1000_runtime_devices COMMAND test_sg1000_runtime)
    add_test(NAME sg1000_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_sg1000_e2e.py"
                --recompiler "$<TARGET_FILE:m0recomp>" --host "$<TARGET_FILE:sg1000_host>")
    set_tests_properties(sg1000_native_end_to_end PROPERTIES TIMEOUT 300)
    if(TARGET sg1000_play)
        add_executable(test_sg1000_input tests/test_sg1000_input.c)
        target_include_directories(test_sg1000_input PRIVATE targets/sg1000/host)
        target_link_libraries(test_sg1000_input PRIVATE SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(test_sg1000_input PRIVATE SDL2::SDL2main)
        endif()
        add_test(NAME sg1000_keyboard_controls COMMAND test_sg1000_input)
        add_test(NAME sg1000_desktop_player
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_sg1000_player.py"
                    "$<TARGET_FILE:sg1000_play>" "$<TARGET_FILE:sg1000_host>")
    endif()
    add_executable(test_source_systems tests/test_source_systems.cpp)
    target_link_libraries(test_source_systems PRIVATE m0boy_frontends)
    add_test(NAME source_system_profiles COMMAND test_source_systems)
    add_executable(test_sm83_reader tests/test_sm83_reader.cpp)
    target_link_libraries(test_sm83_reader PRIVATE m0boy_cpu_sm83)
    add_test(NAME sm83_console_independent_reader COMMAND test_sm83_reader)
    add_executable(test_cpu_decoders tests/test_cpu_decoders.cpp)
    target_link_libraries(test_cpu_decoders PRIVATE m0boy_cpu_sm83 m0boy_cpu_z80)
    add_test(NAME sm83_z80_shared_encoding COMMAND test_cpu_decoders)
    add_test(NAME source_system_cli
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_source_cli.py"
                "$<TARGET_FILE:m0recomp>")
    add_executable(test_register_ir tests/test_register_ir.cpp)
    target_link_libraries(test_register_ir PRIVATE gbrecomp_core)
    add_test(NAME meta_register_ir_semantics COMMAND test_register_ir)
    add_test(NAME meta_register_ir_native
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_register_ir_native.py"
                --ir-test "$<TARGET_FILE:test_register_ir>" --runtime "${PROJECT_SOURCE_DIR}/targets/meta/runtime")
    set_tests_properties(meta_register_ir_native PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME meta_register_ir_clang
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_register_ir_native.py"
                --ir-test "$<TARGET_FILE:test_register_ir>" --runtime "${PROJECT_SOURCE_DIR}/targets/meta/runtime"
                --compiler clang)
    set_tests_properties(meta_register_ir_clang PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME meta_compiler_selection
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_arm_compiler.py")
    add_test(NAME meta_cmake_compiler_selection
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_cmake_compiler.py"
                --cmake "${CMAKE_COMMAND}" --selector "${PROJECT_SOURCE_DIR}/targets/meta/host/prefer_compiler.cmake")
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
    target_include_directories(test_meta_lcd_queue PRIVATE "${GBL_META_DIR}/platform/meta")
    add_test(NAME meta_lcd_queue_is_fifo_and_lossless COMMAND test_meta_lcd_queue)

    add_executable(test_meta_display_sim "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/meta/meta_display.c" "${GBL_META_DIR}/platform/src/meta_palettes.c"
        ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform/src" "${GBL_META_DIR}/platform/meta" "${GBL_META_DIR}/tests")
    add_test(NAME meta_display_panel_matches_reference COMMAND test_meta_display_sim)

    add_executable(test_meta_display_sim_stream "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/meta/meta_display.c" "${GBL_META_DIR}/platform/src/meta_palettes.c"
        ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim_stream PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform/src" "${GBL_META_DIR}/platform/meta" "${GBL_META_DIR}/tests")
    target_compile_definitions(test_meta_display_sim_stream PRIVATE META_MERGE_MODES=0)
    add_test(NAME meta_display_stream_profile_matches_reference COMMAND test_meta_display_sim_stream)

    # 12-bit panel (META_LCD_12BPP): rows packed in place, read back as the panel would.
    add_executable(test_meta_display_sim_12bpp "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/meta/meta_display.c" "${GBL_META_DIR}/platform/src/meta_palettes.c"
        ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim_12bpp PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform/src" "${GBL_META_DIR}/platform/meta"
        "${GBL_META_DIR}/tests")
    target_compile_definitions(test_meta_display_sim_12bpp PRIVATE META_LCD_12BPP=1)
    add_test(NAME meta_display_12bpp_matches_reference COMMAND test_meta_display_sim_12bpp)

    add_executable(test_meta_lcd_pack "${GBL_META_DIR}/tests/lcd_pack.c")
    target_include_directories(test_meta_lcd_pack PRIVATE "${GBL_META_DIR}/platform/meta")
    add_test(NAME meta_lcd_pack444_matches_st7735 COMMAND test_meta_lcd_pack)

    add_executable(test_meta_apu_lfsr "${GBL_META_DIR}/tests/apu_lfsr.c")
    target_include_directories(test_meta_apu_lfsr PRIVATE "${GBL_META_DIR}/runtime")
    add_test(NAME meta_apu_lfsr_matches_per_clock COMMAND test_meta_apu_lfsr)

    add_test(NAME meta_target_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_meta_target_e2e.py"
                --gbrecomp "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_target_end_to_end PROPERTIES TIMEOUT 600)

    # --target pokitto: the generated project, its host harness, and the
    # bare-metal firmware when arm-none-eabi-gcc exists.
    add_test(NAME meta_pokitto_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_pokitto_target.py"
                --gbrecomp "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_pokitto_target PROPERTIES TIMEOUT 300)

    add_executable(test_meta_fat_check "${GBL_META_DIR}/tests/fat_check.c"
        "${GBL_META_DIR}/platform/src/meta_fat.c")
    target_include_directories(test_meta_fat_check PRIVATE "${GBL_META_DIR}/platform/src")
    add_test(NAME meta_fat_finds_streamed_rom
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_meta_fat.py"
                --fat-check "$<TARGET_FILE:test_meta_fat_check>")
endif()
