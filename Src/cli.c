#include "cli.h"
#include "app_config.h"
#include "control.h"
#include "flash_cache.h"
#include "main.h"
#include "uart_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_LINE_SIZE 128U
#define CLI_HISTORY_DEPTH 8U
#define CLI_EXEC_NORMAL 0U
#define CLI_EXEC_EXIT 1U
#define CLI_EXEC_ASYNC 2U

typedef struct
{
  const char *section;
  const char *syntax;
  const char *completion;
  const char *description;
} CliCommand;

static const CliCommand cli_commands[] = {
    {"系统", "help", "help", "显示本帮助页面"},
    {NULL, "status", "status", "显示全部外设当前状态"},
    {NULL, "system status", "system status", "status 的兼容写法"},
    {NULL, "mode passthrough", "mode passthrough", "退出命令行并返回串口透传"},
    {NULL, "exit", "exit", "退出命令行并返回串口透传"},
    {"时间", "time get", "time get", "查看当前真实时间同步状态"},
    {NULL, "time set iso <ISO8601>", "time set iso ", "使用带时区ISO8601时间校时"},
    {NULL, "time set unix <ms> <zone>", "time set unix ", "使用Unix毫秒和时区校时"},
    {NULL, "time clear", "time clear", "清除本次会话真实时间映射"},
    {"UART2", "uart2 baud get", "uart2 baud get", "查看UART2当前波特率"},
    {NULL, "uart2 baud set <baud>", "uart2 baud set ", "修改UART2波特率，范围1200~2000000"},
    {NULL, "uart2 baud save", "uart2 baud save", "保存当前波特率，复位后继续使用"},
    {NULL, "uart2 selftest", "uart2 selftest", "执行32字节UART2硬件回环测试"},
    {"外部 Flash 与缓存", "flash test", "flash test", "执行擦除、写入和读回自检"},
    {NULL, "cache status", "cache status", "显示串口缓存状态"},
    {NULL, "cache dump [length]", "cache dump ", "按默认格式导出全部或指定长度的UART数据"},
    {NULL, "cache dump text [length]", "cache dump text ", "导出带时间和方向的可读文本"},
    {NULL, "cache dump raw [length]", "cache dump raw ", "导出不附加任何字符的原始二进制数据"},
    {NULL, "cache timestamp get", "cache timestamp get", "查看cache dump默认输出格式"},
    {NULL, "cache timestamp on", "cache timestamp on", "兼容命令：默认使用带时间可读文本"},
    {NULL, "cache timestamp off", "cache timestamp off", "兼容命令：默认使用原始二进制输出"},
    {NULL, "cache flush", "cache flush", "立即请求提交RAM中的待写缓存"},
    {NULL, "cache clear", "cache clear", "清空全部串口缓存"},
    {"继电器与电源", "relay on", "relay on", "吸合继电器并点亮LED3"},
    {NULL, "relay off", "relay off", "释放继电器并熄灭LED3"},
    {NULL, "soc power on", "soc power on", "断言SoC电源控制信号"},
    {NULL, "soc power off", "soc power off", "释放SoC电源控制信号"},
    {"SoC启动控制", "reset assert", "reset assert", "断言SoC复位"},
    {NULL, "reset release", "reset release", "释放SoC复位"},
    {NULL, "reset pulse <ms>", "reset pulse ", "输出1~5000毫秒复位脉冲"},
    {NULL, "loader on", "loader on", "断言Loader信号"},
    {NULL, "loader off", "loader off", "释放Loader信号"},
    {NULL, "maskrom on", "maskrom on", "断言MaskROM信号"},
    {NULL, "maskrom off", "maskrom off", "释放MaskROM信号"},
    {NULL, "pins status", "pins status", "显示控制输出和SW3按键状态"},
};

#define CLI_COMMAND_COUNT \
  ((uint16_t)(sizeof(cli_commands) / sizeof(cli_commands[0])))

static char cli_line[CLI_LINE_SIZE];
static uint16_t cli_length;
static uint8_t cli_skip_lf;
static uint8_t cli_escape_state;
static char cli_history[CLI_HISTORY_DEPTH][CLI_LINE_SIZE];
static char cli_history_draft[CLI_LINE_SIZE];
static uint8_t cli_history_count;
static int16_t cli_history_position;
static FlashCacheDumpFormat cli_cache_dump_format;

static void Cli_Write(const char *text)
{
  (void)Bridge_SendDebugBlocking((const uint8_t *)text, (uint16_t)strlen(text));
}

static const char *Cli_DumpFormatName(FlashCacheDumpFormat format)
{
  switch (format)
  {
    case FLASH_CACHE_DUMP_FORMAT_RAW: return "Raw二进制";
    case FLASH_CACHE_DUMP_FORMAT_TEXT: return "带时间可读文本";
    default: return "未知";
  }
}

