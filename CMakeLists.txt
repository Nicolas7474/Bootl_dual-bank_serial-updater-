#############################################################################################################################
# file:  CMakeLists.txt
# brief: the top-level "CMakeLists.txt" (Static Library) 
#############################################################################################################################

cmake_minimum_required(VERSION 3.22)

#############################################################################################################################

# 1. Force the ARM toolchain configuration
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/arm-none-eabi-gcc.cmake")

project(Bootloader_System C CXX ASM)

# 2. Enforce Modern C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_EXECUTABLE_SUFFIX ".elf") # avoid passing manually the start address in the CLI when flashing application

# Aggregate all compilation flags (Hardware target, optimization, warnings, and C++ specifics)
set(GLOBAL_COMPILER_FLAGS
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -Og					# Optimize for debugging
    -g					# Include debugging symbols
    -Wall
    -Wextra
    -Wpedantic
    -ffunction-sections	# Places each function in its own section (helps linker strip unused code)
    -fdata-sections		# Places each data item in its own section		
)

# Separate options to avoid warning message "....is valid for C++/D/ObjC++ but not for C"
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> 	# Disables C++ exception handling overhead
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>		# Disables C++ Run-Time Type Information overhead
)

# Aggregate all linker flags
set(GLOBAL_LINKER_FLAGS
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -Wl,--gc-sections      # This stays -Wl because --gc-sections IS an internal linker flag
    --specs=nano.specs     # Removed -Wl, so the compiler wrapper processes it
    --specs=nosys.specs    # Removed -Wl, so the compiler wrapper processes it
)

# Apply the combined lists to the project
add_compile_options(${GLOBAL_COMPILER_FLAGS})
add_link_options(${GLOBAL_LINKER_FLAGS})

# Explicitly force ASM files to compile using the C preprocessor 
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -x assembler-with-cpp")

# 4. Include our distinct project layers
add_subdirectory(common)
add_subdirectory(bootloader)
add_subdirectory(application)


# 5. Automated System-Wide Flash Target
add_custom_target(flash
    COMMAND  "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe" 
    		-c port=SWD 
            -w "${CMAKE_BINARY_DIR}/bootloader/bootloader.elf" 
            -w "${CMAKE_BINARY_DIR}/application/application.elf" 
            -v
            -rst
    USES_TERMINAL
    DEPENDS bootloader application # Doesn't flash until the binaries have been successfully compiled first
    COMMENT "Flashing complete system: Bootloader + Application..."
)



# Are the common and vendor folders flashed?
# No, they are not flashed directly as folders. Here is what happens under the hood during compilation:
    # The source files inside common (like myConfig.c and timers.c) are compiled into a static library file (libcommon_drivers.a).
    # When your application target compiles, the linker looks at that library and pulls out only the specific functions you actually called in your code.
    # It embeds those raw binary instructions directly inside application.elf / application.bin.
# The same thing happens to vendor files (like system_stm32f4xx.c). They get boiled down into binary instructions and merged into your final executable.
# When you flash application.elf or bootloader.elf, you are flashing a single self-contained binary file that already has everything it needs inside of it. 
# The folders themselves just stay on your computer.



