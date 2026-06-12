#pragma once

#include <stm32f469xx.h>

// This magical macro checks if a C++ compiler is reading this file
#ifdef __cplusplus
extern "C" {
#endif

void SysClockConfig (void);

void activateFPU(void);

void GPIO_Config (void);

void InterruptGPIO_Config (void);

void USB_OTG_FS_Init(void);

#ifdef __cplusplus
}
#endif