static void Cli_WriteLine(const char *syntax, const char *description)
{
  char text[192];

  (void)snprintf(text, sizeof(text), "  %-28s %s\r\n", syntax, description);
  Cli_Write(text);
}

static void Cli_PrintCommandList(void)
{
  uint16_t index;

  Cli_Write("\r\n========== 命令帮助 ==========\r\n");
  for (index = 0U; index < CLI_COMMAND_COUNT; index++)
  {
    if (cli_commands[index].section != NULL)
    {
      Cli_Write("\r\n[");
      Cli_Write(cli_commands[index].section);
      Cli_Write("]\r\n");
    }
    Cli_WriteLine(cli_commands[index].syntax,
                  cli_commands[index].description);
  }
  Cli_Write("\r\n[实体按键]\r\n"
            "  SW3每按一次切换继电器电源，带80毫秒防抖。\r\n"
            "\r\n[命令行编辑]\r\n"
            "  Tab           显示命令、解释或补全当前命令\r\n"
            "  上/下方向键   浏览最近8条命令历史\r\n"
            "  Ctrl+D        退出命令行并返回串口透传\r\n"
            "  双击Ctrl+]    500毫秒内连续按两次进入命令行\r\n"
            "  Ctrl+C        取消正在进行的缓存导出\r\n"
            "\r\n提示：cache dump默认输出可读文本，raw模式才输出原始二进制。\r\n"
            "================================\r\n");
}

static void Cli_RedrawInput(void)
{
  Cli_PrintPrompt();
  if (cli_length != 0U)
  {
    (void)Bridge_SendDebugBlocking((const uint8_t *)cli_line, cli_length);
  }
}

static void Cli_ReplaceInput(const char *text)
{
  size_t length = strlen(text);

  if (length >= CLI_LINE_SIZE)
  {
    length = CLI_LINE_SIZE - 1U;
  }
  memcpy(cli_line, text, length);
  cli_line[length] = '\0';
  cli_length = (uint16_t)length;
  Cli_Write("\r\x1B[2K");
  Cli_RedrawInput();
}

static void Cli_HistoryAdd(void)
{
  const char *command = cli_line;

  while (*command == ' ' || *command == '\t')
  {
    command++;
  }
  if (*command == '\0')
  {
    return;
  }
  if (cli_history_count != 0U &&
      strcmp(cli_history[cli_history_count - 1U], cli_line) == 0)
  {
    return;
  }
  if (cli_history_count == CLI_HISTORY_DEPTH)
  {
    memmove(cli_history[0], cli_history[1],
            (CLI_HISTORY_DEPTH - 1U) * CLI_LINE_SIZE);
    cli_history_count--;
  }
  memcpy(cli_history[cli_history_count], cli_line, cli_length + 1U);
  cli_history_count++;
}

static void Cli_HistoryMove(int8_t direction)
{
  if (direction < 0)
  {
    if (cli_history_count == 0U)
    {
      Cli_Write("\a");
      return;
    }
    if (cli_history_position < 0)
    {
      memcpy(cli_history_draft, cli_line, cli_length + 1U);
      cli_history_position = (int16_t)cli_history_count - 1;
    }
    else if (cli_history_position > 0)
    {
      cli_history_position--;
    }
    else
    {
      Cli_Write("\a");
    }
    Cli_ReplaceInput(cli_history[cli_history_position]);
    return;
  }

  if (cli_history_position < 0)
  {
    Cli_Write("\a");
  }
  else if (cli_history_position < (int16_t)cli_history_count - 1)
  {
    cli_history_position++;
    Cli_ReplaceInput(cli_history[cli_history_position]);
  }
  else
  {
    cli_history_position = -1;
    Cli_ReplaceInput(cli_history_draft);
  }
}

static uint8_t Cli_IsCompletionMatch(uint16_t index)
{
  size_t completion_length = strlen(cli_commands[index].completion);

  return cli_length <= completion_length &&
         strncmp(cli_commands[index].completion, cli_line, cli_length) == 0;
}

static void Cli_PrintCompletionMatches(void)
{
  uint16_t index;

  Cli_Write("\r\n[补全候选]\r\n");
  for (index = 0U; index < CLI_COMMAND_COUNT; index++)
  {
    if (Cli_IsCompletionMatch(index) != 0U)
    {
      Cli_WriteLine(cli_commands[index].syntax,
                    cli_commands[index].description);
    }
  }
  Cli_RedrawInput();
}

static void Cli_HandleTab(void)
{
  uint16_t index;
  uint16_t first_match = 0U;
  uint16_t match_count = 0U;
  size_t common_length = 0U;
  size_t offset;

  if (cli_length == 0U)
  {
    Cli_PrintCommandList();
    Cli_RedrawInput();
    return;
  }

  for (index = 0U; index < CLI_COMMAND_COUNT; index++)
  {
    if (Cli_IsCompletionMatch(index) == 0U)
    {
      continue;
    }
    if (match_count == 0U)
    {
      first_match = index;
      common_length = strlen(cli_commands[index].completion);
    }
    else
    {
      const char *first = cli_commands[first_match].completion;
      const char *current = cli_commands[index].completion;
      for (offset = cli_length; offset < common_length; offset++)
      {
        if (first[offset] != current[offset])
        {
          common_length = offset;
          break;
        }
      }
    }
    match_count++;
  }

  if (match_count == 0U)
  {
    Cli_Write("\a");
    return;
  }

  if (match_count == 1U)
  {
    common_length = strlen(cli_commands[first_match].completion);
  }
  if (common_length > cli_length)
  {
    size_t append_length = common_length - cli_length;
    if (append_length > CLI_LINE_SIZE - 1U - cli_length)
    {
      append_length = CLI_LINE_SIZE - 1U - cli_length;
    }
    memcpy(&cli_line[cli_length],
           &cli_commands[first_match].completion[cli_length], append_length);
    cli_length += (uint16_t)append_length;
    cli_line[cli_length] = '\0';
    (void)Bridge_SendDebugBlocking(
        (const uint8_t *)&cli_line[cli_length - append_length],
        (uint16_t)append_length);
  }

  if (match_count > 1U || common_length == cli_length)
  {
    Cli_PrintCompletionMatches();
  }
}

static void Cli_PrintCacheStatus(void)
{
  FlashCacheStatus status;
  char text[448];
  const char *state;
  uint32_t compression_percent;

  FlashCache_GetStatus(&status);
  compression_percent = status.committed_original_bytes != 0U
                            ? (status.committed_compressed_bytes * 100U) /
                                  status.committed_original_bytes
                            : 0U;
  state = status.paused != 0U ? "错误暂停" :
          (status.busy != 0U ? "正在写入" : "空闲");
  Cli_Write("[外部 Flash]\r\n");
  (void)snprintf(text, sizeof(text),
                 "  连接状态      : %s\r\n"
                 "  JEDEC ID      : %02X %02X %02X\r\n"
                 "  当前会话      : S%08lu\r\n"
                 "  写入状态      : %s\r\n"
                 "  导出状态      : %s / %s\r\n"
                 "  写入策略      : 1024字节 或 60000毫秒\r\n"
                 "  压缩格式      : v3 / LZ4（Flash块统一压缩）\r\n"
                 "  备用扇区      : %s\r\n"
                 "  默认导出格式  : %s\r\n",
                 status.present != 0U ? "已连接" : "未检测到",
                 status.jedec_id[0], status.jedec_id[1], status.jedec_id[2],
                 (unsigned long)status.session_id, state,
                 status.dump_active != 0U ? "进行中" : "空闲",
                 Cli_DumpFormatName((FlashCacheDumpFormat)status.dump_format),
                 status.spare_ready != 0U ? "已擦除可用" : "后台准备中",
                 Cli_DumpFormatName(cli_cache_dump_format));
  Cli_Write(text);

  (void)snprintf(text, sizeof(text),
                 "  已提交数据    : %lu 字节 / %lu 条 / %lu 块\r\n"
                 "  压缩记录区    : %lu -> %lu 字节（%lu%%）\r\n"
                 "  当前扇区剩余  : %lu 字节\r\n"
                 "  RAM待写数据   : %lu 字节 / %lu 条\r\n"
                 "  最早等待时间  : %lu 毫秒\r\n"
                 "  RX累计数据    : %lu 字节\r\n"
                 "  TX累计数据    : %lu 字节\r\n",
                 (unsigned long)status.committed_bytes,
                 (unsigned long)status.committed_records,
                 (unsigned long)status.committed_blocks,
                 (unsigned long)status.committed_original_bytes,
                 (unsigned long)status.committed_compressed_bytes,
                 (unsigned long)compression_percent,
                 (unsigned long)status.active_free_bytes,
                 (unsigned long)status.pending_bytes,
                 (unsigned long)status.pending_records,
                 (unsigned long)status.pending_age_ms,
                 (unsigned long)status.rx_bytes,
                 (unsigned long)status.tx_bytes);
  Cli_Write(text);

  (void)snprintf(text, sizeof(text),
                 "  事件累计      : %lu 条\r\n"
                 "  导出快照      : %lu 字节\r\n"
                 "  导出已输出    : %lu 字节\r\n"
                 "  导出取消      : %lu 次\r\n"
                 "  快照溢出      : %lu 次\r\n"
                 "  RAM丢弃数据   : %lu 字节 / %lu 条\r\n"
                 "  环形覆盖块    : %lu\r\n"
                 "  本次启动写入  : %lu 块\r\n"
                 "  本次启动擦除  : %lu 扇区\r\n"
                 "  写入错误      : %lu\r\n",
                 (unsigned long)status.event_records,
                 (unsigned long)status.dump_snapshot_bytes,
                 (unsigned long)status.dump_output_bytes,
                 (unsigned long)status.dump_cancel_count,
                 (unsigned long)status.dump_overflow_count,
                 (unsigned long)status.dropped_bytes,
                 (unsigned long)status.dropped_records,
                 (unsigned long)status.overwritten_blocks,
                 (unsigned long)status.write_blocks,
                 (unsigned long)status.erase_sectors,
                 (unsigned long)status.write_errors);
  Cli_Write(text);
}

static void Cli_PrintPins(void)
{
  char text[384];

  Cli_Write("[控制输出]\r\n");
  (void)snprintf(text, sizeof(text),
                 "  继电器电源    : %s\r\n"
                 "  SoC 电源控制  : %s\r\n"
                 "  SoC Reset     : %s\r\n"
                 "  Reset脉冲     : %s / 剩余%lu毫秒\r\n"
                 "  Loader        : %s\r\n"
                 "  MaskROM       : %s\r\n"
                 "\r\n[按键]\r\n"
                 "  SW3 电源按键  : %s\r\n",
                 Control_GetRelay() != 0U ? "已开启" : "已关闭",
                 Control_GetSocPower() != 0U ? "已开启" : "已关闭",
                 Control_GetResetAsserted() != 0U ? "已断言" : "已释放",
                 Control_GetResetPulseActive() != 0U ? "进行中" : "空闲",
                 (unsigned long)Control_GetResetPulseRemaining(),
                 Control_GetLoader() != 0U ? "已启用" : "已关闭",
                 Control_GetMaskrom() != 0U ? "已启用" : "已关闭",
                 Control_GetPowerButtonPressed() != 0U ? "已按下" :
                                                         "已释放");
  Cli_Write(text);
}

static void Cli_FormatUint64(uint64_t value, char *text)
{
  char reversed[20];
  uint8_t length = 0U;

  do
  {
    reversed[length++] = (char)('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U && length < sizeof(reversed));
  for (uint8_t index = 0U; index < length; index++)
  {
    text[index] = reversed[length - index - 1U];
  }
  text[length] = '\0';
}

static void Cli_PrintTimeStatus(void)
{
  FlashCacheTimeStatus status;
  char text[160];

  FlashCache_TimeGet(App_GetUptimeMilliseconds(), &status);
  if (status.valid == 0U)
  {
    Cli_Write("  真实时间      : 未同步（仅使用上电毫秒时间）\r\n");
    return;
  }
  {
    char iso[40];
    char unix_ms[21];
    if (FlashCache_TimeFormat(status.unix_ms, status.utc_offset_minutes,
                              iso, sizeof(iso)) == 0U)
    {
      Cli_Write("  真实时间      : 映射无效\r\n");
      return;
    }
    Cli_FormatUint64(status.unix_ms, unix_ms);
    (void)snprintf(text, sizeof(text),
                   "  真实时间      : %s\r\n"
                   "  Unix毫秒      : %s\r\n",
                   iso, unix_ms);
    Cli_Write(text);
  }
}

static void Cli_PrintSystemStatus(void)
{
  char text[256];

  Cli_Write("\r\n========== 当前外设状态 ==========\r\n\r\n"
            "[系统]\r\n");
  (void)snprintf(text, sizeof(text),
                 "  时钟源        : %s\r\n"
                 "  系统频率      : %lu Hz\r\n"
                 "  当前模式      : %s\r\n",
                 App_UsingExternalClock() != 0U
                     ? "外部 HSE 8MHz + PLL"
                     : "内部 HSI16 + PLL（HSE回退）",
                 (unsigned long)HAL_RCC_GetHCLKFreq(),
                 Bridge_GetMode() == BRIDGE_MODE_CLI ? "命令行" : "透传");
  Cli_Write(text);
  Cli_PrintTimeStatus();

  (void)snprintf(text, sizeof(text),
                 "\r\n[串口]\r\n"
                 "  Debug UART    : USART1 / 1500000 / 8N1\r\n"
                 "  UART2         : USART2 / %lu / 8N1\r\n"
                 "  UART 电平     : 由硬件开关选择，软件不可读取\r\n",
                 (unsigned long)Bridge_GetUart2Baud());
  Cli_Write(text);

  (void)snprintf(text, sizeof(text),
                 "  Debug RX丢弃  : %lu\r\n"
                 "  Debug TX丢弃  : %lu\r\n"
                 "  UART2 RX丢弃  : %lu\r\n"
                 "  UART2 TX丢弃  : %lu\r\n"
                 "  Debug错误     : %lu\r\n"
                 "  UART2错误     : %lu\r\n\r\n",
                 (unsigned long)Bridge_GetDebugRxDrops(),
                 (unsigned long)Bridge_GetDebugTxDrops(),
                 (unsigned long)Bridge_GetUart2RxDrops(),
                 (unsigned long)Bridge_GetUart2TxDrops(),
                 (unsigned long)Bridge_GetDebugErrors(),
                 (unsigned long)Bridge_GetUart2Errors());
  Cli_Write(text);
  Cli_PrintCacheStatus();
  Cli_Write("\r\n");
  Cli_PrintPins();
  Cli_Write("\r\n==================================\r\n");
}

static void Cli_PrintHelp(void)
{
  Cli_PrintCommandList();
}

static uint8_t Cli_ParseBaud(const char *text, uint32_t *baud)
{
  char *end;
  unsigned long value;

  value = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value < UART2_MIN_BAUD ||
      value > UART2_MAX_BAUD)
  {
    return 0U;
  }
  *baud = (uint32_t)value;
  return 1U;
}

static uint8_t Cli_ParseMilliseconds(const char *text, uint32_t *duration)
{
  char *end;
  unsigned long value;

  value = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0UL || value > 5000UL)
  {
    return 0U;
  }
  *duration = (uint32_t)value;
  return 1U;
}

