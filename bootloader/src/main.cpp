/**
 * BOOTLOADER FILE
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
#include "crc.hpp"       // Include our unified common CRC driver interface
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
    EXECUTE_FLASH
};

struct PacketHeader {
    uint32_t total_size;
    uint16_t packet_id;
    uint16_t payload_length;
};

struct FlashSectorMap {
    uint8_t sector_number;
    uint32_t start_address;
};

// Packet constants
constexpr uint8_t PACKET_START_BYTE = 0x02;
constexpr uint8_t ACK_BYTE          = 0x06;
constexpr uint8_t NAK_BYTE          = 0x15;

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
inline constexpr uint32_t SECTOR12_START = 0x08100000U; // Eeprom-like sector to store any user data
inline constexpr uint32_t SECTOR12_END   = 0x08104000U; // Boundary limit
inline constexpr uint32_t SECTOR13_START = 0x08104000U; // Eeprom-like sector to store to which Bank the application will run
inline constexpr uint32_t SECTOR13_END   = 0x08108000U;

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
State current_state = State::IDLE_START;
PacketHeader header = {0, 0, 0};
uint8_t payload_buffer[512]; // Matches our max expected packet size
uint32_t incoming_crc = 0;
uint32_t bytes_read = 0;

std::array<uint8_t, 1024> buffer_rx; // Sized to 1024 to easily hold a full 512-byte payload packet + protocol framing

// Keep track of the last erased sector number to make sure we don't accidentally re-erase a sector while streaming multiple 512-byte packets into it
static uint32_t target_bank_start_address = 0;
static int16_t last_erased_sector         = -1;  // -1 means nothing erased yet (uint8_t would be enough but int16_t is better for CPU Word Alignment and Implicit Integer Promotion.
static uint32_t current_flash_address     = 0; // no longer hard-coded (0x08100000U)
static bool target_is_bank2               = true;
static uint32_t tot_fw_bytes_written      = 0; // keep track of the written bits

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
            // Edge Case: If the very first byte is 0xFF, no update ever happened
            if (address == SECTOR13_START) {
                return STATE_RUN_BANK1; // Default to Bank 1
            }

            // Peek backward by 1 byte to check the state
            uint8_t choice = *reinterpret_cast<volatile uint8_t*>(address - 1);

            // Return the choice if valid, otherwise fallback to Bank 1
            if (choice == STATE_RUN_BANK1 || choice == STATE_RUN_BANK2 || choice == STATE_FORCE_UPDATE) {
                return choice;
            }
            return STATE_RUN_BANK1;
        }
        address++; // Move to the next byte slot
    }

    // Fallback if full
    uint8_t choice = *reinterpret_cast<volatile uint8_t*>(SECTOR13_END - 1);
    if (choice == STATE_RUN_BANK1 || choice == STATE_RUN_BANK2 || choice == STATE_FORCE_UPDATE) {
        return choice;
    }
    return STATE_RUN_BANK1;
}

static void record_new_bank_state(uint8_t new_state) {
    uint32_t address = SECTOR13_START;
    flash_unlock();

    while (address < SECTOR13_END) {
        uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

        if (current_byte == 0xFF) {
            // Found the very first available empty byte slot! Write our state here.
            flash_write(address, new_state);
            return;
        }
        address++; // Advance byte by byte
    }
    // Fallback safeguard: If Sector 13 is completely packed full (all 16KB),
    // we must erase it and reset to the beginning.
    flash_erase_sector(13); //
    flash_write(SECTOR13_START, new_state);
}

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
        if (!flash_write(target_address, word)) {
            return false; // Hardware write failure
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
    // 1. Run local CRC32 validation
    CRC32_init();
    uint32_t computed_crc = CRC32_compute(payload_buffer, header.payload_length);

    if (computed_crc != incoming_crc) {
        uart3.UART_Transmit_IT(std::string_view(reinterpret_cast<const char*>(&NAK_BYTE), 1));
        return;
    }

    // 2. Reset tracking fields cleanly if this is the absolute beginning of a transfer
    if (header.packet_id == 1) {
        // Read the persistent state to see which bank is currently active
        uint8_t active_bank = get_active_bank_choice();

        if (active_bank == STATE_RUN_BANK2) {
            target_bank_start_address = BANK1_APP_START_ADDR;
            target_is_bank2 = false; // Bank 2 is active, target Bank 1 for this new update
        } else {
            target_bank_start_address = BANK2_APP_START_ADDR;
            target_is_bank2 = true; // Bank 1 is active (or default), target Bank 2 for this new update
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
        // Safeguard: Protect Sector 12 and 13 (the EEPROM storage space) from being overwritten by an app update
        if (sector_found && target_sector <= 13) {
            sector_found = false;
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

    // 3.4. Finally write the new firmware to the Flash
    std::span<const uint8_t> payload_span(payload_buffer, header.payload_length);

    if (program_packet_to_flash(current_flash_address, payload_span)) {
        current_flash_address += header.payload_length;  		// Advance the flash pointer forward by the actual bytes written
        tot_fw_bytes_written += header.payload_length;  // Track the total written progress

        // 3. Check if this was the absolute last packet of the file
        if (tot_fw_bytes_written >= header.total_size) {
            // The update is 100% complete and verified. Now it is safe to change the boot choice!
            uint8_t bank_choice = (target_is_bank2) ? 0x02 : 0x01;
            record_new_bank_state(bank_choice);

            tot_fw_bytes_written = 0; // Reset counter for the next future update session
        }
        // If the transfer is interrupted, the 0x0A (force update) flag stays active in Sector 13, meaning if the board reboots,
        // it safely stays in the bootloader waiting for you to restart sending the FW instead of jumping into a corrupted application

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
                static uint8_t header_raw[8];
                header_raw[bytes_read++] = byte;

                if (bytes_read == 8) {
                    header.total_size =  ((uint32_t)header_raw[0] << 24) | 	 // Decode total_size (Big-Endian)
                                         ((uint32_t)header_raw[1] << 16) |
                                         ((uint32_t)header_raw[2] << 8)  |
                                                    header_raw[3];
                    header.packet_id = (header_raw[4] << 8) | header_raw[5]; // uart sends in a big endian format (MSB first) then
                    header.payload_length = (header_raw[6] << 8) | header_raw[7]; // we shift them back into a single 16-bit integer

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

// ============================================================================
// DRIVER INTERRUPT SYSTEM CALLBACK HARDWARE INTERFACES
// ============================================================================
void UART_RxCpltCallback_DMA(std::span<const uint8_t> incoming_data) {
     // Process data inside the ISR, not a problem since the PC waits for the ACK after 512 bytes
     // and not other process is supposed to run in parallel
    ParseIncomingStream(incoming_data);
}

void UART_RxCpltCallback_IT([[maybe_unused]] std::span<const uint8_t> incoming_data) {}

// ============================================================================
// CPU HANDOVER & ARCHITECTURAL JUMP SEQUENCER
// ============================================================================
void jump_to_application() {
    uint8_t active_bank = get_active_bank_choice(); // Find the active Bank

    // 1. Enable the SYSCFG peripheral clock so we can modify the memory remap register
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    // 2. Configure the hardware remap bit based explicitly on our Sector 13 state
    if (active_bank == 2) {
        // If Sector 13 says the active firmware is in Bank 2, turn ON the hardware swap
        SYSCFG->MEMRMP |= SYSCFG_MEMRMP_UFB_MODE;
    } else {
        // Default: Leave the swap OFF to run from Bank 1
        SYSCFG->MEMRMP &= ~SYSCFG_MEMRMP_UFB_MODE;
    }

    /* The CPU executes those next few lines directly out of its internal pipeline cache, meaning it doesn't
     * even look at the Flash memory for those immediate instructions. They have already been fetched and decoded
     * into the processor's internal registers a fraction of a microsecond before the Flash map officially changed ! */

    // 4. Safely clean up core peripherals before passing control to the application
    __disable_irq();  // Disable all global interrupts, the application's Reset Handler will re-enable them during its startup
    RCC->AHB1ENR &= ~RCC_AHB1ENR_CRCEN;     // Turn off the CRC peripheral clock
    SysTick->CTRL = 0; // Stop the SysTick timer
    SysTick->VAL  = 0; // Clear current value register

    // 5.When the  bootloader is running, the MSP points to the top of the bootloader's allocated stack space.
    // By explicitly resetting the MSP to app_stack_pointer, the CPU's stack hardware is reset back to the absolute top boundary
    // of the application's designated RAM space, giving the new binary a pristine canvas.
    // Thanks to the hardware remap, our target jump address is ALWAYS 0x08008000
    uint32_t app_stack_pointer = *reinterpret_cast<volatile uint32_t*>(BANK1_APP_START_ADDR);
    __set_MSP(app_stack_pointer); // Overwrites the bootloader's stack pointer with the application's new stack pointer

    // 6. Read the application's Reset Vector address (this is always the second 32-bit word at the application's base address)
    // By storing app_reset_vector in a CPU register (which the compiler naturally does for the function pointer) before you change the stack pointer,
    // the CPU can successfully make the jump even though its RAM context just completely shifted beneath it.
    // app_entry_address is not a pointer but the code works precisely because it takes that raw number and forces the compiler to treat it like a pointer
    uint32_t jump_address = *reinterpret_cast<volatile uint32_t*>(BANK1_APP_START_ADDR + 4); // Memory location where the application's first executable instruction lives

    // 7. Set the Least Significant Bit (LSB) to 1 to satisfy ARM Cortex-M Thumb-state execution rules
    jump_address |= 0x01U;

    // 8. Cast the address to a function pointer and execute the leap!
    AppEntryFunction app_reset_handler = reinterpret_cast<AppEntryFunction>(jump_address);
    app_reset_handler();
}

