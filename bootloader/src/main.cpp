/**
 * BOOTLOADER FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
 */

#include <string.h>
#include <cstdint>
#include <cstddef>
#include <span>
#include <cstring>
//#include <cstdio>
//#include "memory_map.hpp"
#include "crc.hpp"       // Include our unified common CRC driver interface
#include "stm32f469xx.h"
#include "uart3.hpp"
#include "myConfig.h"
#include "timers.h"


//===================================================================
// =================== UART RECEIVE FW ==============================
//===================================================================

enum class State {
    IDLE_START,
    READ_HEADER,
    READ_DATA,
    READ_CRC,
    EXECUTE_FLASH
};

struct PacketHeader {
    uint16_t packet_id;
    uint16_t payload_length;
};


// Global or static tracking variables
State current_state = State::IDLE_START;
PacketHeader header = {0, 0};
uint8_t payload_buffer[512]; // Matches our max expected packet size
uint32_t incoming_crc = 0;
uint32_t bytes_read = 0;

std::array<uint8_t, 1024> buffer_rx; // Sized to 1024 to easily hold a full 512-byte payload packet + protocol framing

// ============================================================================
// DUAL-BANK FLASH BOUNDARIES & HELPERS
// ============================================================================
inline constexpr uint32_t BANK1_START_ADDR = 0x08008000U; // Since we don't write the FW on the first two sectors
inline constexpr uint32_t BANK2_START_ADDR = 0x08100000U; // Physical Bank 2 Base
inline constexpr uint32_t BANK2_END_ADDR   = 0x08200000U; // Max limit for 1MB Bank

// Packet constants
constexpr uint8_t PACKET_START_BYTE = 0x02;
constexpr uint8_t ACK_BYTE          = 0x06;
constexpr uint8_t NAK_BYTE          = 0x15;
// ============================================================================




struct FlashSectorMap {
    uint8_t sector_number;
    uint32_t start_address;
};

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

// Keep track of the last erased sector number to make sure we don't accidentally re-erase a sector while streaming multiple 512-byte packets into it
static uint32_t target_bank_start_address = 0;
static int16_t last_erased_sector         = -1;  // -1 means nothing erased yet
static uint32_t current_flash_address     = 0; // no longer hard-coded (0x08100000U)
static bool target_is_bank2               = true;

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

/*static void flash_lock() {
    FLASH->CR |= FLASH_CR_LOCK;  // Re-engage the hardware lock guard
}*/

static void flash_erase_sector(uint32_t sector)
{
	if (sector == 0 || sector == 1) return; // Bank 1 Bootloader sectors cannot be erased
	while (FLASH->SR & FLASH_SR_BSY);

    // Clear previous errors by writing 1 to them
    FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;

    FLASH->CR &= ~FLASH_CR_SNB;
    // --- Dual-Bank Adjustment ---
    uint32_t snb_value = sector;
    if (sector >= 12) {
        snb_value = 16 + (sector - 12); // Adjusts 12->16, 13->17, etc., for Bank 2 hardware
    }
    // ----------------------------

    FLASH->CR |= (snb_value << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_SER; // Sector Erase activated
    FLASH->CR |= FLASH_CR_STRT;

    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_SER;
}

static bool flash_write_word(uint32_t address, uint32_t data) {
	while (FLASH->SR & FLASH_SR_BSY);

    // 1. Configure Flash Control Register for Programming (PG)
    // Clear out old PSIZE bits and set PSIZE to 0x10 (32-bit word parallelism)
    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FLASH_CR_PSIZE_1; // 32-bit program parallelism
    FLASH->CR |= FLASH_CR_PG;                   // Activate Flash Programming

    // 2. Perform the actual physical memory write operation via pointer dereference
    *reinterpret_cast<volatile uint32_t*>(address) = data;

    while (FLASH->SR & FLASH_SR_BSY);

    // 3. Deactivate Flash Programming mode
    FLASH->CR &= ~FLASH_CR_PG;

    // 4. Verification Check: Read back the status register to ensure no write errors occurred
    if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR)) != 0) {
        // Clear the errors so the flash peripheral isn't permanently locked up
        FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;
        return false;
    }

    return true;
}
//===================================================================
// =================== UART RECEIVE Functions =======================
//===================================================================


static bool program_packet_to_flash(uint32_t start_address, std::span<const uint8_t> payload) {
    // Ensure our length is a multiple of 4 bytes to avoid partial word writes
    if (payload.size() % 4 != 0) {
        return false;
    }
    uint32_t target_address = start_address;

    // C++20 Range-based loop jumping 4 bytes at a time
    for (size_t offset = 0; offset < payload.size(); offset += 4) {
        uint32_t word = 0;

        // Perfectly safe from both alignment and strict aliasing rules
        std::memcpy(&word, &payload[offset], sizeof(uint32_t));

        // Attempt bare-metal physical write
        if (!flash_write_word(target_address, word)) {
            return false; // Hardware write failure
        }
        // Advance physical flash pointer forward by exactly 1 word (4 bytes)
        target_address += 4;
    }

    return true;
}