static uint8_t Cli_ParseUint64(const char *text, uint64_t *value)
{
  uint64_t result = 0U;

  if (text == NULL || *text == '\0')
  {
    return 0U;
  }
  while (*text != '\0')
  {
    uint8_t digit;
    if (*text < '0' || *text > '9')
    {
      return 0U;
    }
    digit = (uint8_t)(*text - '0');
    if (result > (0xFFFFFFFFFFFFFFFFULL - digit) / 10ULL)
    {
      return 0U;
    }
    result = result * 10ULL + digit;
    text++;
  }
  *value = result;
  return 1U;
}

static uint8_t Cli_ParseUtcOffset(const char *text, int16_t *minutes)
{
  uint32_t hours;
  uint32_t mins;
  int16_t value;

  if (text == NULL || strlen(text) != 6U ||
      (text[0] != '+' && text[0] != '-') || text[3] != ':' ||
      text[1] < '0' || text[1] > '9' ||
      text[2] < '0' || text[2] > '9' ||
      text[4] < '0' || text[4] > '9' ||
      text[5] < '0' || text[5] > '9')
  {
    return 0U;
  }
  hours = (uint32_t)(text[1] - '0') * 10U + (uint32_t)(text[2] - '0');
  mins = (uint32_t)(text[4] - '0') * 10U + (uint32_t)(text[5] - '0');
  if (hours > 14U || mins > 59U || (hours == 14U && mins != 0U))
  {
    return 0U;
  }
  value = (int16_t)(hours * 60U + mins);
  *minutes = text[0] == '-' ? (int16_t)-value : value;
  return 1U;
}

static uint8_t Cli_ParseLength(const char *text, uint32_t *length)
{
  char *end;
  unsigned long value;

  value = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0UL ||
      value > 0xFFFFFFFFUL)
  {
    return 0U;
  }
  *length = (uint32_t)value;
  return 1U;
}

static uint8_t Cli_DumpWriter(const uint8_t *data, uint16_t length)
{
  return Bridge_TrySendDebug(data, length);
}

