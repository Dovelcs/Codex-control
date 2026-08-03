#ifndef CONTROL_H
#define CONTROL_H

#include <stdint.h>

typedef enum
{
  CONTROL_SOURCE_CLI = 0,
  CONTROL_SOURCE_SW3 = 1
} ControlSource;

void Control_Init(void);
void Control_Task(void);
void Control_SetRelay(uint8_t on, ControlSource source);
void Control_SetSocPower(uint8_t on, ControlSource source);
void Control_SetReset(uint8_t asserted, ControlSource source);
uint8_t Control_StartResetPulse(uint32_t duration_ms, ControlSource source);
void Control_SetLoader(uint8_t on, ControlSource source);
void Control_SetMaskrom(uint8_t on, ControlSource source);
uint8_t Control_GetRelay(void);
uint8_t Control_GetSocPower(void);
uint8_t Control_GetResetAsserted(void);
uint8_t Control_GetLoader(void);
uint8_t Control_GetMaskrom(void);
uint8_t Control_GetPowerButtonPressed(void);
uint8_t Control_GetResetPulseActive(void);
uint32_t Control_GetResetPulseRemaining(void);

#endif
