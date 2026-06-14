/* 		  STM32F469
*******   UART3 - C++ Bare-Metal Driver - Interrupts and DMA based  **
*******   with COBS option
********  See usage example for IT and DMA in main.c  ****************/

#include "../inc/uart3.hpp"   // Always include its own header first

#include "timers.h"  // For GetSysTick() timing guards
// C++ Library implementations used inside the driver logic
#include <cstring>    // For std::memcpy
#include <algorithm>  // For std::min


//#define USE_COBS
#if defined(USE_COBS)
#include "cobs.h"
uint8_t encodeBuffer[2] = {0}; // COBS
#endif


// ============================================================================
// HARDWARE ISOLATION LAYER
// ============================================================================

// Handles the strict, unchangeable hardware connections for UART3, acts as a pin guard so that other instances cannot steal these pins
static void USART3_LowLevelInit(void) {
    // Enable USART3 and GPIOB Clocks
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

    // Configure Pin PB10 (TX) and PB11 (RX) for Alternate Function 7
    GPIOB->MODER   |= (2 << GPIO_MODER_MODER10_Pos)   | (2 << GPIO_MODER_MODER11_Pos);
    GPIOB->OSPEEDR |= (3 << GPIO_OSPEEDR_OSPEED10_Pos) | (3 << GPIO_OSPEEDR_OSPEED11_Pos);
    GPIOB->AFR[1]  |= (7 << GPIO_AFRH_AFSEL10_Pos)    | (7 << GPIO_AFRH_AFSEL11_Pos);

    // Enable DMA1 Clock
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
}

// Pass the hardware attributes and function pointer directly into the compile-time configuration instance
inline constexpr UartHardwareConfig Uart3Config {
	USART3, 			// .usart
	DMA1_Stream1, 		// .rxStream
	DMA1_Stream3, 		// .txStream
	4, 					// .dmaChannel
	DMA1, 				// .dmaBase
	USART3_IRQn, 		// .usartIrq
	DMA1_Stream3_IRQn,  // .txDmaIrq
	USART3_LowLevelInit // .lowLevelInit - hard-bound by construction
};


// ============================================================================
// DRIVER CORE IMPLEMENTATION
// ============================================================================

BareM_StatusTypeDef UartDriver::init(uint32_t baudrate) {
    // 1. Execute the hard-bound low-level clock/pin configuration
    if (config.lowLevelInit != nullptr) {
        config.lowLevelInit();
    }

    // 2. Configure USART Word Length (8-bits) & Enable Peripheral
    config.usart->CR1 |= USART_CR1_UE;
    config.usart->CR1 &= ~USART_CR1_M;

    // 3. Calculate and Set Baud Rate cleanly using integer math (No math.h / float needed)
    uint32_t pclk = (config.usart == USART1 || config.usart == USART6) ? 90000000 : 45000000;
    uint32_t usartdiv = (pclk + (baudrate / 2)) / baudrate;
    config.usart->BRR = usartdiv;

    // 7. Configure NVIC Interrupt Vectors using constants from Config
    NVIC_SetPriority(config.usartIrq, 4);
    NVIC_EnableIRQ(config.usartIrq);

    // 8. Finalize Hardware Enables & Set Default Driver Flags
    config.usart->CR1 |= USART_CR1_RE | USART_CR1_TE;

    return Bare_OK;
}

void UartDriver::ConfigureDma() {
    if (this->isDmaInitialized) return; // Exit immediately if already run once

    // 1. Configure DMA RX Stream (Circular buffer routing)
    config.rxStream->CR &= ~DMA_SxCR_EN;
    while (config.rxStream->CR & DMA_SxCR_EN);

    config.rxStream->CR |= (config.dmaChannel << DMA_SxCR_CHSEL_Pos)
                        | DMA_SxCR_PL_0   // Priority: Medium
                        | DMA_SxCR_MINC   // Memory Increment Mode enabled
                        | DMA_SxCR_CIRC   // Circular Mode enabled
                        | DMA_SxCR_TCIE;   // Interrupt at byte 1024
                        //| DMA_SxCR_HTIE;  // Interrupt at byte 512

    // 2. Configure DMA TX Stream (Normal transmission mode)
    config.txStream->CR &= ~DMA_SxCR_EN;
    while (config.txStream->CR & DMA_SxCR_EN);

    config.txStream->CR |= (config.dmaChannel << DMA_SxCR_CHSEL_Pos)
                        | DMA_SxCR_PL_0   // Priority: Medium
                        | DMA_SxCR_MINC   // Memory Increment Mode enabled
                        | DMA_SxCR_DIR_0  // Direction: Memory-to-peripheral
                        | DMA_SxCR_TCIE;  // Transfer Complete Interrupt Enable

    NVIC_SetPriority(config.txDmaIrq, 5);
    NVIC_EnableIRQ(config.txDmaIrq);

    // 3. Mark as initialized so this blocks never executes again
    this->isDmaInitialized = true;
}