static uint8_t Cli_Execute(void)
{
  char *command = cli_line;
  char *end;
  char *argument;
  uint32_t value;
  uint64_t unix_ms;
  int16_t utc_offset;
  FlashCacheDumpFormat dump_format;

  while (*command == ' ' || *command == '\t')
  {
    command++;
  }
  end = command + strlen(command);
  while (end > command && (end[-1] == ' ' || end[-1] == '\t'))
  {
    *--end = '\0';
  }
  if (*command == '\0')
  {
    Cli_PrintPrompt();
    return 0U;
  }

  if (strcmp(command, "help") == 0)
  {
    Cli_PrintHelp();
  }
  else if (strcmp(command, "status") == 0 ||
           strcmp(command, "system status") == 0)
  {
    Cli_PrintSystemStatus();
  }
  else if (strcmp(command, "mode passthrough") == 0 ||
           strcmp(command, "exit") == 0)
  {
    Cli_Write("成功：已切换到串口透传模式\r\n");
    return CLI_EXEC_EXIT;
  }
  else if (strcmp(command, "time get") == 0)
  {
    Cli_Write("[时间同步]\r\n");
    Cli_PrintTimeStatus();
  }
  else if (strncmp(command, "time set iso ", 13U) == 0)
  {
    if (FlashCache_TimeSetIso(&command[13],
                              App_GetUptimeMilliseconds()) != 0U)
    {
      Cli_Write("成功：真实时间已按ISO8601同步\r\n");
    }
    else
    {
      Cli_Write("错误：时间格式无效，例如 2026-08-02T15:30:00.000+08:00\r\n");
    }
  }
  else if (strncmp(command, "time set unix ", 14U) == 0)
  {
    char *zone = strchr(&command[14], ' ');
    if (zone != NULL)
    {
      *zone++ = '\0';
    }
    if (zone != NULL && strchr(zone, ' ') == NULL &&
        Cli_ParseUint64(&command[14], &unix_ms) != 0U &&
        Cli_ParseUtcOffset(zone, &utc_offset) != 0U &&
        FlashCache_TimeSetUnix(unix_ms, utc_offset,
                               App_GetUptimeMilliseconds()) != 0U)
    {
      Cli_Write("成功：真实时间已按Unix毫秒同步\r\n");
    }
    else
    {
      Cli_Write("错误：用法 time set unix <unix_ms> <+HH:MM>\r\n");
    }
  }
  else if (strcmp(command, "time clear") == 0)
  {
    FlashCache_TimeClear(App_GetUptimeMilliseconds());
    Cli_Write("成功：本次会话真实时间映射已清除\r\n");
  }
  else if (strcmp(command, "uart2 baud get") == 0)
  {
    char text[48];
    (void)snprintf(text, sizeof(text), "UART2当前波特率：%lu\r\n",
                   (unsigned long)Bridge_GetUart2Baud());
    Cli_Write(text);
  }
  else if (strncmp(command, "uart2 baud set ", 15U) == 0)
  {
    if (Cli_ParseBaud(&command[15], &value) != 0U &&
        Bridge_SetUart2Baud(value) != 0U)
    {
      Cli_Write("成功：UART2波特率已更新\r\n");
    }
    else
    {
      Cli_Write("错误：波特率无效，有效范围为1200~2000000\r\n");
    }
  }
  else if (strcmp(command, "uart2 baud save") == 0)
  {
    Cli_Write(AppConfig_SaveBaud(Bridge_GetUart2Baud()) != 0U ?
              "成功：UART2波特率已保存\r\n" :
              "错误：内部Flash保存失败\r\n");
  }
  else if (strcmp(command, "uart2 selftest") == 0)
  {
    uint16_t received;
    uint8_t ok;
    char text[96];

    ok = Bridge_Uart2SelfTest(&received);
    (void)snprintf(text, sizeof(text),
                   "UART2回环测试：发送32字节，接收%u字节，匹配%s\r\n",
                   (unsigned int)received, ok != 0U ? "成功" : "失败");
    Cli_Write(text);
  }
  else if (strcmp(command, "flash test") == 0)
  {
    Cli_Write(FlashCache_SelfTest() != 0U ?
              "外部Flash自检成功：擦除、写入和读回均正常\r\n" :
              "错误：外部Flash自检失败\r\n");
  }
  else if (strcmp(command, "cache status") == 0)
  {
    Cli_PrintCacheStatus();
  }
  else if (strcmp(command, "cache timestamp get") == 0)
  {
    Cli_Write("cache dump默认输出格式：");
    Cli_Write(Cli_DumpFormatName(cli_cache_dump_format));
    Cli_Write("\r\n");
  }
  else if (strcmp(command, "cache timestamp on") == 0)
  {
    cli_cache_dump_format = FLASH_CACHE_DUMP_FORMAT_TEXT;
    Cli_Write("成功：cache dump默认使用带时间可读文本格式\r\n");
  }
  else if (strcmp(command, "cache timestamp off") == 0)
  {
    cli_cache_dump_format = FLASH_CACHE_DUMP_FORMAT_RAW;
    Cli_Write("成功：cache dump默认使用Raw二进制格式\r\n");
  }
  else if (strcmp(command, "cache flush") == 0)
  {
    Cli_Write(FlashCache_RequestFlush() != 0U ?
              "成功：已请求提交RAM缓存\r\n" :
              "提示：RAM缓存为空或外部Flash不可用\r\n");
  }
  else if (strncmp(command, "cache dump", 10U) == 0 &&
           (command[10] == '\0' || command[10] == ' '))
  {
    dump_format = cli_cache_dump_format;
    argument = &command[10];
    while (*argument == ' ' || *argument == '\t')
    {
      argument++;
    }
    if (strncmp(argument, "text", 4U) == 0 &&
        (argument[4] == '\0' || argument[4] == ' ' || argument[4] == '\t'))
    {
      dump_format = FLASH_CACHE_DUMP_FORMAT_TEXT;
      argument += 4;
    }
    else if (strncmp(argument, "raw", 3U) == 0 &&
             (argument[3] == '\0' || argument[3] == ' ' ||
              argument[3] == '\t'))
    {
      dump_format = FLASH_CACHE_DUMP_FORMAT_RAW;
      argument += 3;
    }
    while (*argument == ' ' || *argument == '\t')
    {
      argument++;
    }
    if (*argument == '\0')
    {
      value = 0U;
    }
    else if (Cli_ParseLength(argument, &value) == 0U)
    {
      value = 0U;
    }
    if ((*argument == '\0' || value != 0U) &&
        FlashCache_DumpStart(value, dump_format, Cli_DumpWriter) != 0U)
    {
      return CLI_EXEC_ASYNC;
    }
    else
    {
      Cli_Write("错误：缓存忙、读取失败或长度无效\r\n");
    }
  }
  else if (strcmp(command, "cache clear") == 0)
  {
    Cli_Write(FlashCache_Clear() != 0U ? "成功：缓存已清空\r\n" :
                                        "错误：缓存清空失败\r\n");
  }
  else if (strcmp(command, "relay on") == 0 || strcmp(command, "relay off") == 0)
  {
    Control_SetRelay(command[6] == 'o' && command[7] == 'n',
                     CONTROL_SOURCE_CLI);
    Cli_Write(Control_GetRelay() != 0U ? "成功：继电器已吸合\r\n" :
                                        "成功：继电器已释放\r\n");
  }
  else if (strcmp(command, "soc power on") == 0 ||
           strcmp(command, "soc power off") == 0)
  {
    Control_SetSocPower(command[10] == 'o' && command[11] == 'n',
                        CONTROL_SOURCE_CLI);
    Cli_Write(Control_GetSocPower() != 0U ?
              "成功：SoC电源控制已断言\r\n" :
              "成功：SoC电源控制已释放\r\n");
  }
  else if (strcmp(command, "reset assert") == 0)
  {
    Control_SetReset(1U, CONTROL_SOURCE_CLI);
    Cli_Write("成功：SoC复位已断言\r\n");
  }
  else if (strcmp(command, "reset release") == 0)
  {
    Control_SetReset(0U, CONTROL_SOURCE_CLI);
    Cli_Write("成功：SoC复位已释放\r\n");
  }
  else if (strncmp(command, "reset pulse ", 12U) == 0 &&
           Cli_ParseMilliseconds(&command[12], &value) != 0U)
  {
    if (Control_StartResetPulse(value, CONTROL_SOURCE_CLI) != 0U)
    {
      Cli_Write("成功：SoC复位脉冲已开始\r\n");
    }
    else
    {
      Cli_Write("错误：已有复位脉冲正在进行\r\n");
    }
  }
  else if (strcmp(command, "loader on") == 0 || strcmp(command, "loader off") == 0)
  {
    Control_SetLoader(command[7] == 'o' && command[8] == 'n',
                      CONTROL_SOURCE_CLI);
    Cli_Write(Control_GetLoader() != 0U ? "成功：Loader已断言\r\n" :
                                         "成功：Loader已释放\r\n");
  }
  else if (strcmp(command, "maskrom on") == 0 ||
           strcmp(command, "maskrom off") == 0)
  {
    Control_SetMaskrom(command[8] == 'o' && command[9] == 'n',
                       CONTROL_SOURCE_CLI);
    Cli_Write(Control_GetMaskrom() != 0U ? "成功：MaskROM已断言\r\n" :
                                          "成功：MaskROM已释放\r\n");
  }
  else if (strcmp(command, "pins status") == 0)
  {
    Cli_PrintPins();
  }
  else
  {
    Cli_Write("错误：未知命令，请输入 help 查看帮助\r\n");
  }

  Cli_PrintPrompt();
  return CLI_EXEC_NORMAL;
}

