#pragma once

#include <stm32f4xx.h>
#include <stm32f469xx.h>


#ifdef __cplusplus
extern "C" {
#endif

void SysTickDelayMs(int delay);

void TIM2_Init (void);

void TIM3config(uint16_t Timeout);

void PWM_TIM5_Init(void);

void TIM6_Init (uint16_t ms);

void Delay_us_TIM7(uint16_t us);

void TIM8_Init(void);

void SysTick_Init();

void NBdelay_ms(uint32_t ms);

// void SysTick_Handler(void); // defined in CMSIS
uint32_t GetSysTick(void);

#ifdef __cplusplus
}
#endif


extern volatile uint32_t msTicks;
extern volatile uint32_t ms_since_last_packet;
extern volatile uint8_t transfer_in_progress;

