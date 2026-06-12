/********************************************************************************************************
 	 	BARE-METAL COMMON PARAMETERS
 	  #INCLUDE THIS FILE INTO THE HEADER FILES WHERE BARE METAL FUNCTIONS ARE USED (I2, SPI, UART...)
******************************************************************************************************/

#pragma once

typedef enum
{
	Bare_OK       = 0x00U,
	Bare_ERROR    = 0x01U,
	Bare_BUSY     = 0x02U,
	Bare_TIMEOUT  = 0x03U

} BareM_StatusTypeDef;


/* This enumeration is defined to standardize return values of the Bare Metal functions
   It is based on HAL_StatusTypeDef. Each file/protocol has its own TypeDef variable.

Example: Checking UART transmission status
HAL_StatusTypeDef status;
status = HAL_UART_Transmit(&huart1, (uint8_t*)data, 10, 100);

if (status == HAL_OK)   // Transmission was successful
else  // Handle error (HAL_ERROR, HAL_BUSY, or HAL_TIMEOUT)

 Besides this, the main functions in the drivers files (I2C, SPI, Uart...) possess often their own internal state variables,
	which are defined locally in the headers. */


