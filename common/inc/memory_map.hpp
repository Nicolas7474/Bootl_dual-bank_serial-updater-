#pragma once

#include <cstdint>

namespace Memory {

// ============================================================================
// PHYSICAL HARDWARE CONSTANTS (STM32F469NI)
// ============================================================================
constexpr uint32_t FLASH_BASE_ADDR     = 0x08000000U; // Start of Flash Bank 1
constexpr uint32_t SRAM1_BASE_ADDR     = 0x20000000U; // Start of internal SRAM1
constexpr uint32_t SRAM1_SIZE          = 320 * 1024;  // 320 KB

// ============================================================================
// BOOTLOADER MEMORY ALLOCATION
// ============================================================================
// Allocating physical Sector 0 (16KB) and Sector 1 (16KB)
constexpr uint32_t BL_START_ADDR       = FLASH_BASE_ADDR;
constexpr uint32_t BL_MAX_SIZE         = 32 * 1024;   // 32 KB total allocation

// ============================================================================
// APPLICATION MEMORY ALLOCATION
// ============================================================================

// The actual application executable code (and its Vector Table) starts here.
// This matches the FLASH ORIGIN address specified in 'linker_application.ld'
constexpr uint32_t APP_START_ADDR      = 0x08008000U; // 0x08000000U + (32 * 1024)

// Maximum space available for the application (Total Flash minus Bootloader space)
constexpr uint32_t MAX_APP_SIZE        = (2048 * 1024) - BL_MAX_SIZE;

} // namespace Memory


/*
When Ninja runs the toolchain compiler and linker, it maps the structure exactly where the linker rules dictated:

Physical Address    Layout Block

0x08000000		   [ Bootloader code - sectors 0 & 1 ]				2x 16KB
0x08008000         [ .isr_vector (Interrupt Vector Table) ]    		ALIGN(512)-> Force the next section to align to 512 bytes (0x08008200)
0x08008200         [ .fw_header  (application_header struct) ]  	struct FirmwareHeader with 4 bytes of uint32_t = 16 bytes
                    ├── magic        = 0x5A5A5A5A
                    ├── payload_size = [Calculated value of your C++ code]
                    └── expected_crc = 0xFFFFFFFF
0x08008210         [ .text       (Your actual compiled functions start here) ]


0x08008288 is the mathematically exact byte where the Reset_Handler lands after accounting for the Vector Table alignment,
the 16-byte custom header, and the mandatory compiler startup runtime instructions.

*/
