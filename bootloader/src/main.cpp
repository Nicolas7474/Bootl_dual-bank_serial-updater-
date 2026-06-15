/* Bootloader with firmware update capability on dual-bank flash memory swap
 * APPLICATION FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
 */

// ============================================================================
// SYSTEM INCLUDES & DRIVERS
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <span>
#include <cstring>
#include <array>
#include <string_view>
#include <type_traits>
#include <concepts>
//#include <cstdio>
//#include "memory_map.hpp"
#include "stm32f469xx.h"
#include "uart3.hpp"
#include "myConfig.h"
#include "timers.h"

// ============================================================================
// CONFIGURATIONS, CONSTANTS & PROTOCOL MEMORY LAYOUT
// ============================================================================
enum class State {
    IDLE_START,
    READ_HEADER,
    READ_DATA,
    READ_CRC,
};

struct PacketHeader {
    uint32_t total_size;
    uint16_t packet_id;		 // packet_id increments but we track only the first
    uint16_t payload_length;
    uint32_t total_crc;
};

struct FlashSectorMap {
    uint8_t sector_number;
    uint32_t start_address;
};

// Packet constants
constexpr uint8_t PACKET_START_BYTE = 0x02;
constexpr uint8_t ACK_BYTE          = 0x06;
constexpr uint8_t NAK_BYTE          = 0x15;
constexpr uint8_t EOF_BYTE          = 0x21;
constexpr uint8_t ERR_BYTE          = 0xEE;

// Explicit constants for our boot tracking state machine
constexpr uint8_t STATE_RUN_BANK1      = 1;
constexpr uint8_t STATE_RUN_BANK2      = 2;
constexpr uint8_t STATE_FORCE_UPDATE   = 0x0A;

// Define the function pointer type for jumping to the application entry point
using AppEntryFunction = void(*)(); // in C: typedef void (*AppEntryFunction)(void);

// ============================================================================
// DUAL-BANK FLASH BOUNDARIES & HELPERS
// ============================================================================
inline constexpr uint32_t BANK1_APP_START_ADDR = 0x08008000U; // Since we don't write the FW on the first two sectors
inline constexpr uint32_t BANK2_APP_START_ADDR = 0x08108000U; // Pushed from 12 originally to 14th sector to get symmetry with Bank 1
// Total Isolation: because the linker doesn't know these addresses exist, it will never try to compile code into them.
inline constexpr uint32_t SECTOR12_START = 0x08100000U; // Eeprom-like sector to store any user data
inline constexpr uint32_t SECTOR12_END   = 0x08104000U; // Boundary limit
inline constexpr uint32_t SECTOR13_START = 0x08104000U; // Non-volatile sector that store linearly the Bank number the application currently runs on
inline constexpr uint32_t SECTOR13_END   = 0x08108000U;
inline constexpr uint32_t SECTOR13_MAGIC_ADDR = 0x08107FFCU; // Last 32-bit word of Sector 13
inline constexpr uint32_t SECTOR13_MAGIC_VAL  = 0x1A2B3C4DU; // Your chosen initialization token

// Explicit physical layout of STM32F469 Bank 1
// Sized to 12 sectors total (Sector 0 to 11)
inline constexpr std::array<FlashSectorMap, 12> bank1_sectors{{
    {0,  0x08000000U}, // 16 KB - Bootloader Entry Point / Vector Table
    {1,  0x08004000U}, // 16 KB - Bootloader Continuation Space
    {2,  0x08008000U}, // 16 KB - Historical App Vector / Header Target
    {3,  0x0800C000U}, // 16 KB - Workspace / App Execution
    {4,  0x08010000U}, // 64 KB
    {5,  0x08020000U}, // 128 KB
    {6,  0x08040000U}, // 128 KB
    {7,  0x08060000U}, // 128 KB
    {8,  0x08080000U}, // 128 KB
    {9,  0x080A0000U}, // 128 KB
    {10, 0x080C0000U}, // 128 KB
    {11, 0x080E0000U}  // 128 KB
}}; // The Outer Braces { } initialize the std::array object itself
    //The Inner Braces { } initialize the underlying raw internal C-array hidden inside the class

