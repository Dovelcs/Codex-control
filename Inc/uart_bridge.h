#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include <stdint.h>

typedef enum
{
  BRIDGE_MODE_CLI = 0,
  BRIDGE_MODE_PASSTHROUGH = 1
} BridgeMode;

void Bridge_Init(uint32_t uart2_baud);
void Bridge_Process(void);
void Bridge_SetMode(BridgeMode mode);
BridgeMode Bridge_GetMode(void);
uint8_t Bridge_ReadDebug(uint8_t *byte);
uint8_t Bridge_ReadUart2(uint8_t *byte);
uint8_t Bridge_SendDebug(const uint8_t *data, uint16_t length);
uint8_t Bridge_TrySendDebug(const uint8_t *data, uint16_t length);
uint8_t Bridge_SendUart2(const uint8_t *data, uint16_t length);
uint8_t Bridge_Uart2SelfTest(uint16_t *received);
uint8_t Bridge_SendDebugBlocking(const uint8_t *data, uint16_t length);
uint8_t Bridge_SetUart2Baud(uint32_t baud);
uint32_t Bridge_GetUart2Baud(void);
uint32_t Bridge_GetDebugRxDrops(void);
uint32_t Bridge_GetUart2RxDrops(void);
uint32_t Bridge_GetDebugTxDrops(void);
uint32_t Bridge_GetUart2TxDrops(void);
uint32_t Bridge_GetDebugErrors(void);
uint32_t Bridge_GetUart2Errors(void);

#endif