void Cli_Init(void)
{
  cli_line[0] = '\0';
  cli_length = 0U;
  cli_skip_lf = 0U;
  cli_escape_state = 0U;
  cli_history_count = 0U;
  cli_history_position = -1;
  cli_history_draft[0] = '\0';
  cli_cache_dump_format = FLASH_CACHE_DUMP_FORMAT_TEXT;
}

void Cli_Task(void)
{
  FlashCacheDumpResult result;
  uint32_t dumped;
  FlashCacheDumpFormat dump_format;
  uint8_t had_records;
  char text[80];

  if (FlashCache_DumpTakeResult(&result, &dumped, &dump_format,
                                &had_records) == 0U)
  {
    return;
  }
  if (result == FLASH_CACHE_DUMP_COMPLETE &&
      dump_format == FLASH_CACHE_DUMP_FORMAT_RAW)
  {
    return;
  }
  if (result == FLASH_CACHE_DUMP_COMPLETE)
  {
    if (had_records == 0U)
    {
      Cli_Write("提示：缓存为空\r\n");
    }
    else
    {
      (void)snprintf(text, sizeof(text), "导出完成：%lu 字节UART数据\r\n",
                     (unsigned long)dumped);
      Cli_Write(text);
    }
  }
  else if (result == FLASH_CACHE_DUMP_CANCELLED)
  {
    Cli_Write("\r\n导出已由Ctrl+C取消\r\n");
  }
  else if (result == FLASH_CACHE_DUMP_OVERFLOW)
  {
    Cli_Write("\r\n错误：导出期间RAM快照被新数据覆盖，导出已中止\r\n");
  }
  else
  {
    Cli_Write("\r\n错误：缓存读取或记录校验失败，导出已中止\r\n");
  }
  Cli_PrintPrompt();
}