// Explicit physical layout of STM32F469 Bank 2
// Sized to 12 sectors total (Sector 12 to 23)
inline constexpr std::array<FlashSectorMap, 12> bank2_sectors{{
    {12, 0x08100000U}, // 16 KB
    {13, 0x08104000U}, // 16 KB
    {14, 0x08108000U}, // 16 KB
    {15, 0x0810C000U}, // 16 KB
    {16, 0x08110000U}, // 64 KB
    {17, 0x08120000U}, // 128 KB
    {18, 0x08140000U}, // 128 KB
    {19, 0x08160000U}, // 128 KB
    {20, 0x08180000U}, // 128 KB
    {21, 0x081A0000U}, // 128 KB
    {22, 0x081C0000U}, // 128 KB
    {23, 0x081E0000U}  // 128 KB
}};

// ============================================================================
// GLOBAL STATE VOLATILE & RUNTIME TRACKING VARIABLES
// ============================================================================
// Global or static tracking variables
volatile State current_state = State::IDLE_START;
PacketHeader header = {0, 0, 0, 0};
uint8_t payload_buffer[512]; // Matches our max expected packet size
uint32_t incoming_crc = 0;
uint32_t bytes_read = 0;
constexpr uint32_t INACTIVITY_TIMEOUT_MS = 4000; // 4 seconds of silence = abort

std::array<uint8_t, 1024> buffer_rx{}; // Sized to 1024 to easily hold a full 512-byte payload packet + protocol framing

// Keep track of the last erased sector number to make sure we don't accidentally re-erase a sector while streaming multiple 512-byte packets into it
static uint32_t target_bank_start_address = 0;
static int16_t last_erased_sector         = -1;  // -1 means nothing erased yet (uint8_t would be enough but int16_t is better for CPU Word Alignment and Implicit Integer Promotion.
static uint32_t current_flash_address     = 0; // no longer hard-coded (0x08100000U)
static bool target_is_bank2               = true;
static uint32_t tot_fw_bytes_written      = 0; // keep track of the written bits
static uint32_t expected_firmware_crc = 0; // Global tracking variable

//===================================================================
// =================== Functions Flash ==============================
//===================================================================

static void flash_unlock() {
    // Check if the flash is already unlocked
    if ((FLASH->CR & FLASH_CR_LOCK) != 0) {
        // Authorize flash register access by writing the mandatory key sequence
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
}

static void flash_lock() {
    FLASH->CR |= FLASH_CR_LOCK;  // Re-engage the hardware lock guard
}

static bool flash_erase_sector(uint32_t sector)
{
    if (sector == 0 || sector == 1) return false; // Bank 1 Bootloader sectors cannot be erased
    while (FLASH->SR & FLASH_SR_BSY);
    // Clear previous errors by writing 1 to them
    FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;

    FLASH->CR &= ~FLASH_CR_SNB;
    // --- Dual-Bank Adjustment ---
    uint32_t snb_value = sector;
    if (sector >= 12) {
        snb_value = 16 + (sector - 12); // Adjusts 12->16, 13->17, etc., for Bank 2 hardware
    }

    FLASH->CR |= (snb_value << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_SER; // Sector Erase activated
    FLASH->CR |= FLASH_CR_STRT;

    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_SER;

    // Check if any error flags were pulled high during the erase process
    if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)) != 0) {
    	// Clear flags so the hardware doesn't lock up future attempts
    	FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;
    	return false; // Erase failed
    }

    return true; // Erase succeeded cleanly
}


static uint32_t CRC32_compute(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320; // Reverse polynomial for standard CRC32
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}


// Define a custom C++20 concept to lock down our allowed Flash data types
template <typename T>
concept ValidFlashType = std::same_as<T, uint8_t> || std::same_as<T, uint32_t>;