void execute_flash_and_respond() {
    // 1. Run local CRC32 validation
    CRC32_init();
    uint32_t computed_crc = CRC32_compute(payload_buffer, header.payload_length);

    if (computed_crc != incoming_crc) {
    	uart3.UART_Transmit_IT(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
    	return;
    }

    // 2. Reset tracking fields cleanly if this is the absolute beginning of a transfer
    if (header.packet_id == 1) {
    	// Get the execution address of this running function
    	uint32_t current_pc = reinterpret_cast<uint32_t>(&execute_flash_and_respond);

    	if (current_pc < BANK2_START_ADDR) {
    		// Bootloader is executing in Bank 1 -> Target Bank 2 for the update
    		target_bank_start_address = BANK2_START_ADDR;
    		target_is_bank2 = true;
    	} else {
    		// Bootloader is executing in Bank 2 -> Target Bank 1 for the update
    		target_bank_start_address = BANK1_START_ADDR; // Physical Bank 1 Base
    		target_is_bank2 = false;
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
    	for (const auto& sector_info : bank2_sectors) {
    		if (packet_end_address >= sector_info.start_address) {
    			target_sector = sector_info.sector_number;
    			sector_found = true;
    		}
    	}
    } else {
    	for (const auto& sector_info : bank1_sectors) {
    		if (packet_end_address >= sector_info.start_address) {
    			target_sector = sector_info.sector_number;
    			sector_found = true;
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
            flash_erase_sector(target_sector);
            last_erased_sector = static_cast<int16_t>(target_sector);
        }
    }

    // 3.4. Write the Payload Buffer safely via our C++20 Loop
    std::span<const uint8_t> payload_span(payload_buffer, header.payload_length);

    if (program_packet_to_flash(current_flash_address, payload_span)) {
        // Advance our flash pointer forward by the actual bytes written
        current_flash_address += header.payload_length;

        // Send ACK (0x06) to pull the next chunk from the PC
        uart3.UART_Transmit_IT(std::string_view(reinterpret_cast<const char*>(&ACK_BYTE), 1));
    } else {
        // Hardware fault during programming
        uart3.UART_Transmit_IT(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
    }

    // Optional: Lock flash here if you want strict safety between packets,
    // but leaving it unlocked until the end of the update session is also completely fine.
    // flash_lock();
}

static void ParseIncomingStream(std::span<const uint8_t> incoming_chunk) {
    if (incoming_chunk.empty()) return;

    for (uint8_t byte : incoming_chunk) {

        switch (current_state) {

            case State::IDLE_START: {
                if (byte == PACKET_START_BYTE) {
                    bytes_read = 0;
                    current_state = State::READ_HEADER;
                }
                break;
            }

            case State::READ_HEADER: {
                static uint8_t header_raw[4];
                header_raw[bytes_read++] = byte;

                if (bytes_read == 4) {
                    header.packet_id = (header_raw[0] << 8) | header_raw[1]; // uart sends in a big endian format (MSB first) then
                    header.payload_length = (header_raw[2] << 8) | header_raw[3]; // we shift them back into a single 16-bit integer
                    bytes_read = 0;
                    current_state = State::READ_DATA;
                }
                break;
            }

            case State::READ_DATA: {
            	GPIOD->ODR ^= GPIO_ODR_OD4; //toggle orange
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

                    current_state = State::EXECUTE_FLASH;
                }
                break;
            }

            case State::EXECUTE_FLASH: {
                // 1. Run local CRC32 on payload_buffer via your 'crc.hpp' interface
                // 2. If it matches incoming_crc -> Write to Bank 2 Flash, advance address pointer
                // 3. Send Handshake Reply back to PC

            	execute_flash_and_respond();

                // Ready to look for the next packet
                current_state = State::IDLE_START;
                break;
            }
        }
    }
}



void UART_RxCpltCallback_DMA(std::span<const uint8_t> incoming_data) {
	 // Process data inside the ISR, not a problem since the PC waits for the ACK after 512 bytes
	 // and not other process is supposed to run in parallel
	ParseIncomingStream(incoming_data);
}

void UART_RxCpltCallback_IT(std::span<const uint8_t> incoming_data) {
	if (incoming_data.empty()) return;
}

//===================================================================
// =================== MAIN () ======================================
//===================================================================

int main() {
    // Attempt verification and switch context to the main firmware application
    //jump_to_application();

    SysClockConfig();
    SysTick_Init();
    GPIO_Config();
    BareM_StatusTypeDef res = uart3.init(115200); // VCOM (USB): specifying baud rate is irrelevant
    while(res != Bare_OK);

    // Start once the DMA Rx - spin up the automatic continuous circular pipeline. The hardware takes over now.
    uart3.startReceiveToIdle_DMA(buffer_rx);

    // The processor should never reach this point
    while (true) {
    	// Spin continuously looking for valid packets inside your UART ring buffer
    	asm("nop");
    }

    return 0;
}
