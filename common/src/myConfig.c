//#include "stm32f469xx.h"
#include "stm32f4xx.h"
#include <myConfig.h>


void activateFPU(void) {

#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
	SCB->CPACR |= ((3UL << 20U)|(3UL << 22U));  /* set CP10 and CP11 Full Access */

	// Enable Lazy Stacking for better ISR performance : When an interrupt (like your TIM7 or TIM8 ISRs) occurs, the processor normally has to save all the FPU registers
	// to the stack. This is slow. Lazy Stacking tells the hardware only to save FPU registers if the ISR actually performs a floating-point operation.
	FPU->FPCCR |= FPU_FPCCR_LSPEN_Msk;
#endif
	// Enabling the hardware bits isn't enough; you must also tell your compiler (GCC, Clang, or Keil) to actually generate FPU instructions instead of using slow software libraries.
	// If you are using GCC (arm-none-eabi-gcc), add these flags to your build command:
	// -mfloat-abi=hard: Uses the hardware FPU for calculations and passing arguments.
	// -mfpu=fpv4-sp-d16: Specifies the specific FPU version on the STM32F4.
}


void SysClockConfig (void)
{
	#define PLL_M 	4
	#define PLL_N 	180
	#define PLL_P 	0  // PLLP = 2

	// 1. ENABLE HSE and wait for the HSE to become Ready
	RCC->CR |= RCC_CR_HSEON;  // RCC->CR |= 1<<16;
	while (!(RCC->CR & RCC_CR_HSERDY));  // while (!(RCC->CR & (1<<17)));

	// 2. Set the POWER ENABLE CLOCK and VOLTAGE REGULATOR
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;  // RCC->APB1ENR |= 1<<28; RCC APB1 peripheral clock enable register (RCC_APB1ENR);
	PWR->CR |= PWR_CR_VOS;  // PWR->CR |= 3<<14; PWR power control register (PWR_CR);

	// 3. Configure the FLASH PREFETCH and the LATENCY Related Settings
	FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_5WS;  // FLASH->ACR = (1<<8) | (1<<9)| (1<<10)| (5<<0);

	// 4. Configure the PRESCALARS HCLK, PCLK1, PCLK2
	RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
	// AHB PR
	RCC->CFGR |= RCC_CFGR_HPRE_DIV1;  // RCC->CFGR &= ~(1<<4); RCC clock configuration register (RCC_CFGR);
	// APB1 PR
	RCC->CFGR |= RCC_CFGR_PPRE1_DIV4;  // RCC->CFGR |= (5<<10);
	// APB2 PR
	RCC->CFGR |= RCC_CFGR_PPRE2_DIV2;  // RCC->CFGR |= (4<<13);

	// 5. Configure the MAIN PLL
	RCC->PLLCFGR = (PLL_M <<0) | (PLL_N << 6) | (PLL_P <<16) | (RCC_PLLCFGR_PLLSRC_HSE);  // (1<<22);

	// 6. Enable the PLL and wait for it to become ready
	RCC->CR |= RCC_CR_PLLON;  // RCC->CR |= (1<<24);
	while (!(RCC->CR & RCC_CR_PLLRDY));  // while (!(RCC->CR & (1<<25)));

	// 7. Select the Clock Source and wait for it to be set
	RCC->CFGR |= RCC_CFGR_SW_PLL;  // RCC->CFGR |= (2<<0);
	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);  // while (!(RCC->CFGR & (2<<2)));


	// ------------- set RTC Wakeup Timer 1Hz ----------------------- //

	PWR->CR |= PWR_CR_DBP; // (1U<<8); Disable backup domain write protection
		while((PWR->CR & PWR_CR_DBP) == 0);
	RCC->BDCR |= RCC_BDCR_BDRST; // (1U<<16); BDRST: Backup domain software reset (1: Resets the entire Backup domain)
	RCC->BDCR &= ~RCC_BDCR_BDRST; // 0: Reset not activated
	RCC->BDCR |= RCC_BDCR_LSEON; //(1U<<0); Enable LSE Clock source and wait until LSERDY bit to set
		while ((RCC->BDCR & RCC_BDCR_LSERDY) == 0); // (1U<<1)
	RCC->BDCR |= RCC_BDCR_RTCSEL; //  (1U<<8) Select LSE as RTC Clock
	RCC->BDCR &= ~(1U<<9); // Select LSE as RTC Clock (2nd bit)
	RCC->BDCR |= (1U<<15); // Enable RTC Clock

	RTC->WPR = 0xCA; // Disable the write protection for RTC registers. After backup domain reset, all the RTC registers
	RTC->WPR = 0x53; // are write-protected. Writing to the RTC registers is enabled by writing a key into the Write Protection register.
	RTC->CR &= ~RTC_CR_WUTE; //(1U<<10); Clear WUTE in RTC_CR to disable the wakeup timer before configuring it
	RTC->ISR = RTC_ISR_WUTWF; // (1U<<2); The wakeup timer values can be changed when WUTE bit is cleared and WUTWF is set. (1: Wakeup timer configuration update allowed)

	// Poll WUTWF until it is set in RTC_ICSR to make sure the access to wakeup autoreload counter and to WUCKSEL[2:0] bits is allowed
	if((RTC->ISR & (1U<<6))==0)
		while((RTC->ISR & RTC_ISR_WUTWF)==0);

	// Configure the Wakeup Timer counter and auto clear value
	RTC->WUTR = 0; // When the wakeup timer is enabled (WUTE set to 1), the WUTF flag is set every (WUT[15:0] + 1) ck_wut cycles
	RTC->PRER = 0xFF;         // Configure the RTC PRER ; Synchronus value set as 255
	RTC->PRER |= (0x7F<<16);    // Asynchronus value set as 127
	RTC->CR |= RTC_CR_WUCKSEL;  // WUCKSEL[2:0]: Configure the clock source; 10x: ck_spre (usually 1 Hz) clock is selected

	EXTI->IMR |= (1U<<22); // Configure and enable the EXTI Line 22 in interrupt mode
	EXTI->RTSR |= (1U<<22); // Rising edge trigger enabled (for Event and Interrupt) for input line 10

	RTC->CR |= RTC_CR_WUTIE; // Wakeup timer interrupt enabled (1U<<14)
	RTC->CR |= RTC_CR_WUTE; // Enable the Wakeup Timer (1U<<10)
	RTC->WPR = RTC_WPR_KEY; // Enable the write protection for RTC registers (0xFF)

	NVIC_SetPriority(RTC_WKUP_IRQn, 10);
	NVIC_EnableIRQ(RTC_WKUP_IRQn);
}