static bool flash_write(uint32_t address, ValidFlashType auto data) {

    while (FLASH->SR & FLASH_SR_BSY);

    FLASH->CR &= ~FLASH_CR_PSIZE;

    // Changed T to decltype(data)
    if constexpr (std::same_as<decltype(data), uint32_t>) {
        FLASH->CR |= FLASH_CR_PSIZE_1;
    }

    FLASH->CR |= FLASH_CR_PG;

    // Changed T to decltype(data)
    *reinterpret_cast<volatile decltype(data)*>(address) = data;

    while (FLASH->SR & FLASH_SR_BSY);

    FLASH->CR &= ~FLASH_CR_PG;

    if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR)) != 0) {
        FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;
        return false;
    }

    return true;
}

static uint8_t get_active_bank_choice() {
    uint32_t address = SECTOR13_START;

    while (address < SECTOR13_END) {
        uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

        if (current_byte == 0xFF) {
            if (address == SECTOR13_START) {
                return STATE_RUN_BANK1; // Default to Bank 1 if empty
            }

            // Peek backward by 1 byte and return exactly what is there
            return *reinterpret_cast<volatile uint8_t*>(address - 1);
        }
        address++;
    }

    return *reinterpret_cast<volatile uint8_t*>(SECTOR13_END - 1);
}


static void format_sector13_fresh(uint8_t initial_state) {
	flash_unlock();
	// Attempt physical erasure and verify success
	if (!flash_erase_sector(13)) {
		// HARDWARE ERASE FAULT: Stop everything!
		flash_lock();
		uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
		return; // Break out of execution early to prevent programming un-erased memory
	}
	flash_write(SECTOR13_START, initial_state);  // Write the starting bank choice at the very first byte
	flash_write(SECTOR13_MAGIC_ADDR, SECTOR13_MAGIC_VAL);  // Write the magic word at the very last 32-bit word
}

static void record_new_bank_state(uint8_t new_state) {
	// Wear-Leveling byte-by-byte linear scanning strategy inside Sector 13
	uint32_t address = SECTOR13_START;
	flash_unlock();

    while (address < SECTOR13_MAGIC_ADDR) {
        uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

        if (current_byte == 0xFF) {
            // Found the very first available empty byte slot! Write our state here.
            flash_write(address, new_state);
            return;
        }
        address++; // Advance byte by byte
    }
    // Fallback safeguard: if Sector 13 is completely packed full (all 16KB),
    // we must erase it and reset to the beginning
    format_sector13_fresh(new_state);
}


static bool program_packet_to_flash(uint32_t start_address, std::span<const uint8_t> payload) {
    // Ensure our length is a multiple of 4 bytes to avoid partial word writes
    if (payload.size() % 4 != 0) {
        return false;
    }
    uint32_t target_address = start_address;

    // Loop jumping 4 bytes at a time
    for (size_t offset = 0; offset < payload.size(); offset += 4) {
        uint32_t word = 0;

        // Perfectly safe from both alignment and strict aliasing rules
        std::memcpy(&word, &payload[offset], sizeof(uint32_t));

        // Attempt bare-metal physical write
        if (!flash_write(target_address, word)) {
        	return false; // Hardware write failure
        }
        // Immediate read-back verification
        // Directly sample the physical Flash memory address
        uint32_t written_word = *reinterpret_cast<volatile uint32_t*>(target_address);
        if (written_word != word) {
        	uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&ERR_BYTE), 1));
        	return false; // Flash verification mismatch! Silicon/Data corruption detected.
        }
        // Advance physical flash pointer forward by exactly 1 word (4 bytes)
        target_address += 4;
    }

    return true;
}

//===================================================================
// =================== UART RECEIVE FW ENGINE =======================
//===================================================================

