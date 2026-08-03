#ifndef __STM32G0XX_IT_H
#define __STM32G0XX_IT_H

#ifdef __cplusplus
extern "C" {
#endif

void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void EXTI0_1_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif
