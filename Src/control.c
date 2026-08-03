#include "control.h"
#include "flash_cache.h"
#include "main.h"

#define POWER_BUTTON_DEBOUNCE_MS 80U
#define CONTROL_EVENT_QUEUE_SIZE 8U

typedef struct
{
  FlashCacheControlEvent event;
  ControlSource source;
  uint8_t value;
  uint32_t argument;
  uint32_t tick;
} ControlEvent;

static volatile uint8_t relay_state;
static uint8_t soc_power_state;
static uint8_t reset_asserted;
static uint8_t loader_state;
static uint8_t maskrom_state;
static volatile uint32_t power_button_last_tick;
static volatile ControlEvent control_events[CONTROL_EVENT_QUEUE_SIZE];
static volatile uint8_t control_event_head;
static volatile uint8_t control_event_tail;
static uint8_t reset_pulse_active;
static uint32_t reset_pulse_started;
static uint32_t reset_pulse_duration;
static ControlSource reset_pulse_source;

static void Control_WriteReset(uint8_t asserted)
{
  reset_asserted = asserted != 0U ? 1U : 0U;
  HAL_GPIO_WritePin(SOC_RESET_GPIO_Port, SOC_RESET_Pin,
                    reset_asserted != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void Control_QueueEvent(FlashCacheControlEvent event,
                               ControlSource source, uint8_t value,
                               uint32_t argument)
{
  uint32_t primask = __get_PRIMASK();
  uint8_t next;

  __disable_irq();
  next = (uint8_t)((control_event_head + 1U) % CONTROL_EVENT_QUEUE_SIZE);
  if (next != control_event_tail)
  {
    control_events[control_event_head].event = event;
    control_events[control_event_head].source = source;
    control_events[control_event_head].value = value;
    control_events[control_event_head].argument = argument;
    control_events[control_event_head].tick = HAL_GetTick();
    control_event_head = next;
  }
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static uint8_t Control_PopEvent(ControlEvent *event)
{
  uint32_t primask = __get_PRIMASK();
  uint8_t available = 0U;

  __disable_irq();
  if (control_event_tail != control_event_head)
  {
    *event = control_events[control_event_tail];
    control_event_tail =
        (uint8_t)((control_event_tail + 1U) % CONTROL_EVENT_QUEUE_SIZE);
    available = 1U;
  }
  if (primask == 0U)
  {
    __enable_irq();
  }
  return available;
}

static uint64_t Control_ExpandTick(uint32_t tick)
{
  uint64_t now = App_GetUptimeMilliseconds();
  uint64_t timestamp = (now & 0xFFFFFFFF00000000ULL) | tick;

  if (tick > (uint32_t)now && timestamp >= 0x100000000ULL)
  {
    timestamp -= 0x100000000ULL;
  }
  return timestamp;
}

void Control_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  const uint16_t pins = RELAY_Pin | SOC_POWER_Pin | SOC_RESET_Pin |
                        SOC_LOADER_Pin | SOC_MASKROM_Pin;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  relay_state = 0U;
  soc_power_state = 0U;
  reset_asserted = 0U;
  loader_state = 0U;
  maskrom_state = 0U;
  power_button_last_tick = HAL_GetTick() - POWER_BUTTON_DEBOUNCE_MS;
  control_event_head = 0U;
  control_event_tail = 0U;
  reset_pulse_active = 0U;
  reset_pulse_started = 0U;
  reset_pulse_duration = 0U;
  reset_pulse_source = CONTROL_SOURCE_CLI;

  HAL_GPIO_WritePin(GPIOB, pins, GPIO_PIN_RESET);

  gpio.Pin = pins;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gpio);

  gpio.Pin = POWER_BUTTON_Pin;
  gpio.Mode = GPIO_MODE_IT_FALLING;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(POWER_BUTTON_GPIO_Port, &gpio);

  __HAL_GPIO_EXTI_CLEAR_IT(POWER_BUTTON_Pin);
  HAL_NVIC_SetPriority(EXTI0_1_IRQn, 2U, 0U);
  HAL_NVIC_EnableIRQ(EXTI0_1_IRQn);
}

void Control_Task(void)
{
  ControlEvent event;

  if (reset_pulse_active != 0U &&
      (HAL_GetTick() - reset_pulse_started) >= reset_pulse_duration)
  {
    reset_pulse_active = 0U;
    Control_WriteReset(0U);
    Control_QueueEvent(FLASH_CACHE_CONTROL_RESET_PULSE,
                       reset_pulse_source, 0U, reset_pulse_duration);
  }

  while (Control_PopEvent(&event) != 0U)
  {
    FlashCache_PushControl(
        event.event,
        event.source == CONTROL_SOURCE_SW3 ? FLASH_CACHE_SOURCE_SW3
                                           : FLASH_CACHE_SOURCE_CLI,
        event.value, event.argument, Control_ExpandTick(event.tick));
  }
}

void Control_SetRelay(uint8_t on, ControlSource source)
{
  relay_state = on != 0U ? 1U : 0U;
  HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin,
                    relay_state != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
  Control_QueueEvent(FLASH_CACHE_CONTROL_RELAY, source, relay_state, 0U);
}

void Control_SetSocPower(uint8_t on, ControlSource source)
{
  soc_power_state = on != 0U ? 1U : 0U;
  HAL_GPIO_WritePin(SOC_POWER_GPIO_Port, SOC_POWER_Pin,
                    soc_power_state != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
  Control_QueueEvent(FLASH_CACHE_CONTROL_SOC_POWER, source, soc_power_state,
                     0U);
}

void Control_SetReset(uint8_t asserted, ControlSource source)
{
  reset_pulse_active = 0U;
  Control_WriteReset(asserted);
  Control_QueueEvent(reset_asserted != 0U
                         ? FLASH_CACHE_CONTROL_RESET_ASSERT
                         : FLASH_CACHE_CONTROL_RESET_RELEASE,
                     source, reset_asserted, 0U);
}

uint8_t Control_StartResetPulse(uint32_t duration_ms, ControlSource source)
{
  if (duration_ms < 1U || duration_ms > 5000U || reset_pulse_active != 0U)
  {
    return 0U;
  }
  reset_pulse_active = 1U;
  reset_pulse_started = HAL_GetTick();
  reset_pulse_duration = duration_ms;
  reset_pulse_source = source;
  Control_WriteReset(1U);
  return 1U;
}

void Control_SetLoader(uint8_t on, ControlSource source)
{
  loader_state = on != 0U ? 1U : 0U;
  HAL_GPIO_WritePin(SOC_LOADER_GPIO_Port, SOC_LOADER_Pin,
                    loader_state != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
  Control_QueueEvent(FLASH_CACHE_CONTROL_LOADER, source, loader_state, 0U);
}

void Control_SetMaskrom(uint8_t on, ControlSource source)
{
  maskrom_state = on != 0U ? 1U : 0U;
  HAL_GPIO_WritePin(SOC_MASKROM_GPIO_Port, SOC_MASKROM_Pin,
                    maskrom_state != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
  Control_QueueEvent(FLASH_CACHE_CONTROL_MASKROM, source, maskrom_state, 0U);
}

uint8_t Control_GetRelay(void)
{
  return relay_state;
}

uint8_t Control_GetSocPower(void)
{
  return soc_power_state;
}

uint8_t Control_GetResetAsserted(void)
{
  return reset_asserted;
}

uint8_t Control_GetLoader(void)
{
  return loader_state;
}

uint8_t Control_GetMaskrom(void)
{
  return maskrom_state;
}

uint8_t Control_GetPowerButtonPressed(void)
{
  return HAL_GPIO_ReadPin(POWER_BUTTON_GPIO_Port, POWER_BUTTON_Pin) ==
                 GPIO_PIN_RESET
             ? 1U
             : 0U;
}

uint8_t Control_GetResetPulseActive(void)
{
  return reset_pulse_active;
}

uint32_t Control_GetResetPulseRemaining(void)
{
  uint32_t elapsed;

  if (reset_pulse_active == 0U)
  {
    return 0U;
  }
  elapsed = HAL_GetTick() - reset_pulse_started;
  return elapsed >= reset_pulse_duration ? 0U : reset_pulse_duration - elapsed;
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t gpio_pin)
{
  uint32_t now;

  if (gpio_pin != POWER_BUTTON_Pin ||
      Control_GetPowerButtonPressed() == 0U)
  {
    return;
  }

  now = HAL_GetTick();
  if ((now - power_button_last_tick) < POWER_BUTTON_DEBOUNCE_MS)
  {
    return;
  }

  power_button_last_tick = now;
  Control_SetRelay(relay_state == 0U ? 1U : 0U, CONTROL_SOURCE_SW3);
}