//===================================================================
// =================== MAIN () ======================================
//===================================================================
int main() {
    // 1. Core hardware initialization
    SysClockConfig(); //
    SysTick_Init();   //
    GPIO_Config();    //

    // 2. Read our persistent flash marker
    uint8_t boot_state = get_active_bank_choice();

    // 3. Condition Check: Only jump if we are NOT forcing an update!
    if (boot_state == STATE_RUN_BANK1 || boot_state == STATE_RUN_BANK2) {
        // jump_to_application() handles remapping internally based on current execution bank
        jump_to_application();
    }

    // 4. BYPASS / FALLBACK: If boot_state was 0x0A (or flash was corrupted),
    // we bypass the jump entirely and wait for the bytes over UART.
    BareM_StatusTypeDef res = uart3.init(115200);
    while(res != Bare_OK);

    uart3.startReceiveToIdle_DMA(buffer_rx);

    while (true) {
        asm("nop");
    }

    return 0;
}


/*
FB_MODE is volatile. It resets to 0 every single time the chip loses power or undergoes a power-on reset.
If we rely only on the volatile FB_MODE bit inside jump_to_application(), then a simple power cycle would make the chip wake up,
default back to Bank 1 mapping, and boot right back into the old Bank 1 application. Your new Bank 2 application would be stranded!

Let’s solve this architectural puzzle together by showing how our Sector 13 persistent memory fixes this problem permanently.
How Sector 13 Saves the Day After a Power Cycle

Because Sector 13 is non-volatile physical flash, its contents survive power cuts, battery drains, and hard resets perfectly.
When you turn on the power, the STM32F469 wakes up, FB_MODE is 0, and the CPU starts executing your bootloader from the beginning of physical Bank 1.
Here is exactly how main() uses Sector 13 to handle a cold power-on start:

    The Power Turns On: The MCU boots natively into Bank 1. main() starts executing.
    The Non-Volatile Check: main() immediately calls get_active_bank_choice(), which reads physical Sector 13.
    The Discovery: Even though the chip just lost power, Sector 13 stubbornly remembers its last state. Let's say it reads 0x02 (meaning Bank 2 contains the active, updated firmware).
    The Volatile Restoration: Inside main(), because active_bank == 2, the bootloader immediately calls jump_to_application().
    Flipping the Bit: Inside jump_to_application(), the code detects it needs to run Bank 2, so it explicitly sets SYSCFG->MEMRMP |= SYSCFG_MEMRMP_FB_MODE;.

By setting the bit right there, the bootloader re-engages the hardware swap on every single boot before handing control to the application. From the user's perspective, it feels like a permanent hardware change, even though the bootloader is secretly running for a microsecond at power-up to configure the steering wheel.

This means:
    If Bank 1 is the active software, the bootloader runs for a microsecond, leaves FB_MODE at 0, and jumps to Bank 1.
    If Bank 2 is the active software, the bootloader runs for a microsecond, forces FB_MODE to 1, and jumps to Bank 2.

The volatile nature of the register is no longer a glitch—it becomes a feature!
It guarantees your bootloader in Bank 1 always gets a chance to wake up first, read the flash memory state, and safely route the processor exactly where it belongs.
*/
