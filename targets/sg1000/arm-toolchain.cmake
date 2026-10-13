set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# GNU ARM toolchain: PATH first, otherwise PlatformIO's (as the Pokitto
# Makefile of Game Boy projects does).
set(SG_ARM_HINTS "$ENV{HOME}/.platformio/packages/toolchain-gccarmnoneeabi/bin"
                 "$ENV{USERPROFILE}/.platformio/packages/toolchain-gccarmnoneeabi/bin")
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc HINTS ${SG_ARM_HINTS} REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++ HINTS ${SG_ARM_HINTS} REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m0plus -mthumb")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-m0plus -mthumb")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-m0plus -mthumb")