void execute_flash_and_respond() {
    // 1. Run local CRC32 validation (STM32 CRC hardware was not working)
    uint32_t computed_crc = CRC32_compute(payload_buffer, header.payload_length);

    if (computed_crc != incoming_crc) {
        uart3.UART_Transmit_IT(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
        return;
    }

    // 2. Reset tracking fields cleanly if this is the absolute beginning of a transfer
    if (header.packet_id == 1) {							// first packet
    	tot_fw_bytes_written = 0; // Reset tracking counter (robustness: avoid accumulating values if transfer has crashed)
    	expected_firmware_crc = header.total_crc; // Capture the total_crc token once at the start
    	uint8_t active_bank = get_active_bank_choice();

    	// If we are currently in a forced update state, find out what the underlying active bank was
    	if (active_bank == STATE_FORCE_UPDATE) {
    		// Find the first 0xFF slot again to step back 2 bytes total
    		uint32_t address = SECTOR13_START;
    		while (address < SECTOR13_END) {
    			if (*reinterpret_cast<volatile uint8_t*>(address) == 0xFF) {
    				break;
    			}
    			address++;
    		}

    		// Step back 2 bytes (past the 0x0A) to get the true active bank
    		if (address >= (SECTOR13_START + 2)) {
    			active_bank = *reinterpret_cast<volatile uint8_t*>(address - 2);
    		} else {
    			active_bank = STATE_RUN_BANK1; // Ultimate fallback
    		}
    	}

    	// Now assign the targets reliably
    	if (active_bank == STATE_RUN_BANK2) {
    		target_bank_start_address = BANK1_APP_START_ADDR;
    		target_is_bank2 = false; // Bank 2 is active, target Bank 1
    	} else {
    		target_bank_start_address = BANK2_APP_START_ADDR;
    		target_is_bank2 = true;  // Bank 1 is active (or default), target Bank 2
    	}

    	current_flash_address = target_bank_start_address;
    	last_erased_sector    = -1;
    }

    // 3. Dynamic Sector Erase Engine
    uint32_t packet_end_address = current_flash_address + header.payload_length - 1;
    uint8_t target_sector = 0;
    bool sector_found = false;

    // 3.2. Scan our lookup table to find which exact sector contains our ending byte
    // Choose the correct sector lookup table based on our target bank
    if (target_is_bank2) {
    	// Search backwards from the highest sector to the lowest
    	for (int i = static_cast<int>(bank2_sectors.size()) - 1; i >= 0; --i) {
    		if (packet_end_address >= bank2_sectors[i].start_address) {
    			target_sector = bank2_sectors[i].sector_number;
    			sector_found = true;
    			break; // Stop immediately! We found the highest matching boundary.
    		}
    	}
    	// Safeguard: Protect Sector 12 and 13 (the EEPROM storage space) from being overwritten by an app update
    	if (sector_found && target_sector <= 13) {
    		sector_found = false;
    	}
    } else {
    	// Search backwards from the highest sector to the lowest
    	for (int i = static_cast<int>(bank1_sectors.size()) - 1; i >= 0; --i) {
    		if (packet_end_address >= bank1_sectors[i].start_address) {
    			target_sector = bank1_sectors[i].sector_number;
    			sector_found = true;
    			break; // Stop immediately! We found the highest matching boundary.
    		}
    	}

    	if (sector_found && target_sector <= 1) {
    		sector_found = false; // Redundant safeguard forcing to false, refusing to erase or write
    	}
    }

    // Always unlock flash right before any modification attempts (Erase OR Write)
    flash_unlock();

    // 3.3 If we successfully mapped the address to a sector, check our erasure tracker
    if (sector_found) {
    	// For Bank 1, sectors go 2 to 11. For Bank 2, they go 12 to 23.
    	// This condition still holds true as long as we process sequentially upward.
    	if (static_cast<int16_t>(target_sector) > last_erased_sector) {
    		// Attempt physical erasure and verify success
    		if (!flash_erase_sector(target_sector)) {
    			// HARDWARE ERASE FAULT: Stop everything!
    			flash_lock();
    			uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
    			return; // Break out of execution early to prevent programming un-erased memory
    		}
    		last_erased_sector = static_cast<int16_t>(target_sector);
    	}
    }

    // 3.4. Finally write the new firmware to the Flash
    std::span<const uint8_t> payload_span(payload_buffer, header.payload_length);

    if (program_packet_to_flash(current_flash_address, payload_span)) {
    	current_flash_address += header.payload_length;  		// Advance the flash pointer forward by the actual bytes written
    	tot_fw_bytes_written += header.payload_length;  // Track the total written progress

    	// 3. Check if this was the absolute last packet of the file
    	if (tot_fw_bytes_written >= header.total_size) {

    		// Compute CRC over the entire physical flashed binary space
    		uint32_t physical_flash_crc = CRC32_compute(reinterpret_cast<const uint8_t*>(target_bank_start_address), header.total_size);

    		if (physical_flash_crc == expected_firmware_crc) {
    			// The update is 100% complete, written and verified. Now it is safe to change the boot choice
    			uint8_t bank_choice = (target_is_bank2) ? 0x02 : 0x01;
    			record_new_bank_state(bank_choice); // update the new bank number the app will run onto
    			flash_lock();
    			tot_fw_bytes_written = 0; // Reset counter for the next future update session
    			transfer_in_progress = 0;
    			uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&ACK_BYTE), 1));
    			// Clean the hardware pipeline before leaving
    			while ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0); // Crucial: wait for DMA Stream to turn off
    			while ((USART3->SR & USART_SR_TC) == 0);  // Wait for USART Transmission Complete (TC) flag
    			__disable_irq(); // Nothing can interrupt the reboot
    			NVIC_SystemReset(); // Reboot
    		} else {
    			// MACRO IMAGE CORRUPTION DETECTED!
    			// Do NOT write the new bank state to Sector 13.
    			flash_lock();
    			tot_fw_bytes_written = 0;
    			transfer_in_progress = false;
    			// Blast back a NAK or an explicit ERR_BYTE so Python flags a flashing failure
    			uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&ERR_BYTE), 1));
    			return;
    		}
    	}
    	// If the transfer is interrupted, the 0x0A (force update) flag stays active in Sector 13, meaning if the board reboots,
    	// it safely stays in the bootloader waiting for you to restart sending the FW instead of jumping into a corrupted application
    	// Send ACK (0x06) to pull the next chunk from the PC
    	uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&ACK_BYTE), 1));
    } else {
    	// Hardware fault during programming
    	uart3.UART_Transmit_DMA(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
    }
}

