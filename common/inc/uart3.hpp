/* STM32F469
******* UART3 - C++ Bare-Metal Driver - Polling, Interrupts and DMA based  **
******* with COBS option
******** See usage example for IT and DMA in main.c  ****************/

#pragma once

#include "stm32f469xx.h"
#include "BareM_Def.h"
// C++ Standard Library elements explicitly used by the class declaration
#include <cstdint>      // For fixed-width integer types (uint8_t, uint32_t)
#include <array>        // For std::array
#include <span>         // For std::span


// Clean, type-safe status definitions using modern C++ enums
enum class UartMode { Polling, Interrupt, DMA };

namespace Uart {
    enum Status : uint8_t {
        Idle       = 0x00, // No bits set
        BusyTx     = 0x01, // Bit 0
        BusyRx     = 0x02, // Bit 1
        TxComplete = 0x04, // Bit 2
        RxComplete = 0x08, // Bit 3
        Error      = 0x10  // Bit 4
    };
}

using UartLowLevelInitFn = void(*)(void); // Function pointer type for hardware-specific low-level setup (GPIO/Clocks)

struct UartHardwareConfig { 	// Compile-time configuration structure holding immutable hardware parameters
    USART_TypeDef* usart;
    DMA_Stream_TypeDef* rxStream;
    DMA_Stream_TypeDef* txStream;
    uint32_t             dmaChannel;
    DMA_TypeDef* dmaBase;
    IRQn_Type            usartIrq;
    IRQn_Type            txDmaIrq;
    UartLowLevelInitFn   lowLevelInit; // Uniquely binds the GPIO setup to the instance
};

// Forward declaration of your hard-coded global callback function
void UART_RxCpltCallback_DMA(std::span<const uint8_t> incoming_data);
void UART_RxCpltCallback_IT(std::span<const uint8_t> incoming_data);

// Type alias for the function pointer signature
using UartRxCallback_DMA = void(*)(std::span<const uint8_t> data);
using UartRxCallback_IT  = void(*)(std::span<const uint8_t> data);

// To enable friendship, declare the ISR functions with C-linkage outside the class body
extern "C" {
    void USART3_IRQHandler(void);
    void DMA1_Stream1_IRQHandler(void);
    void DMA1_Stream3_IRQHandler(void);
}

class UartDriver {

	// The compiler will correctly match these to the extern "C" declarations above.
	friend void USART3_IRQHandler(void); // Grant friendship inside the class body normally
	friend void DMA1_Stream1_IRQHandler(void);
	friend void DMA1_Stream3_IRQHandler(void);

private:
    enum class LinkState : uint8_t {
        Idle,
        Polling,
        Interrupt,
        DMA
    };  // Instead of a single 'currentMode' and 'status', create independent tracking enums

    volatile LinkState tx_link = LinkState::Idle; // Controls the TX wire
    volatile LinkState rx_link = LinkState::Idle; // Controls the RX wire

    const UartHardwareConfig config;  // Permanent, read-only configuration for this specific instance
    bool isDmaInitialized = false;    // Guard flag for single-run execution

    // Internal buffers and trackers
    static constexpr size_t BufferSize = 1024;  // Matching the DMA length (NDTR)
    uint8_t  txBuffer_DMA[BufferSize] = {0};
    uint16_t last_rx_read_index = 0;   // Crucial for Rx-To-Idle tracking

    // Use :: to explicitly bind to the global function outside the class
    UartRxCallback_DMA rxCallback_DMA = ::UART_RxCpltCallback_DMA;
    UartRxCallback_IT  rxCallback_IT  = ::UART_RxCpltCallback_IT;

    std::array<uint8_t, BufferSize> txInterruptBuffer{0}; // Buffers for transmit_IT
    volatile uint16_t tx_len_IT = 0;  // Tracker for transmit_IT
    volatile uint16_t tx_index_IT = 0;  // Tracker for transmit_IT
    volatile uint16_t count_rx_IT = 0;  // counter of bytes received
    uint8_t* pRxUserBuffer_IT = nullptr; // Pointer to user-provided memory
    uint16_t rxMaxLen_IT = 0;           // Maximum capacity of user buffer
    uint8_t* pRxUserBuffer_DMA = nullptr; // Pointer to user-provided memory
    uint16_t rxMaxLen_DMA = 0;

    // Internal pipeline helper methods
    void invalidateAndFlushRx();    // Private helper to execute calculations inside the ISR context swiftly
    void ConfigureDma(); 			// Private hardware helper method

public:
    // Constructor handles direct assignment of the configuration struct on boot
    explicit UartDriver(const UartHardwareConfig& hwConfig) : config(hwConfig) {}

    // Public API Methods
    BareM_StatusTypeDef init(uint32_t baudrate);

    BareM_StatusTypeDef UART_Transmit(std::span<const uint8_t> message, uint32_t timeout_ms);
    BareM_StatusTypeDef UART_Receive(std::span<uint8_t> user_buffer, uint32_t timeout_ms);
    BareM_StatusTypeDef UART_Transmit_IT(std::span<const uint8_t> message);
    BareM_StatusTypeDef UART_Receive_IT(std::span<uint8_t> user_buffer, bool waitIfBusy = true);
    BareM_StatusTypeDef UART_Transmit_DMA(std::span<const uint8_t> message);
    BareM_StatusTypeDef startReceiveToIdle_DMA(std::span<uint8_t> user_buffer); // Pass the callback directly when starting the listener
    BareM_StatusTypeDef stopReceiveToIdle_DMA();

    uint16_t getRxBufferIndex() const { return rxMaxLen_DMA - config.rxStream->NDTR;} // getter for the ISR to access NDTR

    // Clean, public "getter" functions for the application layer
    // Replace lines 112-115 with these updated, link-state tracking getters:
    bool isUartIdle() const {
        return tx_link == LinkState::Idle && rx_link == LinkState::Idle;
    }
    bool isUartBusy() const {
        return tx_link != LinkState::Idle || rx_link != LinkState::Idle;
    }

};

extern UartDriver uart3; // Declares the global driver instance to make it visible to the application