UartDriver uart3(Uart3Config); // Global Instance allocation in the memory section (uart3 is extern)

// ============================================================================
// Functions DMA MODE
// ============================================================================

BareM_StatusTypeDef UartDriver::UART_Transmit_DMA(std::string_view message) {
	// std::string_view automatically calculates the length and capture the pointer at zero runtime cost
	ConfigureDma();

    // Wait until the TX DMA stream is disabled and UART hardware shift register is empty (crucial)
    while ((config.txStream->CR & DMA_SxCR_EN) || !(config.usart->SR & USART_SR_TC));

    config.dmaBase->LIFCR = (0x3DUL << 22); // Clear out hardware flags from the previous transmission
    config.usart->SR &= ~USART_SR_TC; // Clear UART transmission complete flag

    uart3.status = Uart::BusyTx;
    uart3.currentMode = UartMode::DMA;
    uint16_t final_tx_length = 0;

#if defined(USE_COBS)
    // Calculate the maximum safe raw input allowed based on your BufferSize limits
    // For a BufferSize of 1024, max raw input is 1021 bytes to guarantee the encoded result fits.
    constexpr size_t maxRawInputSize = BufferSize - (BufferSize / 1021 + 1) - 1;
    size_t length_to_encode = std::min(message.size(), maxRawInputSize);

    // Encode directly from the string_view data into your permanent asynchronous class buffer
    cobs_encode_result encode_result = cobs_encode(
        txBuffer_DMA,
        BufferSize,
        reinterpret_cast<const uint8_t*>(message.data()),
        length_to_encode
    );

    if (encode_result.status != COBS_ENCODE_OK) {
        this->status = Uart::Idle; // Reset state machine on error
        return Bare_ERROR;
    }
    // Append the mandatory framing delimiter zero marker at the end of the packet frame
    if (encode_result.out_len < BufferSize) {
        txBuffer_DMA[encode_result.out_len] = 0x00;
        final_tx_length = static_cast<uint16_t>(encode_result.out_len + 1);
    } else {
        this->status = Uart::Idle;
        return Bare_ERROR; // Out of bounds safety check
    }

#else
    // Standard Transmission Layer (No COBS modification)
    size_t length = std::min(message.size(), BufferSize);
    // Because DMA transmissions happen asynchronously in the background while the CPU moves on,
    // you cannot point the DMA directly to message.data(). If you do, the DMA might transmit corrupted stack memory
    std::memcpy(txBuffer_DMA, message.data(), length); // Decouples the memory by copying the string into a dedicated, permanent internal class buffer
    final_tx_length = static_cast<uint16_t>(length);
#endif

    // The internal config structural elements replace the local pointers entirely
    config.txStream->PAR  = reinterpret_cast<uint32_t>(&config.usart->DR);
    config.txStream->M0AR = reinterpret_cast<uint32_t>(txBuffer_DMA);
    config.txStream->NDTR = final_tx_length;

    // Direct registry access with no runtime conditional dependencies
    config.dmaBase->LIFCR = (0x3DUL << 16); // The mask 0x3DUL clears all 5 flags completely
	config.usart->CR3 	|= USART_CR3_DMAT;
	config.usart->CR1 	|= USART_CR1_TE;
    config.txStream->CR |= DMA_SxCR_EN;

    return Bare_OK;
}

BareM_StatusTypeDef UartDriver::startReceiveToIdle_DMA(std::span<uint8_t> user_buffer) {
	if (user_buffer.empty()) return Bare_ERROR;
	ConfigureDma();

    uint32_t timeout_counter = GetSysTick();
    config.rxStream->CR &= ~DMA_SxCR_EN; // Safely disable the RX DMA stream to reconfigure it

    // Wait for hardware to fully flush and disable the stream (Max 5ms)
    while (config.rxStream->CR & DMA_SxCR_EN || !(config.usart->SR & USART_SR_TC)) {
        if (GetSysTick() - timeout_counter > 5) {
            return Bare_TIMEOUT;
        }
    }
    // Bind the runtime buffer destinations
    this->pRxUserBuffer_DMA = user_buffer.data();
    this->rxMaxLen_DMA = static_cast<uint16_t>(user_buffer.size());

    // Update modern C++ driver state machine
    this->status = Uart::BusyRx;
    this->currentMode = UartMode::DMA;
    this->last_rx_read_index = 0;

    // Configure memory addresses and buffer lengths natively via span
    config.rxStream->PAR  = reinterpret_cast<uint32_t>(&config.usart->DR);
    config.rxStream->M0AR = reinterpret_cast<uint32_t>(this->pRxUserBuffer_DMA);
    config.rxStream->NDTR = rxMaxLen_DMA; // size of the user-provided buffer

    // Clear hardware flags for Stream 1 (CTCIF1, CHTIF1, CTEIF1, CDMEIF1, CFEIF1)
    config.dmaBase->LIFCR = (0x3DUL << 6);
    config.usart->CR3 |= USART_CR3_DMAR; // DMA mode is enabled for reception
    config.usart->CR1 |= USART_CR1_RE | USART_CR1_IDLEIE; // Enable receiver and IDLE line interrupt

    // Fire up the RX engine
    config.rxStream->CR |= DMA_SxCR_EN;

    return Bare_OK;
}

//  Asynchronous Deferred Processor Executed inside ISR Context
void UartDriver::invalidateAndFlushRx() {
    if (rxCallback_DMA == nullptr) return;

    uint16_t current_dma_index = getRxBufferIndex(); // Snapshot the current hardware position (NDTR) first

    if (current_dma_index > last_rx_read_index) {
        // Direct linear data segment invocation
    	rxCallback_DMA(std::span<const uint8_t>(&pRxUserBuffer_DMA[last_rx_read_index], current_dma_index - last_rx_read_index));
    }
    else if (current_dma_index < last_rx_read_index) {
        // Hardware buffer wrap-around handling
    	rxCallback_DMA(std::span<const uint8_t>(&pRxUserBuffer_DMA[last_rx_read_index], rxMaxLen_DMA - last_rx_read_index));
        if (current_dma_index > 0) {
            rxCallback_DMA(std::span<const uint8_t>(&pRxUserBuffer_DMA[0], current_dma_index));
        }
    }
    last_rx_read_index = current_dma_index; // Update the pointer strictly to the snapshot above
    // If they are equal, nothing happens, indices stay exactly where they are!
}


BareM_StatusTypeDef UartDriver::stopReceiveToIdle_DMA() {
    uint32_t timeout_counter = GetSysTick();

    config.usart->CR1 &= ~USART_CR1_IDLEIE; // 1. Disable the IDLE Line Interrupt first so it doesn't fire during shutdown
    config.rxStream->CR &= ~DMA_SxCR_EN; // 2. Disable the RX DMA Stream
    // 3. Wait for hardware to fully flush and acknowledge the disabled state (Max 5ms)
    while (config.rxStream->CR & DMA_SxCR_EN) {
        if (GetSysTick() - timeout_counter > 5) {
            return Bare_TIMEOUT; // Hardware stream is jammed
        }
    }
    config.usart->CR3 &= ~USART_CR3_DMAR;  // Disconnect the USART receiver from the DMA engine
    config.dmaBase->LIFCR = (0x3DUL << 6); // Clear all DMA hardware status flags for this stream (Stream 1 = bits [11:6])

    volatile uint32_t dummy_sr = config.usart->SR;  // Clear the UART internal IDLE flag line by reading SR then DR
    volatile uint32_t dummy_dr = config.usart->DR;
    (void)dummy_sr; (void)dummy_dr;

    this->status &= ~Uart::BusyRx; // Update software status state
    return Bare_OK;
}

// ============================================================================
// Functions INTERRUPT MODE
// ============================================================================