static void ParseIncomingStream(std::span<const uint8_t> incoming_chunk) {
	if (incoming_chunk.empty()) return;

	for (uint8_t byte : incoming_chunk) {

		switch (current_state) {

		case State::IDLE_START: {
			if (byte == PACKET_START_BYTE) {
				bytes_read = 0;
				transfer_in_progress = 1; // Mark that we are alive
				ms_since_last_packet = 0;    // Reset inactivity countdown
				current_state = State::READ_HEADER;
			}
			break;
		}

		case State::READ_HEADER: {
			static uint8_t header_raw[12];
			header_raw[bytes_read++] = byte;

			if (bytes_read == 12) {
				header.total_size	 =  ((uint32_t)header_raw[0] << 24) | 	 // Decode total_size (Big-Endian)
										((uint32_t)header_raw[1] << 16) |
										((uint32_t)header_raw[2] << 8)  |
										header_raw[3];
        		header.packet_id	  = (header_raw[4] << 8) | header_raw[5]; // uart sends in a big endian format (MSB first) then
        		header.payload_length = (header_raw[6] << 8) | header_raw[7]; // we shift them back into a single 16-bit integer
        		header.total_crc	  = ((uint32_t)header_raw[8]  << 24) |
										((uint32_t)header_raw[9]  << 16) |
										((uint32_t)header_raw[10] << 8)  |
										  header_raw[11];

        		// DEFENSIVE SAFEGUARD: Prevent malicious or malformed packets from crashing RAM
        		if (header.payload_length > 512) {
        			header.payload_length = 512;
        		}

        		bytes_read = 0;
        		current_state = State::READ_DATA;
        	}
        	break;
        }

        case State::READ_DATA: {
        	payload_buffer[bytes_read++] = byte;

        	if (bytes_read == header.payload_length) {
        		bytes_read = 0;
        		current_state = State::READ_CRC;
        	}
        	break;
        }

        case State::READ_CRC: {
        	static uint8_t crc_raw[4];
        	crc_raw[bytes_read++] = byte;

        	if (bytes_read == 4) {
        		incoming_crc = ((uint32_t)crc_raw[0] << 24) |
        				((uint32_t)crc_raw[1] << 16) |
						((uint32_t)crc_raw[2] << 8)  |
						crc_raw[3];

        		execute_flash_and_respond();  // Process the flash operations immediatly
        		bytes_read = 0;
        		current_state = State::IDLE_START; // Reset variables for the next incoming packet
        	}
        	break;
        }
        }
    }
}

