#include "uart_bridge.h"
#include "app_config.h"
#include "main.h"

#include <string.h>

#define BRIDGE_RX_DMA_SIZE 1024U
#define BRIDGE_QUEUE_SIZE 16384U

typedef struct
{
  UART_HandleTypeDef *huart;
  uint8_t *rx_dma;
  uint16_t rx_last;
  uint8_t *rx_queue;
  volatile uint16_t rx_head;
  volatile uint16_t rx_tail;
  uint8_t *tx_queue;
  volatile uint16_t tx_head;
  volatile uint16_t tx_tail;
  volatile uint16_t tx_inflight;
  volatile uint8_t tx_active;
  volatile uint32_t rx_drops;
  volatile uint32_t tx_drops;
  volatile uint32_t errors;
} BridgeStream;

static uint8_t debug_rx_dma[BRIDGE_RX_DMA_SIZE];
static uint8_t uart2_rx_dma[BRIDGE_RX_DMA_SIZE];
static uint8_t debug_rx_queue[BRIDGE_QUEUE_SIZE];
static uint8_t uart2_rx_queue[BRIDGE_QUEUE_SIZE];
static uint8_t debug_tx_queue[BRIDGE_QUEUE_SIZE];
static uint8_t uart2_tx_queue[BRIDGE_QUEUE_SIZE];

static BridgeStream debug_stream = {
  &huart1, debug_rx_dma, 0U, debug_rx_queue, 0U, 0U,
  debug_tx_queue, 0U, 0U, 0U, 0U, 0U, 0U, 0U
};
static BridgeStream uart2_stream = {
  &huart2, uart2_rx_dma, 0U, uart2_rx_queue, 0U, 0U,
  uart2_tx_queue, 0U, 0U, 0U, 0U, 0U, 0U, 0U
};

static BridgeMode bridge_mode = BRIDGE_MODE_PASSTHROUGH;

static uint16_t Bridge_QueueFree(uint16_t head, uint16_t tail)
{
  if (head >= tail)
  {
    return (uint16_t)(BRIDGE_QUEUE_SIZE - (head - tail) - 1U);
  }
  return (uint16_t)(tail - head - 1U);
}

static void Bridge_RxPush(BridgeStream *stream, const uint8_t *data,
                          uint16_t length)
{
  for (uint16_t i = 0U; i < length; ++i)
  {
    uint16_t next = (uint16_t)((stream->rx_head + 1U) % BRIDGE_QUEUE_SIZE);
    if (next == stream->rx_tail)
    {
      stream->rx_drops++;
      continue;
    }
    stream->rx_queue[stream->rx_head] = data[i];
    stream->rx_head = next;
  }
}

static void Bridge_RxEvent(BridgeStream *stream, uint16_t size)
{
  uint16_t current = size;

  if (current >= BRIDGE_RX_DMA_SIZE)
  {
    current = 0U;
  }

  if (current >= stream->rx_last)
  {
    Bridge_RxPush(stream, &stream->rx_dma[stream->rx_last],
                  (uint16_t)(current - stream->rx_last));
  }
  else
  {
    Bridge_RxPush(stream, &stream->rx_dma[stream->rx_last],
                  (uint16_t)(BRIDGE_RX_DMA_SIZE - stream->rx_last));
    Bridge_RxPush(stream, stream->rx_dma, current);
  }

  stream->rx_last = current;
}

static void Bridge_TxStart(BridgeStream *stream)
{
  uint16_t length;

  if (stream->tx_active != 0U || stream->tx_tail == stream->tx_head)
  {
    return;
  }

  if (stream->tx_head > stream->tx_tail)
  {
    length = (uint16_t)(stream->tx_head - stream->tx_tail);
  }
  else
  {
    length = (uint16_t)(BRIDGE_QUEUE_SIZE - stream->tx_tail);
  }

  stream->tx_inflight = length;
  stream->tx_active = 1U;
  if (HAL_UART_Transmit_DMA(stream->huart,
                            &stream->tx_queue[stream->tx_tail], length) != HAL_OK)
  {
    stream->tx_active = 0U;
    stream->tx_inflight = 0U;
  }
}