CliAction Cli_ProcessByte(uint8_t byte)
{
  uint8_t execute_result;

  if (FlashCache_DumpIsActive() != 0U)
  {
    if (byte == 0x03U)
    {
      FlashCache_DumpCancel();
    }
    return CLI_ACTION_NONE;
  }

  if (byte == 0x04U)
  {
    cli_line[0] = '\0';
    cli_length = 0U;
    cli_escape_state = 0U;
    cli_history_position = -1;
    cli_history_draft[0] = '\0';
    Cli_Write("\r\n成功：已切换到串口透传模式\r\n");
    return CLI_ACTION_EXIT;
  }

  if (cli_escape_state != 0U)
  {
    if (cli_escape_state == 1U && (byte == '[' || byte == 'O'))
    {
      cli_escape_state = 2U;
      return CLI_ACTION_NONE;
    }
    cli_escape_state = 0U;
    if (byte == 'A')
    {
      Cli_HistoryMove(-1);
    }
    else if (byte == 'B')
    {
      Cli_HistoryMove(1);
    }
    return CLI_ACTION_NONE;
  }

  if (byte == 0x1BU)
  {
    cli_escape_state = 1U;
    return CLI_ACTION_NONE;
  }

  if (byte == '\t')
  {
    Cli_HandleTab();
    return CLI_ACTION_NONE;
  }

  if (byte == '\r' || byte == '\n')
  {
    if (byte == '\n' && cli_skip_lf != 0U)
    {
      cli_skip_lf = 0U;
      return CLI_ACTION_NONE;
    }
    cli_skip_lf = byte == '\r' ? 1U : 0U;
    (void)Bridge_SendDebugBlocking((const uint8_t *)"\r\n", 2U);
    cli_line[cli_length] = '\0';
    Cli_HistoryAdd();
    execute_result = Cli_Execute();
    cli_length = 0U;
    cli_line[0] = '\0';
    cli_history_position = -1;
    cli_history_draft[0] = '\0';
    if (execute_result == CLI_EXEC_EXIT)
    {
      return byte == '\r' ? CLI_ACTION_EXIT_SKIP_LF : CLI_ACTION_EXIT;
    }
    if (execute_result == CLI_EXEC_ASYNC)
    {
      return CLI_ACTION_NONE;
    }
    return CLI_ACTION_NONE;
  }

  if (byte == '\b' || byte == 0x7FU)
  {
    if (cli_length != 0U)
    {
      cli_length--;
      cli_line[cli_length] = '\0';
      cli_history_position = -1;
      (void)Bridge_SendDebugBlocking((const uint8_t *)"\b \b", 3U);
    }
    return CLI_ACTION_NONE;
  }

  if (byte >= 0x20U && byte <= 0x7EU && cli_length < CLI_LINE_SIZE - 1U)
  {
    cli_line[cli_length++] = (char)byte;
    cli_line[cli_length] = '\0';
    cli_history_position = -1;
    (void)Bridge_SendDebugBlocking(&byte, 1U);
  }
  return CLI_ACTION_NONE;
}

void Cli_PrintPrompt(void)
{
  Cli_Write("cli> ");
}

void Cli_OnEnter(void)
{
  Cli_Write("\r\n已进入命令行模式，输入 help 或按 Tab 查看命令。\r\n");
  Cli_PrintPrompt();
}
