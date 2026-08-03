#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g0xx_hal.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern SPI_HandleTypeDef hspi1;
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart1_tx;
extern DMA_HandleTypeDef hdma_usart2_rx;
extern DMA_HandleTypeDef hdma_usart2_tx;

#define FLASH_CS_GPIO_Port GPIOA
#define FLASH_CS_Pin GPIO_PIN_4

#define POWER_BUTTON_GPIO_Port GPIOA
#define POWER_BUTTON_Pin GPIO_PIN_0

#define RELAY_GPIO_Port GPIOB
#define RELAY_Pin GPIO_PIN_0
#define SOC_POWER_GPIO_Port GPIOB
#define SOC_POWER_Pin GPIO_PIN_1
#define SOC_RESET_GPIO_Port GPIOB
#define SOC_RESET_Pin GPIO_PIN_2
#define SOC_LOADER_GPIO_Port GPIOB
#define SOC_LOADER_Pin GPIO_PIN_10
#define SOC_MASKROM_GPIO_Port GPIOB
#define SOC_MASKROM_Pin GPIO_PIN_11

void Error_Handler(void);
uint64_t App_GetUptimeMilliseconds(void);
uint8_t App_UsingExternalClock(void);

#ifdef __cplusplus
}
#endif

#endif
