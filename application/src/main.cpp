/* Bootloader with firmware update capability on dual-bank flash memory swap
 * APPLICATION FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
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