BareM_StatusTypeDef UartDriver::UART_Transmit_IT(std::string_view message) {
	uint32_t timeout_counter = GetSysTick();
	while (!this->isUartIdle()) {
		if (GetSysTick() - timeout_counter > 5) return Bare_TIMEOUT;
	}

	this->status = Uart::BusyTx;
	this->currentMode = UartMode::Interrupt;
	this->tx_index_IT = 0;

	#if defined(USE_COBS)
	    // Cobs overheads, calculate the maximum safe raw input allowed based on your txInterruptBuffer capacity.
	    // For a buffer size of 1024, max raw input is 1021 bytes to guarantee the encoded result fits.
		constexpr size_t maxRawInputSize = BufferSize - (BufferSize / 1021 + 1) - 1; // BufferSize is fully known at compile time (this-> is not)
	    size_t length_to_encode = std::min(message.size(), maxRawInputSize);

	    // Encode directly from the string_view data into the persistent interrupt buffer
	    cobs_encode_result encode_result = cobs_encode(
	        txInterruptBuffer.data(),
	        txInterruptBuffer.size(),
	        reinterpret_cast<const uint8_t*>(message.data()),
	        length_to_encode
	    );

	    if (encode_result.status != COBS_ENCODE_OK) {
	        this->status = Uart::Idle; // Reset state machine on failure
	        return Bare_ERROR;
	    }

	    // Append the mandatory framing delimiter zero marker at the end of the packet frame
	    if (encode_result.out_len < txInterruptBuffer.size()) {
	        txInterruptBuffer[encode_result.out_len] = 0x00;
	        this->tx_len_IT = static_cast<uint16_t>(encode_result.out_len + 1);
	    } else {
	        this->status = Uart::Idle;
	        return Bare_ERROR; // Out of bounds safety check
	    }
	#else
	    // Standard Transmission Layer (No COBS modification)
	    size_t length = std::min(message.size(), txInterruptBuffer.size()); // Clip length safely to prevent buffer overflows
	    this->tx_len_IT = static_cast<uint16_t>(length);
	    // Modern C++ copy mechanism
	    std::copy_n(reinterpret_cast<const uint8_t*>(message.data()), length, txInterruptBuffer.begin());
	#endif

	config.usart->SR &= ~USART_SR_TC;  // TC flag is cleared before enabling interrupts. This step is important, as TC might still be set from a previous transmission.
	config.usart->CR1 |= USART_CR1_TXEIE; // TXE interrupt enable: An USART interrupt is generated whenever TXE=1 in the USART_SR register. It is cleared by a write to the USART_DR register.
	/* and TXE=1 when idle ! (do not enable TXE interrupt until you have smthg to send. Disable it BEFORE writing the last char to be sent)
	TXE: Transmit data register empty: This bit is set by hardware when the content of the TDR register has been transferred into the shift register. */
	return Bare_OK;
}

BareM_StatusTypeDef UartDriver::UART_Receive_IT(std::span<uint8_t> user_buffer, bool waitIfBusy) {
	if (user_buffer.empty()) return Bare_ERROR;

	while (this->status & Uart::BusyRx) {
		if (waitIfBusy) {
			uint32_t timeout_counter = GetSysTick();
			if (GetSysTick() - timeout_counter > 10) {
				return Bare_TIMEOUT; // Unjams the CPU and reports the issue!
			}
			else return Bare_BUSY; // Act like the ST HAL: Return immediately if busy
		}
	}
	// Bind the runtime buffer destinations
	this->pRxUserBuffer_IT = user_buffer.data();
    this->rxMaxLen_IT = static_cast<uint16_t>(user_buffer.size());

    // Set State Machine Flags
    this->status |= Uart::BusyRx;
    this->currentMode = UartMode::Interrupt;

    // Unmask the hardware interrupts to allow reception now
    config.usart->CR1 |= (USART_CR1_RXNEIE | USART_CR1_IDLEIE);

    return Bare_OK;
}

// ============================================================================
// INTERRUPT SERVICE ROUTINE
// ============================================================================