// ============================================================================
// DRIVER INTERRUPT SYSTEM CALLBACK HARDWARE INTERFACES
// ============================================================================
void UART_RxCpltCallback_DMA(std::span<const uint8_t> incoming_data){
     // Process data inside the ISR, not a problem since the PC waits for the ACK after 512 bytes
     // and not other process is supposed to run in parallel
    ParseIncomingStream(incoming_data);

/*    std::string_view tx_view(
            reinterpret_cast<const char*>(incoming_data.data()),
            incoming_data.size()
        );*/

    //uart3.UART_Transmit_DMA(tx_view);
}

void UART_RxCpltCallback_IT([[maybe_unused]] std::span<const uint8_t> incoming_data) {}

// ============================================================================
// CPU HANDOVER & ARCHITECTURAL JUMP SEQUENCER
// ============================================================================
// Force this function to run entirely out of RAM
__attribute__((section(".RamFunc"), noinline))
static void jump_to_application(uint32_t target_app_addr) {

    // 1. CLEAN HARDWARE DE-INITIALIZATION
    // Forcefully disable the global interrupts at the core level first
    __disable_irq();
    // Disable SysTick completely and clear its pending exception state
    SysTick->CTRL = 0;
    SysTick->VAL  = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;
    // Stop and clear UART3 + DMA configuration registers
    // (Adjust these register resets to match your custom driver teardown)
    USART3->CR1 &= ~USART_CR1_UE;  // Disable USART3 globally
    USART3->CR3 &= ~(USART_CR3_DMAR | USART_CR3_DMAT); // Sever DMA linkages
    // Force clear the specific NVIC Enable and Pending registers used
    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
    // If UART3 interrupts were mapped:
    NVIC_DisableIRQ(USART3_IRQn);
    NVIC_ClearPendingIRQ(USART3_IRQn);
    // Optional but safest bare-metal approach: Reset peripheral clocks completely
    // This forces the hardware modules back to their factory reset state
    RCC->APB1RSTR |= RCC_APB1RSTR_USART3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USART3RST;

    // 2. HARDWARE SWAP AND EXECUTION HANDOFF
    uint8_t active_bank = get_active_bank_choice();

    if (active_bank == 2) {
        SYSCFG->MEMRMP |= SYSCFG_MEMRMP_UFB_MODE;
    } else {
        SYSCFG->MEMRMP &= ~SYSCFG_MEMRMP_UFB_MODE;
    }
    __DSB();
    __ISB();

    uint32_t app_stack_pointer = *reinterpret_cast<volatile uint32_t*>(target_app_addr);
    uint32_t jump_address      = *reinterpret_cast<volatile uint32_t*>(target_app_addr + 4);
    jump_address |= 0x01U;

    __set_MSP(app_stack_pointer);

    AppEntryFunction app_reset_handler = reinterpret_cast<AppEntryFunction>(jump_address);
    app_reset_handler();
}

// function just in case we need to force an update byte in sector 13
[[maybe_unused]] static void write_0xA() {
    uint32_t address = 0x08104000U; // Sector 13 start address
    constexpr uint32_t sector_boundary = 0x08107FFCU;

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
}

