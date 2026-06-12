#pragma once

#include <cstdint>
#include <cstddef>

/**
 * @brief Activates the Hardware CRC calculation clock loop
 */
void CRC32_init();

/**
 * @brief Pushes 32-bit aligned flash data through the STM32 hardware pipeline
 */
uint32_t  CRC32_compute(const void* start_address, size_t size_in_bytes);
