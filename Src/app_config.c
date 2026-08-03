#include "app_config.h"
#include "crc32.h"
#include "main.h"

#include <stddef.h>
#include <string.h>

#define APP_CONFIG_ADDRESS 0x0801F800UL
#define APP_CONFIG_MAGIC 0x43464731UL
#define APP_CONFIG_VERSION 1UL

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t baud;
  uint32_t crc;
  uint32_t reserved[4];
} AppConfigRecord;

static uint8_t AppConfig_IsValid(const AppConfigRecord *record)
{
  if (record->magic != APP_CONFIG_MAGIC ||
      record->version != APP_CONFIG_VERSION ||
      record->baud < UART2_MIN_BAUD || record->baud > UART2_MAX_BAUD)
  {
    return 0U;
  }

  return (Crc32_Calculate(record, offsetof(AppConfigRecord, crc)) == record->crc)
             ? 1U
             : 0U;
}

uint32_t AppConfig_LoadBaud(void)
{
  const AppConfigRecord *record = (const AppConfigRecord *)APP_CONFIG_ADDRESS;

  return AppConfig_IsValid(record) != 0U ? record->baud : UART2_DEFAULT_BAUD;
}

uint8_t AppConfig_SaveBaud(uint32_t baud)
{
  AppConfigRecord record = {0};
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_error = 0U;

  if (baud < UART2_MIN_BAUD || baud > UART2_MAX_BAUD)
  {
    return 0U;
  }

  record.magic = APP_CONFIG_MAGIC;
  record.version = APP_CONFIG_VERSION;
  record.baud = baud;
  record.crc = Crc32_Calculate(&record, offsetof(AppConfigRecord, crc));

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return 0U;
  }

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks = FLASH_BANK_1;
  erase.Page = FLASH_PAGE_NB - 1U;
  erase.NbPages = 1U;

  if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK)
  {
    HAL_FLASH_Lock();
    return 0U;
  }

  for (uint32_t offset = 0U; offset < sizeof(record); offset += 8U)
  {
    uint64_t value = 0ULL;
    memcpy(&value, ((const uint8_t *)&record) + offset, sizeof(value));
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          APP_CONFIG_ADDRESS + offset, value) != HAL_OK)
    {
      HAL_FLASH_Lock();
      return 0U;
    }
  }

  HAL_FLASH_Lock();
  return AppConfig_IsValid((const AppConfigRecord *)APP_CONFIG_ADDRESS);
}
