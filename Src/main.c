/* USER CODE BEGIN Header */
/**
  * @file    main.c
  * @brief   UART bridge and board control firmware.
  */
/* USER CODE END Header */

#include "main.h"
#include "app_config.h"
#include "cli.h"
#include "control.h"
#include "flash_cache.h"
#include "uart_bridge.h"

#include <string.h>

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart2_rx;
DMA_HandleTypeDef hdma_usart2_tx;
volatile uint32_t firmware_status_magic;
volatile uint32_t firmware_system_clock;
volatile uint32_t firmware_flash_jedec;
volatile uint32_t firmware_ready;

static void SystemClock_Config(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(uint32_t baud);
static void MX_SPI1_Init(void);
static void ProcessDebugInput(void);
static void ProcessUart2Input(void);

#define CLI_ESCAPE_BYTE 0x1DU
#define CLI_ESCAPE_INTERVAL_MS 500U

static uint8_t cli_escape_pending;
static uint32_t cli_escape_tick;
static uint64_t cli_escape_timestamp;
static uint8_t skip_passthrough_lf;
static uint8_t debug_forward_buffer[128];
static uint16_t debug_forward_length;
static uint64_t debug_forward_timestamp;
static uint32_t uptime_last_tick;
static uint32_t uptime_wraps;
static uint8_t using_external_clock;

uint64_t App_GetUptimeMilliseconds(void)
{
  uint32_t tick = HAL_GetTick();

  if (tick < uptime_last_tick)
  {
    uptime_wraps++;
  }
  uptime_last_tick = tick;
  return ((uint64_t)uptime_wraps << 32U) | tick;
}

uint8_t App_UsingExternalClock(void)
{
  return using_external_clock;
}

static void FlushDebugForward(void)
{
  if (debug_forward_length == 0U)
  {
    return;
  }
  if (Bridge_SendUart2(debug_forward_buffer, debug_forward_length) != 0U)
  {
    FlashCache_Push(FLASH_CACHE_DIRECTION_TX, debug_forward_buffer,
                    debug_forward_length, debug_forward_timestamp);
  }
  debug_forward_length = 0U;
}

static void QueueDebugForward(const uint8_t *data, uint16_t length,
                              uint64_t timestamp)
{
  while (length != 0U)
  {
    uint16_t available;
    uint16_t chunk;
    if (debug_forward_length == 0U)
    {
      debug_forward_timestamp = timestamp;
    }
    available = (uint16_t)(sizeof(debug_forward_buffer) -
                           debug_forward_length);
    chunk = length < available ? length : available;
    memcpy(&debug_forward_buffer[debug_forward_length], data, chunk);
    debug_forward_length = (uint16_t)(debug_forward_length + chunk);
    data += chunk;
    length = (uint16_t)(length - chunk);
    if (debug_forward_length == sizeof(debug_forward_buffer))
    {
      FlushDebugForward();
    }
  }
}

static void ForwardDebugByte(uint8_t byte, uint64_t timestamp)
{
  uint32_t now = HAL_GetTick();

  if (cli_escape_pending != 0U)
  {
    if (byte == CLI_ESCAPE_BYTE &&
        (now - cli_escape_tick) <= CLI_ESCAPE_INTERVAL_MS)
    {
      cli_escape_pending = 0U;
      FlushDebugForward();
      Bridge_SetMode(BRIDGE_MODE_CLI);
      Cli_OnEnter();
      return;
    }
    {
      const uint8_t escape = CLI_ESCAPE_BYTE;
      QueueDebugForward(&escape, 1U, cli_escape_timestamp);
    }
    cli_escape_pending = 0U;
  }

  if (byte == CLI_ESCAPE_BYTE)
  {
    cli_escape_pending = 1U;
    cli_escape_tick = now;
    cli_escape_timestamp = timestamp;
    return;
  }
  QueueDebugForward(&byte, 1U, timestamp);
}

static void ProcessCliEscapeTimeout(void)
{
  if (cli_escape_pending != 0U &&
      (HAL_GetTick() - cli_escape_tick) > CLI_ESCAPE_INTERVAL_MS)
  {
    const uint8_t byte = CLI_ESCAPE_BYTE;
    QueueDebugForward(&byte, 1U, cli_escape_timestamp);
    cli_escape_pending = 0U;
  }
}

static void ProcessDebugInput(void)
{
  uint8_t byte;

  while (Bridge_ReadDebug(&byte) != 0U)
  {
    if (Bridge_GetMode() == BRIDGE_MODE_CLI)
    {
      {
        CliAction action = Cli_ProcessByte(byte);
        if (action != CLI_ACTION_NONE)
        {
          Bridge_SetMode(BRIDGE_MODE_PASSTHROUGH);
          skip_passthrough_lf =
              action == CLI_ACTION_EXIT_SKIP_LF ? 1U : 0U;
        }
      }
    }
    else
    {
      if (skip_passthrough_lf != 0U && byte == '\n')
      {
        skip_passthrough_lf = 0U;
        continue;
      }
      skip_passthrough_lf = 0U;
      ForwardDebugByte(byte, App_GetUptimeMilliseconds());
    }
  }
  if (Bridge_GetMode() == BRIDGE_MODE_PASSTHROUGH)
  {
    ProcessCliEscapeTimeout();
  }
  FlushDebugForward();
}

static void ProcessUart2Input(void)
{
  uint8_t byte;
  uint8_t forward_buffer[128];
  uint16_t forward_length = 0U;
  uint64_t timestamp = 0U;

  while (Bridge_ReadUart2(&byte) != 0U)
  {
    if (forward_length == 0U)
    {
      timestamp = App_GetUptimeMilliseconds();
    }
    forward_buffer[forward_length++] = byte;
    if (forward_length == sizeof(forward_buffer))
    {
      if (FlashCache_DumpIsActive() == 0U)
      {
        (void)Bridge_SendDebug(forward_buffer, forward_length);
      }
      FlashCache_Push(FLASH_CACHE_DIRECTION_RX, forward_buffer,
                      forward_length, timestamp);
      forward_length = 0U;
    }
  }

  if (forward_length != 0U)
  {
    if (FlashCache_DumpIsActive() == 0U)
    {
      (void)Bridge_SendDebug(forward_buffer, forward_length);
    }
    FlashCache_Push(FLASH_CACHE_DIRECTION_RX, forward_buffer,
                    forward_length, timestamp);
  }
}

int main(void)
{
  FlashCacheStatus flash_status;
  uint32_t uart2_baud;

  HAL_Init();
  SystemClock_Config();
  uptime_last_tick = HAL_GetTick();
  uptime_wraps = 0U;
  Control_Init();
  uart2_baud = AppConfig_LoadBaud();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init(uart2_baud);
  MX_SPI1_Init();
  Bridge_Init(uart2_baud);
  FlashCache_Init();
  Cli_Init();

  FlashCache_GetStatus(&flash_status);
  firmware_system_clock = HAL_RCC_GetHCLKFreq();
  firmware_flash_jedec = ((uint32_t)flash_status.jedec_id[0] << 16U) |
                         ((uint32_t)flash_status.jedec_id[1] << 8U) |
                         flash_status.jedec_id[2];
  firmware_status_magic = 0x46575244UL;
  firmware_ready = 1U;

  while (1)
  {
    (void)App_GetUptimeMilliseconds();
    ProcessUart2Input();
    ProcessDebugInput();
    Control_Task();
    FlashCache_Task();
    FlashCache_DumpTask();
    Cli_Task();
    Bridge_Process();
  }
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef oscillator = {0};
  RCC_ClkInitTypeDef clocks = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  using_external_clock = 0U;
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  oscillator.HSEState = RCC_HSE_ON;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  oscillator.PLL.PLLM = RCC_PLLM_DIV1;
  oscillator.PLL.PLLN = 16U;
  oscillator.PLL.PLLP = RCC_PLLP_DIV2;
  oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
  oscillator.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&oscillator) == HAL_OK)
  {
    using_external_clock = 1U;
  }
  else
  {
    if (HAL_RCC_DeInit() != HAL_OK)
    {
      Error_Handler();
    }
    memset(&oscillator, 0, sizeof(oscillator));
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSIDiv = RCC_HSI_DIV1;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLM = RCC_PLLM_DIV1;
    oscillator.PLL.PLLN = 8U;
    oscillator.PLL.PLLP = RCC_PLLP_DIV2;
    oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
    oscillator.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
    {
      Error_Handler();
    }
  }

  clocks.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                     RCC_CLOCKTYPE_PCLK1;
  clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clocks.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 1500000U;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_USART2_UART_Init(uint32_t baud)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = baud;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7U;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;

  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  firmware_status_magic = 0x45525221UL;
  firmware_ready = 0U;
  __disable_irq();
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
}
#endif
