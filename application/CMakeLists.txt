# Script to generate application.bin (Executable mapping to Sectors 2+)

# 1. Define the executable target name and its source files
# This compiles your main application code and its dedicated startup file
add_executable(application
    src/main.cpp
    src/metadata.cpp 
    src/startup_stm32f469nihx.s  
    "${CMAKE_SOURCE_DIR}/vendor/CMSIS_Device/system_stm32f4xx.c"
)


# 2. Specify where to find headers specific to the main application
# 'PRIVATE' means these include paths are only visible when compiling the application target itself
target_include_directories(application PRIVATE inc)

# 3. Link the shared code framework
# This pulls in the 'common_drivers' static library, giving your application access
# to 'memory_map.hpp' and any bare-metal drivers you write in the common folder
target_link_libraries(application 
    PRIVATE 
        common_drivers
)

# 4. Configure linker-specific options for the application target
# -T links our custom application linker script (which targets 0x08008400)
# -Map outputs a text file showing exactly where application variables and functions sit in memory
target_link_options(application PRIVATE 
    "-T${CMAKE_CURRENT_SOURCE_DIR}/linker_application.ld"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/application.map"
    "--specs=rdimon.specs"        # <--- Silences the missing implementation warnings
    "-Wl,--no-warn-rwx-segments" # <--- Silences the RWX security warning _close, _lseek, _read, and _write
)

# 5. Define an automated post-build pipeline
# This runs automatically after the compiler successfully creates the 'application.elf' file
add_custom_command(TARGET application POST_BUILD
    # Command A: Convert the ELF file into a raw, flashable 'application.bin' binary
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:application> ${CMAKE_CURRENT_BINARY_DIR}/application.bin
    
    # Command B: Print the text, data, and bss section sizes directly to the terminal
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:application>
    
    # Message displayed in the terminal during the build
    COMMENT "Generating application.bin and printing memory usage..."
)