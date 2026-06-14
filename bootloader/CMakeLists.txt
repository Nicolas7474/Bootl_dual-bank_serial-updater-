# Script to generate bootloader.bin (Executable mapping to Sectors 0-1)

# 1. Define the executable target name and its source files
# This tells CMake to compile main.cpp and the assembly startup file into an executable named 'bootloader'
add_executable(bootloader
    src/main.cpp  
    src/startup_stm32f469nihx.s   
    "${CMAKE_SOURCE_DIR}/vendor/CMSIS_Device/system_stm32f4xx.c"
)

# 2. Specify where to find headers specific to the bootloader
# 'PRIVATE' means these include paths are only visible when compiling the bootloader target itself
target_include_directories(bootloader PRIVATE inc)

# 3. Link the shared code framework
# This pulls in the 'common_drivers' static library we created in the common folder, 
# automatically granting the bootloader access to your shared C++ drivers and 'memory_map.hpp'
target_link_libraries(bootloader 
    PRIVATE 
        common_drivers
)

# 4. Configure linker-specific options for this target
# -T tells the linker to use our custom memory layout script instead of the default host script
# -Map instructs the linker to output a text file mapping out exactly where every function and variable lands in flash/RAM
target_link_options(bootloader PRIVATE 
    "-T${CMAKE_CURRENT_SOURCE_DIR}/linker_bootloader.ld"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/bootloader.map"
    "--specs=rdimon.specs"        # <--- Silences the missing implementation warnings
    "-Wl,--no-warn-rwx-segments" # <--- Silences the RWX security warning _close, _lseek, _read, and _write
)

# 5. Define an automated post-build pipeline
# This runs immediately after the compiler successfully finishes creating the 'bootloader.elf' file
add_custom_command(TARGET bootloader POST_BUILD
    # Command A: Convert the ELF file (which contains debug symbols) into a raw, flashable 'bootloader.bin' binary
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader> ${CMAKE_CURRENT_BINARY_DIR}/bootloader.bin
    
    # Command B: Print the text, data, and bss section sizes directly to the terminal so you can track memory usage
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:bootloader>
    
    # Message displayed in the terminal when this block executes
    COMMENT "Generating bootloader.bin and printing memory usage..."
)