void GPIO_Config (void)
{
	// 1. Enable the GPIO CLOCK
	RCC->AHB1ENR |= (1<<0); // GPIO-A
	RCC->AHB1ENR |= (1<<1); // GPIO-B
	RCC->AHB1ENR |= (1<<3); // GPIO-D
	RCC->AHB1ENR |= (1<<6); // GPIO-G
	RCC->AHB1ENR |= (1<<10); // GPIO-K

	// 2. Set the Pins as OUTPUT / INPUT
	GPIOA->MODER &= ~(3<<0);  // pin PA0(bits 1:0)
	GPIOB->MODER &= ~(3<<24); // PB12 input mode
	// error GPIOG->MODER |= (1<<21);  //
	GPIOG->MODER |= (1<<12);  // pin PG6(bits 13:12) as Output (01) - Green Led
	GPIOK->MODER |= (1<<6);  // pin PK3(bits 7:6) as Output (01)
	GPIOG->MODER &= ~(3<<26);  // pin PG13 as Output
	GPIOG->MODER |= (1<<26);  // pin PG13 as Output
	GPIOD->MODER |= (1<<8);  // pin PD4 as Output (01) - Orange Led
	GPIOD->MODER |= (1<<10);  // pin PD4 as Output (01) - Rouge Led
	GPIOA->MODER &= ~(3U << (8 * 2)); // PA8 in input mode

	// 3. Configure the OUTPUT MODE
	GPIOA->PUPDR &= ~(1<<0); // input, 00: No pull-up, pull-down
	GPIOA->PUPDR &= ~(1<<1); // input, 00: No pull-up, pull-down
	GPIOA->OSPEEDR &= ~(1<<30 | 1<<31);
	GPIOB->PUPDR &= ~(1<<24 | 1<<25); // PB12 input, 00: No pull-up, no pull-down
	GPIOG->OTYPER = 1;
	GPIOG->OSPEEDR = 0;
	GPIOK->OTYPER = 1;
	GPIOK->OSPEEDR = 0;
	GPIOG->OTYPER &= ~(1 << 13); // PG13 Push Pull Output
	GPIOG->OSPEEDR &= ~(3U << (26)); // PG13 Low Speed
	GPIOG->BSRR = (1U << 13); // set the PG13 to #1
	GPIOD->OTYPER &= ~(1 << 4); // orange Led PD4 in P-P
	GPIOD->OTYPER &= ~(1 << 5); // rouge Led PD4 in P-P
	GPIOA->PUPDR &= ~(3U << (8 * 2)); // PA8 01: Pull-up
	GPIOA->PUPDR |= (1 << (8 * 2)); // PA8 01: Pull-up
}


void InterruptGPIO_Config (void)
{
	RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;  // Enable SYSCFGEN: System configuration controller clock enable
	SYSCFG->EXTICR[0] &= ~(0xF<<0); // Select Port A for EXTI0
	EXTI->PR = (1 << 0); // clear pending flag
	EXTI->IMR |= (1<<0);  // Unmask EXTI line 0
	EXTI->RTSR |= (1<<0);  // Enable Rising Edge Trigger for PA0 (Rising trigger selection register (EXTI_RTSR))
	EXTI->FTSR &= ~(1<<0);  // Disable Falling Edge Trigger for PA0 (Falling trigger selection register (EXTI_FTSR))
	NVIC_SetPriority(EXTI0_IRQn, 10);
	NVIC_EnableIRQ(EXTI0_IRQn);
}