static uint8_t Bridge_TxPush(BridgeStream *stream, const uint8_t *data,
                             uint16_t length, uint8_t count_drop)
{
  uint8_t result = 1U;

  __disable_irq();
  if (Bridge_QueueFree(stream->tx_head, stream->tx_tail) < length)
  {
    if (count_drop != 0U)
    {
      stream->tx_drops += length;
    }
    result = 0U;
  }
  else
  {
    for (uint16_t i = 0U; i < length; ++i)
    {
      stream->tx_queue[stream->tx_head] = data[i];
      stream->tx_head = (uint16_t)((stream->tx_head + 1U) % BRIDGE_QUEUE_SIZE);
    }
    Bridge_TxStart(stream);
  }
  __enable_irq();

  return result;
}

static uint16_t Bridge_TxPending(const BridgeStream *stream)
{
  uint16_t pending;

  __disable_irq();
  if (stream->tx_head >= stream->tx_tail)
  {
    pending = (uint16_t)(stream->tx_head - stream->tx_tail);
  }
  else
  {
    pending = (uint16_t)(BRIDGE_QUEUE_SIZE - stream->tx_tail +
                         stream->tx_head);
  }
  __enable_irq();

  return pending;
}

static uint8_t Bridge_RxPop(BridgeStream *stream, uint8_t *byte)
{
  uint8_t available = 0U;

  __disable_irq();
  if (stream->rx_tail != stream->rx_head)
  {
    *byte = stream->rx_queue[stream->rx_tail];
    stream->rx_tail = (uint16_t)((stream->rx_tail + 1U) % BRIDGE_QUEUE_SIZE);
    available = 1U;
  }
  __enable_irq();

  return available;
}

static void Bridge_StartRx(BridgeStream *stream)
{
  stream->rx_last = 0U;
  if (HAL_UARTEx_ReceiveToIdle_DMA(stream->huart, stream->rx_dma,
                                   BRIDGE_RX_DMA_SIZE) != HAL_OK)
  {
    stream->errors++;
  }
}

void Bridge_Init(uint32_t uart2_baud)
{
  (void)uart2_baud;
  bridge_mode = BRIDGE_MODE_PASSTHROUGH;
  Bridge_StartRx(&debug_stream);
  Bridge_StartRx(&uart2_stream);
}

void Bridge_Process(void)
{
  Bridge_TxStart(&debug_stream);
  Bridge_TxStart(&uart2_stream);
}

void Bridge_SetMode(BridgeMode mode)
{
  bridge_mode = mode;
}

BridgeMode Bridge_GetMode(void)
{
  return bridge_mode;
}

uint8_t Bridge_ReadDebug(uint8_t *byte)
{
  return Bridge_RxPop(&debug_stream, byte);
}

uint8_t Bridge_ReadUart2(uint8_t *byte)
{
  return Bridge_RxPop(&uart2_stream, byte);
}

uint8_t Bridge_SendDebug(const uint8_t *data, uint16_t length)
{
  return Bridge_TxPush(&debug_stream, data, length, 1U);
}

uint8_t Bridge_TrySendDebug(const uint8_t *data, uint16_t length)
{
  return Bridge_TxPush(&debug_stream, data, length, 0U);
}

uint8_t Bridge_SendUart2(const uint8_t *data, uint16_t length)
{
  return Bridge_TxPush(&uart2_stream, data, length, 1U);
}

uint8_t Bridge_Uart2SelfTest(uint16_t *received)
{
  static const uint8_t pattern[] = {
    0x55U, 0xAAU, 0x00U, 0xFFU, 0x11U, 0xEEU, 0x22U, 0xDDU,
    0x33U, 0xCCU, 0x44U, 0xBBU, 0x66U, 0x99U, 0x77U, 0x88U,
    0x5AU, 0xA5U, 0x3CU, 0xC3U, 0x69U, 0x96U, 0x18U, 0x81U,
    0x24U, 0x42U, 0x7EU, 0xE7U, 0x0FU, 0xF0U, 0x12U, 0x21U
  };
  uint32_t start;
  uint16_t count = 0U;
  uint8_t match = 1U;
  uint8_t byte;

  if (received == NULL)
  {
    return 0U;
  }

  __disable_irq();
  uart2_stream.rx_tail = uart2_stream.rx_head;
  __enable_irq();

  if (HAL_UART_Transmit(&huart2, (uint8_t *)pattern, sizeof(pattern),
                        200U) != HAL_OK)
  {
    *received = 0U;
    return 0U;
  }

  start = HAL_GetTick();
  while ((HAL_GetTick() - start) < 100U)
  {
    while (Bridge_RxPop(&uart2_stream, &byte) != 0U)
    {
      if (count >= sizeof(pattern) || byte != pattern[count])
      {
        match = 0U;
      }
      if (count < 0xFFFFU)
      {
        count++;
      }
    }

    if (count >= sizeof(pattern))
    {
      break;
    }
  }

  *received = count;
  return (match != 0U && count == sizeof(pattern)) ? 1U : 0U;
}