static void GPIO__Interrupt() {
    // 1. Enable GPIOA Clock (and SYSCFG clock for interrupt routing)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    // 2. Set PA0 as Input Mode (00 in MODER)
    //GPIOA->MODER &= ~GPIO_MODER_MODER0;

    // 3. Clear and map EXTI0 line to Port A (0000 in EXTICR[0])
    SYSCFG->EXTICR[0] &= ~(0xF<<0);

    // 4. Configure EXTI Line 0 (Unmask + Set Rising Edge Trigger)
    EXTI->IMR  |= EXTI_IMR_IM0;     // Unmask interrupt line 0
    EXTI->RTSR |= EXTI_RTSR_TR0;    // Trigger on rising edge (button press)
    EXTI->FTSR &= ~EXTI_FTSR_TR0;   // Disable falling edge trigger

    // 5. Enable the Interrupt in the NVIC
    NVIC_SetPriority(EXTI0_IRQn, 2); // Set lower priority than critical UART/DMA
    NVIC_EnableIRQ(EXTI0_IRQn);
}
//===================================================================
// =================== MAIN () ======================================
//===================================================================
int main() {
	// 1. Core hardware initialization
	SysClockConfig(); //
	SysTick_Init();   //
	GPIO_Config();  //
	GPIOG->ODR ^= GPIO_ODR_OD6; // disable green led
	GPIOD->ODR ^= GPIO_ODR_OD5;  // disable rouge

	//write_0xA(); // reset the bootloader in "waiting for update file..." state
	//record_new_bank_state(0x01); // manually set the application bank
	// format_sector13_fresh(1);
	// Fast autonomous magic check: read the absolute end of the sector. If it's not our token, reset the sector
	if (*reinterpret_cast<volatile uint32_t*>(SECTOR13_MAGIC_ADDR) != SECTOR13_MAGIC_VAL) {
		// The sector 13 must be initialized at 0xFF at the first use after programming
		format_sector13_fresh(STATE_RUN_BANK1); // Force a clean format (bank x [...0xFF...0xFF...] magic_nb)
	}

	uint8_t boot_state = get_active_bank_choice(); // 2. Read our persistent flash marker

	// 3. Condition Check: Only jump if we are NOT forcing an update!
	if (boot_state == STATE_RUN_BANK1 || boot_state == STATE_RUN_BANK2) {
		jump_to_application(BANK1_APP_START_ADDR);
	}

	else if(boot_state == STATE_FORCE_UPDATE) {
		GPIO__Interrupt();
		// 4.  Wait for firmware update
		BareM_StatusTypeDef res = uart3.init(115200);
		while(res != Bare_OK);

		uart3.startReceiveToIdle_DMA(buffer_rx);

		while (true) {
			// Active watchdog check for mid-transfer abandonment
			if (transfer_in_progress && (ms_since_last_packet > INACTIVITY_TIMEOUT_MS)) {
				while ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0); // Crucial: wait for DMA Stream to turn off
				while ((USART3->SR & USART_SR_TC) == 0);  // Wait for USART Transmission Complete (TC) flag
				__disable_irq();
				NVIC_SystemReset(); // Purge state and reboot cleanly
			}

			GPIOD->ODR ^= GPIO_ODR_OD4;
			NBdelay_ms(150); // blink wait to receive the uart bytes

		}
		return 0;
	}
}

