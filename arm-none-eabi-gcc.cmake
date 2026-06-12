# Target System Configuration
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Force CMake to skip compiler sanity checks (essential for bare-metal)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Define Compiler Executables matching your STM32CubeIDE 1.19.0 path
if(WIN32)
    set(TOOLCHAIN_PATH "C:/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin")
    set(CMAKE_C_COMPILER "${TOOLCHAIN_PATH}/arm-none-eabi-gcc.exe")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/arm-none-eabi-g++.exe")
    set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PATH}/arm-none-eabi-gcc.exe")
    set(CMAKE_OBJCOPY "${TOOLCHAIN_PATH}/arm-none-eabi-objcopy.exe" CACHE INTERNAL "")
    set(CMAKE_SIZE "${TOOLCHAIN_PATH}/arm-none-eabi-size.exe" CACHE INTERNAL "")
else()
    # Linux / macOS fallback paths if applicable
    set(CMAKE_C_COMPILER "arm-none-eabi-gcc")
    set(CMAKE_CXX_COMPILER "arm-none-eabi-g++")
    set(CMAKE_ASM_COMPILER "arm-none-eabi-gcc")
    set(CMAKE_OBJCOPY "arm-none-eabi-objcopy" CACHE INTERNAL "")
    set(CMAKE_SIZE "arm-none-eabi-size" CACHE INTERNAL "")
endif()

# Pass essential system specs down to CMake compiler tests
set(OBJECT_GEN_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS "${OBJECT_GEN_FLAGS}" CACHE INTERNAL "")
set(CMAKE_CXX_FLAGS "${OBJECT_GEN_FLAGS}" CACHE INTERNAL "")
set(CMAKE_ASM_FLAGS "${OBJECT_GEN_FLAGS}" CACHE INTERNAL "")