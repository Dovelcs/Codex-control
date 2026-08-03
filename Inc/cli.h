#ifndef CLI_H
#define CLI_H

#include <stdint.h>

typedef enum
{
  CLI_ACTION_NONE = 0,
  CLI_ACTION_EXIT,
  CLI_ACTION_EXIT_SKIP_LF
} CliAction;

void Cli_Init(void);
void Cli_Task(void);
CliAction Cli_ProcessByte(uint8_t byte);
void Cli_PrintPrompt(void);
void Cli_OnEnter(void);

#endif