extern "C" void EXTI0_IRQHandler(void) {
    // Check if the interrupt came from line 0
	if (EXTI->PR & (1<<0)) {  // button pushed : if the PA0 triggered the interrupt

        // Safety check: Only escape if we are actively waiting for an update file
        // (This prevents accidental button presses from wiping states at wrong times)
        uint8_t current_boot_state = get_active_bank_choice();

        if (current_boot_state == STATE_FORCE_UPDATE) {

            // 1. Find the last running valid bank choice
            uint8_t fallback_bank = STATE_RUN_BANK1; // Default
            uint32_t address = SECTOR13_START;

            while (address < SECTOR13_END) {
                if (*reinterpret_cast<volatile uint8_t*>(address) == 0xFF) {
                    break;
                }
                address++;
            }

            // Step back 2 bytes (past the 0x0A) to find the old working bank
            if (address >= (SECTOR13_START + 2)) {
                fallback_bank = *reinterpret_cast<volatile uint8_t*>(address - 2);
            }

            record_new_bank_state(fallback_bank); // Write Sector 13 to store the fallback bank byte after the 0x0A

            // 3. Clean down and reboot
            __disable_irq(); // Prevent other interrupts from breaking the restart
            EXTI->PR = EXTI_PR_PR0; // Clear pending flag just in case
            while ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0); // Crucial: wait for DMA Stream to turn off
            while ((USART3->SR & USART_SR_TC) == 0);  // Wait for USART Transmission Complete (TC) flag
            NVIC_SystemReset(); // Reboot directly out of the loop!
        }
        EXTI->PR = (1<<0);  // Clear the interrupt flag by writing a 1
    }
}


/*
 Every time the board powers up or undergoes a hard reset, your code executes this exact sequence:
    The Magic Check: The CPU checks a single address (0x08107FFC).
    The Self-Heal (Day 1): If the board just came off the factory assembly line and is filled with noise/zeros,
    the magic number isn't there. The bootloader immediately calls format_sector13_fresh(),
    erasing the entire sector to a pristine physical 0xFF state, stamps the first byte with 0x01 (Bank 1), and seals the end with 0x1A2B3C4D.
    The Instant Jump: If the magic number is there (Normal Day 2+ operation), it skips the formatting entirely,
    reads the current active bank choice from your sequential list, sets the SYSCFG->MEMRMP hardware mapping steering wheel,
    resets the stack pointer, and leaps into your main application in less than a microsecond.

UFB_MODE is volatile. It resets to 0 every single time the chip loses power or undergoes a power-on reset.
If we rely only on the volatile UFB_MODE bit inside jump_to_application(), then a simple power cycle would make the chip wake up,
default back to Bank 1 mapping, and boot right back into the old Bank 1 application. Your new Bank 2 application would be stranded!

-> How Sector 13 Saves the Day After a Power Cycle
Because Sector 13 is non-volatile physical flash, its contents survive power cuts, battery drains, and hard resets perfectly.
When you turn on the power, the STM32F469 wakes up, UFB_MODE is 0, and the CPU starts executing your bootloader from the beginning of physical Bank 1.
Here is how main() uses Sector 13 to handle a cold power-on start:

    The Power Turns On: The MCU boots natively into Bank 1. main() starts executing.
    The Non-Volatile Check: main() immediately calls get_active_bank_choice(), which reads physical Sector 13.
    The Discovery: Even though the chip just lost power, Sector 13 stubbornly remembers its last state. Let's say it reads 0x02 (meaning Bank 2 contains the active, updated firmware).
    The Volatile Restoration: Inside main(), because active_bank == 2, the bootloader immediately calls jump_to_application().
    Flipping the Bit: Inside jump_to_application(), the code detects it needs to run Bank 2, so it explicitly sets SYSCFG->MEMRMP |= SYSCFG_MEMRMP_UFB_MODE;.

By setting the bit right there, the bootloader re-engages the hardware swap on every single boot before handing control to the application. From the user's perspective, it feels like a permanent hardware change, even though the bootloader is secretly running for a microsecond at power-up to configure the steering wheel.

This means:
    If Bank 1 is the active software, the bootloader runs for a microsecond, leaves UFB_MODE at 0, and jumps to Bank 1.
    If Bank 2 is the active software, the bootloader runs for a microsecond, forces UFB_MODE to 1, and jumps to Bank 2.

The volatile nature of the register is no longer a glitch—it becomes a feature!
It guarantees your bootloader in Bank 1 always gets a chance to wake up first, read the flash memory state, and safely route the processor exactly where it belongs.
*/
