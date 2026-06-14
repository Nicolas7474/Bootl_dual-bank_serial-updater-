# 1. Define a static library target named 'common_drivers'
# This compiles the shared bare-metal C++ drivers into a reusable object block
add_library(common_drivers STATIC)

# 2. Specify header lookup paths for this library
# 'PUBLIC' means these paths are used to compile common files AND are automatically 
# passed to the bootloader and application when they link to this library.
target_include_directories(common_drivers PUBLIC 
    inc                                      # Maps to common/inc/ (where memory_map.hpp sits)
    ${CMAKE_SOURCE_DIR}/vendor/CMSIS         # Maps to your vendor core headers (core_cm4.h)
    ${CMAKE_SOURCE_DIR}/vendor/CMSIS_Device  # Maps to your vendor device headers (stm32f469xx.h)
)

# 3. Source files belonging to this library
# Right now, you only have headers, so we leave this empty. 
# As you write bare-metal C++ source files, you will list them here.
target_sources(common_drivers PRIVATE  
   src/myConfig.c
   src/timers.c
   src/uart3.cpp  
)



# Are the common and vendor folders flashed?
# No, they are not flashed directly as folders. Here is what happens under the hood during compilation:
    # The source files inside common (like myConfig.c and timers.c) are compiled into a static library file (libcommon_drivers.a).
    # When your application target compiles, the linker looks at that library and pulls out only the specific functions you actually called in your code.
    # It embeds those raw binary instructions directly inside application.elf / application.bin.
# The same thing happens to vendor files (like system_stm32f4xx.c). They get boiled down into binary instructions and merged into your final executable.
# When you flash application.elf or bootloader.elf, you are flashing a single self-contained binary file that already has everything it needs inside of it. 
# The folders themselves just stay on your computer.