# Source CPU, source system and output backend are independent build layers.
add_library(m0boy_common INTERFACE)
target_include_directories(m0boy_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# No Nintendo header or mapper dependency in the CPU decoder.
add_library(m0boy_cpu_sm83 STATIC src/cpu/sm83/decoder.cpp)
target_link_libraries(m0boy_cpu_sm83 PUBLIC m0boy_common)
set_target_properties(m0boy_cpu_sm83 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(m0boy_cpu_z80 STATIC src/cpu/z80/decoder.cpp)
target_link_libraries(m0boy_cpu_z80 PUBLIC m0boy_common)
set_target_properties(m0boy_cpu_z80 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(m0boy_frontends STATIC
    src/system.cpp
    src/system/gameboy/cartridge.cpp
    src/system/gameboy/frontend.cpp
    src/system/sg1000/cartridge.cpp
)
target_link_libraries(m0boy_frontends PUBLIC m0boy_common)
set_target_properties(m0boy_frontends PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Existing Game Boy analysis, IR and emitters retain their public API.
add_library(gbrecomp_core STATIC
    src/system/gameboy/analyzer.cpp
    src/system/gameboy/bank_tracker.cpp
    src/system/gameboy/decode_bank.cpp
    src/system/sg1000/generator.cpp
    src/system/sg1000/emitter.cpp
    src/symbol_table.cpp
    src/native_patch.cpp
    src/generator.cpp
    src/ir/ir_builder.cpp
    src/ir/ir_optimizer.cpp
    src/codegen/c_emitter.cpp
    src/codegen/register_ir.cpp
    src/codegen/meta_emitter.cpp
)

target_link_libraries(gbrecomp_core PUBLIC m0boy_frontends m0boy_cpu_sm83 m0boy_cpu_z80 m0boy_z80_runtime)

target_include_directories(gbrecomp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # the SM83 cycle table shared with the runtime (gbl_cycles.h)
        ${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/runtime
)

# Versions of the two consoles of the META edition, one file each: the META's
# in targets/meta/VERSION (the one source for the repository and the release
# archive), the Pokitto's in targets/meta/platform/pokitto/VERSION.  The meta
# emitter stamps the target console's into gbl_config.h so a firmware records
# which gbrecomp produced it; compile.sh and tools/meta-compiler read it back
# from firmware.bin and compare it with `gbrecomp --meta-version` or
# `gbrecomp --pokitto-version`.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/VERSION" GBRECOMP_META_VERSION LIMIT_COUNT 1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/platform/pokitto/VERSION" GBRECOMP_POKITTO_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/platform/pokitto/VERSION")
target_compile_definitions(gbrecomp_core PRIVATE
    GBRECOMP_VERSION="${PROJECT_VERSION}"
    GBRECOMP_META_VERSION="${GBRECOMP_META_VERSION}"
    GBRECOMP_POKITTO_VERSION="${GBRECOMP_POKITTO_VERSION}"
)

# Enable position-independent code for shared library support
set_target_properties(gbrecomp_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_LINUX)
endif()

# m0recomp is the system-neutral entry point; gbrecomp stays available for
# compile.sh, the Node frontend and existing generated-project tests.
add_library(m0boy_cli OBJECT src/main.cpp)
target_link_libraries(m0boy_cli PRIVATE gbrecomp_core m0boy_cpu_z80)
add_executable(m0recomp $<TARGET_OBJECTS:m0boy_cli>)
add_executable(gbrecomp $<TARGET_OBJECTS:m0boy_cli>)
target_link_libraries(m0recomp PRIVATE gbrecomp_core m0boy_cpu_z80)
target_link_libraries(gbrecomp PRIVATE gbrecomp_core m0boy_cpu_z80)
target_compile_definitions(m0boy_cli PRIVATE
    GBRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtime"
    GBRECOMP_DEFAULT_META_DIR="${PROJECT_SOURCE_DIR}/targets/meta"
    GBRECOMP_VERSION="${PROJECT_VERSION}"
    GBRECOMP_META_VERSION="${GBRECOMP_META_VERSION}"
    GBRECOMP_POKITTO_VERSION="${GBRECOMP_POKITTO_VERSION}"
)