uint8_t Bridge_SendDebugBlocking(const uint8_t *data, uint16_t length)
{
  uint32_t start = HAL_GetTick();
  uint16_t offset = 0U;

  while (offset < length)
  {
    uint16_t chunk = (uint16_t)(length - offset);
    if (chunk > 256U)
    {
      chunk = 256U;
    }

    while (Bridge_SendDebug(&data[offset], chunk) == 0U)
    {
      Bridge_Process();
      if ((HAL_GetTick() - start) > 5000U)
      {
        return 0U;
      }
    }
    offset = (uint16_t)(offset + chunk);
    while (Bridge_TxPending(&debug_stream) != 0U)
    {
      Bridge_Process();
      if ((HAL_GetTick() - start) > 5000U)
      {
        return 0U;
      }
    }
    HAL_Delay(1U);
  }

  return 1U;
}

uint8_t Bridge_SetUart2Baud(uint32_t baud)
{
  if (baud < UART2_MIN_BAUD || baud > UART2_MAX_BAUD)
  {
    return 0U;
  }

  HAL_UART_Abort(&huart2);
  if (HAL_UART_DeInit(&huart2) != HAL_OK)
  {
    return 0U;
  }

  huart2.Init.BaudRate = baud;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    return 0U;
  }

  uart2_stream.rx_head = 0U;
  uart2_stream.rx_tail = 0U;
  uart2_stream.tx_head = 0U;
  uart2_stream.tx_tail = 0U;
  uart2_stream.tx_active = 0U;
  uart2_stream.tx_inflight = 0U;
  Bridge_StartRx(&uart2_stream);
  return 1U;
}

uint32_t Bridge_GetUart2Baud(void)
{
  return huart2.Init.BaudRate;
}

uint32_t Bridge_GetDebugRxDrops(void)
{
  return debug_stream.rx_drops;
}

uint32_t Bridge_GetUart2RxDrops(void)
{
  return uart2_stream.rx_drops;
}

uint32_t Bridge_GetDebugTxDrops(void)
{
  return debug_stream.tx_drops;
}

uint32_t Bridge_GetUart2TxDrops(void)
{
  return uart2_stream.tx_drops;
}

uint32_t Bridge_GetDebugErrors(void)
{
  return debug_stream.errors;
}

uint32_t Bridge_GetUart2Errors(void)
{
  return uart2_stream.errors;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
  if (huart == &huart1)
  {
    Bridge_RxEvent(&debug_stream, size);
  }
  else if (huart == &huart2)
  {
    Bridge_RxEvent(&uart2_stream, size);
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  BridgeStream *stream = NULL;

  if (huart == &huart1)
  {
    stream = &debug_stream;
  }
  else if (huart == &huart2)
  {
    stream = &uart2_stream;
  }

  if (stream != NULL)
  {
    stream->tx_tail = (uint16_t)((stream->tx_tail + stream->tx_inflight) %
                                 BRIDGE_QUEUE_SIZE);
    stream->tx_inflight = 0U;
    stream->tx_active = 0U;
    Bridge_TxStart(stream);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == &huart1)
  {
    debug_stream.errors++;
    HAL_UART_AbortReceive(huart);
    Bridge_StartRx(&debug_stream);
  }
  else if (huart == &huart2)
  {
    uart2_stream.errors++;
    HAL_UART_AbortReceive(huart);
    Bridge_StartRx(&uart2_stream);
  }
}