extern "C" {

void DMA1_Stream1_IRQHandler(void) {		// Rx IRQHandler
    if (DMA1->LISR & DMA_LISR_TCIF1) {
    	DMA1->LIFCR = DMA_LIFCR_CTCIF1; 	// Clear Transfer Complete flag
    	uart3.invalidateAndFlushRx();
    	//uart3.status = Uart::RxComplete | Uart::Idle; // add 16 + 1 to cleared status (=)
    }
    if (DMA1->LISR & DMA_LISR_HTIF1) {
    	DMA1->LIFCR = DMA_LIFCR_CHTIF1;
    	uart3.invalidateAndFlushRx();
    }
}

void DMA1_Stream3_IRQHandler(void) {		// Tx IRQHandler
	if (DMA1->LISR & DMA_LISR_TCIF3) {
		DMA1->LIFCR = DMA_LIFCR_CTCIF3; 	// Clear TX Transfer Complete
		uart3.status = Uart::TxComplete | Uart::Idle; // add 16 + 1 to cleared status (=)
	}
}

void USART3_IRQHandler() {

	if(uart3.currentMode == UartMode::Interrupt)
	{
		// receive UART bytes
		if (uart3.config.usart->SR & USART_SR_RXNE) { // 'Receive register not empty' interrupt; RXNE is cleared by a read to the USART_DR register
			uart3.status |= Uart::BusyRx;
			uart3.pRxUserBuffer_IT[uart3.count_rx_IT] = uart3.config.usart->DR;    // Copy new data into the buffer
			uart3.count_rx_IT = uart3.count_rx_IT + 1;
			if (uart3.count_rx_IT >= uart3.rxMaxLen_IT) uart3.count_rx_IT = 0;  // Prevent overflowing the 1024-byte bufferRx array
		}
		// detect idle line, indicates the last character is received
		else if(uart3.config.usart->SR & USART_SR_IDLE) {
			GPIOD->ODR^=GPIO_ODR_OD4;
			volatile uint32_t dummy = uart3.config.usart->SR;
			dummy = uart3.config.usart->DR; (void)dummy; 	// Clear IDLE flag (Read SR then DR)
			// Turn off interrupts to close the reception window (Matches HAL style)
			uart3.config.usart->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_IDLEIE);
			uart3.status = Uart::RxComplete | Uart::Idle;
			// Execute callback if there is data collected and the pointer is valid
			if (uart3.count_rx_IT > 0 && uart3.rxCallback_IT != nullptr) {
				// Pass exactly the slice of bytes that arrived
				uart3.rxCallback_IT(std::span<const uint8_t>(uart3.pRxUserBuffer_IT, uart3.count_rx_IT));
			}
			uart3.count_rx_IT = 0;
		}
		// send UART bytes
		else if((uart3.config.usart->CR1 & USART_CR1_TXEIE) && (uart3.config.usart->SR & USART_SR_TXE)) {
			if (uart3.tx_index_IT < uart3.tx_len_IT) {
				uart3.status = Uart::BusyTx;
				uart3.config.usart->DR = uart3.txInterruptBuffer[uart3.tx_index_IT]; // Read and assign, then modify and write back safely
				uart3.tx_index_IT = uart3.tx_index_IT + 1;  // Avoid the warning: '++' expression of 'volatile'-qualified type is deprecated
			}
			if (uart3.tx_index_IT == uart3.tx_len_IT)	{
				uart3.config.usart->CR1 |= USART_CR1_TCIE; // TCIE: Transmission complete interrupt enable
				uart3.config.usart->CR1 &= ~USART_CR1_TXEIE;
				// No need to memset/clear the buffer here: tx_len_IT and tx_index_IT guard the index access,
				// leaving old data in memory has zero performance impact or side effects.
			}
		}
		// This bit is set by hw if the transmit of a frame is complete and if TXE is set.
		else if (uart3.config.usart->SR & USART_SR_TC && uart3.config.usart->CR1 & USART_CR1_TCIE) {
			uart3.config.usart->SR &= ~USART_SR_TC; // clear USART_SR_TC;
			uart3.config.usart->CR1 &= ~USART_CR1_TCIE;
			uart3.status = Uart::TxComplete | Uart::Idle;
		}
	}

	if (uart3.currentMode == UartMode::DMA) {
	    if (uart3.config.usart->SR & USART_SR_IDLE) {
	        // Clear IDLE flag safely
	    	volatile uint32_t dummy = uart3.config.usart->SR | uart3.config.usart->DR;
	        (void)dummy;
	        // Flush data to the callback immediately
	        uart3.invalidateAndFlushRx();
	        uart3.status |= Uart::RxComplete;
	    }
	}
	// Aggressive error clearing
	if (uart3.config.usart->SR & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
		GPIOD->ODR ^= GPIO_ODR_OD5;
		volatile uint32_t dummy_sr = uart3.config.usart->SR;
		volatile uint32_t dummy_dr = uart3.config.usart->DR;
		(void)dummy_sr;
		(void)dummy_dr; // Reading SR followed by DR clears all core errors
	}
}

} // extern "C"


