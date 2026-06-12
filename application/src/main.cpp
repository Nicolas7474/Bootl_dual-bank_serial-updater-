/**
 * APPLICATION FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
 * ============================================================================
 * ARCHITECTURE & MEMORY TOPOLOGY
 * ============================================================================
 * This project utilizes a unified repository structure (Monorepo) to build
 * two entirely distinct, isolated binary artifacts:
 * 1. bootloader.bin   (Flashed once in production / rarely updated)
 * 2. application.bin  (Updated routinely in the field)
 * * Shared hardware drivers (UART, GPIO, Flash) and the global memory map
 * are maintained in a 'common/' directory to prevent code duplication and
 * eliminate "configuration drift" (mismatched flash addresses between apps).
 * ============================================================================
 * FLASH MEMORY MAP (STM32F469 - Bank 1)
 * ============================================================================
 * Sector erasure boundaries dictate that the Application MUST begin exactly
 * at the start of a physical flash sector.
 * * +----------+----------------------+---------+-----------------------------+
 * | Sector     | Start Address        | Size    | Target Assignment           |
 * +------------+----------------------+---------+-----------------------------+
 * | Sector 0   | 0x0800 0000          | 16 KB   | Bootloader Code             |
 * | Sector 1   | 0x0800 4000          | 16 KB   | Bootloader Code (Cont.)     |
 * +------------+----------------------+---------+-----------------------------+
 * | Sector 2   | 0x0800 8000          | 16 KB   | App Header (Vector Table)   |
 * | Sector 2+  | 0x0800 8400          | --      | Application Binary Execution|
 * +------------+----------------------+---------+-----------------------------+
 * * - BL_START_ADDR   (0x08000000): Bootloader entry point and hardware vector table.
 *   - APP_HEADER_ADDR (0x08008000): 1KB reserved for metadata (CRC, size, magic numbers).
 *   - APP_START_ADDR  (0x08008400): Application vector table & main execution entry.
 * ============================================================================
 * BOOTLOADER SEQUENCE
 * ============================================================================
 * 1. Power-on Reset (POR) -> Executes Bootloader at 0x08000000.
 * 2. Check update pin / check valid application signature in APP_HEADER_ADDR.
 * 3. If update required: Receive new binary over UART -> Erase Sector 2+ -> Write.
 * 4. If execution ready:
 * a. Disable interrupts & clear pending ISRs.
 * b. Set Main Stack Pointer (MSP) to application's Vector Table [0].
 * c. Offset Cortex-M4 SCB->VTOR to APP_START_ADDR.
 * d. Jump to application Reset_Handler.
 */

#include "memory_map.hpp"
#include <cstdint>
#include "stm32f469xx.h"
#include "myConfig.h"
#include "timers.h"
#include <string.h>
//#include "../../common/inc/uart3.hpp"


void initialize_hardware() {
    // 1. Relocate the Vector Table to point to the application's starting Flash boundary.
    // This is the absolute first thing a shifted bare-metal firmware must execute.
	SCB->VTOR = Memory::APP_START_ADDR;
	__enable_irq();

     // 3. Initialize your application-specific peripherals here (GPIO, clocks, timers, etc.)
    SysClockConfig();
    SysTick_Init();
    GPIO_Config();
}

int main() {
	initialize_hardware();
  	GPIOD->ODR ^= GPIO_ODR_OD4; //toggle orange

    // The Application's Main Loop
    while (true) {
        // Your application logic lives here (e.g., blinking an LED, running tasks)
        asm("nop");
    }

    return 0;
}
