#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#define UART2_DEFAULT_BAUD 115200U
#define UART2_MIN_BAUD 1200U
#define UART2_MAX_BAUD 2000000U

uint32_t AppConfig_LoadBaud(void);
uint8_t AppConfig_SaveBaud(uint32_t baud);

#endif
