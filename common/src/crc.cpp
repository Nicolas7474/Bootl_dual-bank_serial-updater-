#include <cstdint>
#include <cstddef>
#include <stm32f4xx.h>
#include "crc.hpp"


/**
 * @brief Initializes the Hardware CRC engine by enabling its peripheral clock
 */
void CRC32_init() {
	RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
    __asm__ volatile("dsb"); // Data Synchronization Barrier to ensure clock is active
}

/**
 * @brief Computes the hardware CRC-32 across a block of 32-bit aligned memory
 * @param start_address Pointer to the starting point in Flash
 * @param size_in_bytes Total size of the payload (must be multiple of 4)
 * @return Final calculated uint32_t CRC value
 */
uint32_t CRC32_compute(const void* start_address, size_t size_in_bytes) {
    // 1. Reset the CRC processing unit to seed it with 0xFFFFFFFF
	CRC->CR |= CRC_CR_RESET;

    // 2. Cast the address to 32-bit words
    const uint32_t* current_word = static_cast<const uint32_t*>(start_address);
    size_t total_words = size_in_bytes / 4;

    // 3. Each write operation into the data register creates
	// a combination of the previous CRC value and the new one
    for (size_t i = 0; i < total_words; ++i) {

    	CRC->DR = current_word[i];
    }

    // 4. Return the final evaluation
    return CRC->DR;
}
