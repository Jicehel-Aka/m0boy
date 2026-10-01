# Recompiler implementation shared by the CLI and focused unit tests.
add_library(gbrecomp_core STATIC
    src/rom.cpp
    src/decoder.cpp
    src/analyzer.cpp
    src/bank_tracker.cpp
    src/symbol_table.cpp
    src/native_patch.cpp
    src/generator.cpp
    src/ir/ir_builder.cpp
    src/ir/ir_optimizer.cpp
    src/codegen/c_emitter.cpp
    src/codegen/meta_emitter.cpp
)

target_include_directories(gbrecomp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Version of the Gamebuino META edition (targets/meta/VERSION, the one source
# for the repository and the release archive).  The meta emitter stamps it
# into gbl_config.h so a firmware records which gbrecomp produced it;
# compile.sh and tools/meta-compiler read it back from firmware.bin and
# compare it with `gbrecomp --meta-version`.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/VERSION" GBRECOMP_META_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/VERSION")
target_compile_definitions(gbrecomp_core PRIVATE
    GBRECOMP_VERSION="${PROJECT_VERSION}"
    GBRECOMP_META_VERSION="${GBRECOMP_META_VERSION}"
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

add_executable(gbrecomp src/main.cpp)
target_link_libraries(gbrecomp PRIVATE gbrecomp_core)
target_compile_definitions(gbrecomp PRIVATE
    GBRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtime"
    GBRECOMP_DEFAULT_META_DIR="${PROJECT_SOURCE_DIR}/targets/meta"
    GBRECOMP_VERSION="${PROJECT_VERSION}"
    GBRECOMP_META_VERSION="${GBRECOMP_META_VERSION}"
)
