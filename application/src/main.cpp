/* Bootloader with firmware update capability on dual-bank flash memory swap
 * APPLICATION FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
 * rouge    GPIOD->ODR ^= GPIO_ODR_OD5;
 */

#include "memory_map.hpp"
#include <cstdint>
#include "stm32f469xx.h"
#include "myConfig.h"
#include "timers.h"
#include <string.h>
#include "uart3.hpp"


void initialize_hardware() {
    // 1. Relocate the Vector Table to point to the application's starting Flash boundary.
    // This is the absolute first thing a shifted bare-metal firmware must execute.
	SCB->VTOR = Memory::APP_START_ADDR;
	__enable_irq();

     // 3. Initialize your application-specific peripherals here (GPIO, clocks, timers, etc.)
    SysClockConfig();
    SysTick_Init();
    GPIO_Config();
    InterruptGPIO_Config();
}

static void flash_unlock() {
    // Check if the flash is already unlocked
    if ((FLASH->CR & FLASH_CR_LOCK) != 0) {
        // Authorize flash register access by writing the mandatory key sequence
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
}


static uint8_t get_active_bank_choice() {
    // Enable SYSCFG clock just in case it was turned off
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    // Check if the User Flash Bank mode bit is set
    if ((SYSCFG->MEMRMP & SYSCFG_MEMRMP_UFB_MODE) != 0) {
        return 2;
    }
    return 1;
}

static void write_0xA() {

    uint32_t address = (get_active_bank_choice() == 1) ? 0x08104000U : 0x08004000U; // Sector 13 start address
    uint32_t sector_boundary = (get_active_bank_choice() == 1) ? 0x08107FFCU : 0x08007FFCU;
    while (address < sector_boundary) {
    	uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

    	if (current_byte == 0xFF) {
    		// Found an empty slot! Unlock flash right here where needed
    		flash_unlock();
    		// Clear any outstanding flash status flags before writing
    		FLASH->SR = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR |FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;
    		// Set register to byte write size (PSIZE = 0)
    		FLASH->CR &= ~FLASH_CR_PSIZE;
    		FLASH->CR |= FLASH_CR_PG;
    		// Write the 0x0A force update flag
    		*reinterpret_cast<volatile uint8_t*>(address) = 0x0A;
    		// Wait for operation to complete safely
    		while (FLASH->SR & FLASH_SR_BSY);
    		// Clean up programming configuration and lock flash immediately
    		FLASH->CR &= ~FLASH_CR_PG;
    		FLASH->CR |= FLASH_CR_LOCK;
    		return;
    	}
    	address++;
    }

    // FALLBACK: If the code reaches here, Sector 13 is full!
    // We must erase the sector to reset the wear-leveling tracking block.
    flash_unlock();
    // Sector 13 Erase Sequence (Sector 13, Voltage Range x32 assumed for 3.3V)
    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= (13U << FLASH_CR_SNB_Pos) | FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_STRT;
    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_SER;
    // Now write 0x0A to the very first slot of the clean sector
    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FLASH_CR_PG;
    *reinterpret_cast<volatile uint8_t*>(0x08104000U) = 0x0A;
    *reinterpret_cast<volatile uint32_t*>(0x08107FFCU) = 0x1A2B3C4DU; // Write the magic word at the very last 32-bit word
    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->CR |= FLASH_CR_LOCK;
}

void UART_RxCpltCallback_IT([[maybe_unused]] std::span<const uint8_t> incoming_data) {}
void UART_RxCpltCallback_DMA([[maybe_unused]] std::span<const uint8_t> incoming_data) {}

int main() {
	initialize_hardware();
	BareM_StatusTypeDef res = uart3.init(115200);
	while(res != Bare_OK);
	GPIOD->ODR ^= GPIO_ODR_OD4;


	// char sel[4] = "oi ";
	char se = get_active_bank_choice();
	char ascii_bank = se + '0';
	uart3.UART_Transmit_IT(&ascii_bank);

	NBdelay_ms(500);
    GPIOG->ODR ^= GPIO_ODR_OD6;


    while (true) {
    	NBdelay_ms(3000);
    	uart3.UART_Transmit_IT(&ascii_bank);
    }
    return 0;
}

// Prevent C++ name mangling so the Assembly vector table can find this exact symbol
extern "C" {
    void EXTI0_IRQHandler(void);
}

// PAO button input interrupt handler
void EXTI0_IRQHandler() {
	if (EXTI->PR & (1<<0)) {  // button pushed : if the PA0 triggered the interrupt
		EXTI->PR = (1<<0);  // Clear the interrupt flag by writing a 1
		write_0xA();
		GPIOD->ODR ^= GPIO_ODR_OD4; //toggle orange

	}
}
