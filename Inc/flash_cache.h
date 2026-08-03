#ifndef FLASH_CACHE_H
#define FLASH_CACHE_H

#include <stdint.h>

typedef enum
{
  FLASH_CACHE_DIRECTION_RX = 0,
  FLASH_CACHE_DIRECTION_TX = 1,
  FLASH_CACHE_RECORD_CONTROL = 2,
  FLASH_CACHE_RECORD_TIME_SYNC = 3
} FlashCacheRecordType;

typedef enum
{
  FLASH_CACHE_CONTROL_RELAY = 1,
  FLASH_CACHE_CONTROL_SOC_POWER,
  FLASH_CACHE_CONTROL_RESET_ASSERT,
  FLASH_CACHE_CONTROL_RESET_RELEASE,
  FLASH_CACHE_CONTROL_RESET_PULSE,
  FLASH_CACHE_CONTROL_LOADER,
  FLASH_CACHE_CONTROL_MASKROM
} FlashCacheControlEvent;

typedef enum
{
  FLASH_CACHE_SOURCE_CLI = 0,
  FLASH_CACHE_SOURCE_SW3 = 1
} FlashCacheEventSource;

typedef enum
{
  FLASH_CACHE_DUMP_NONE = 0,
  FLASH_CACHE_DUMP_COMPLETE,
  FLASH_CACHE_DUMP_CANCELLED,
  FLASH_CACHE_DUMP_ERROR,
  FLASH_CACHE_DUMP_OVERFLOW
} FlashCacheDumpResult;

typedef enum
{
  FLASH_CACHE_DUMP_FORMAT_RAW = 0,
  FLASH_CACHE_DUMP_FORMAT_HEX,
  FLASH_CACHE_DUMP_FORMAT_TEXT
} FlashCacheDumpFormat;

typedef struct
{
  uint8_t valid;
  uint64_t unix_ms;
  int16_t utc_offset_minutes;
} FlashCacheTimeStatus;

typedef struct
{
  uint8_t present;
  uint8_t jedec_id[3];
  uint8_t busy;
  uint8_t paused;
  uint8_t dump_active;
  uint8_t dump_format;
  uint32_t session_id;
  uint32_t committed_bytes;
  uint32_t committed_original_bytes;
  uint32_t committed_compressed_bytes;
  uint32_t pending_bytes;
  uint32_t committed_records;
  uint32_t pending_records;
  uint32_t committed_blocks;
  uint32_t pending_age_ms;
  uint32_t rx_bytes;
  uint32_t tx_bytes;
  uint32_t dropped_bytes;
  uint32_t dropped_records;
  uint32_t overwritten_blocks;
  uint32_t write_blocks;
  uint32_t erase_sectors;
  uint32_t write_errors;
  uint32_t event_records;
  uint32_t dump_snapshot_bytes;
  uint32_t dump_output_bytes;
  uint32_t dump_cancel_count;
  uint32_t dump_overflow_count;
  uint32_t active_free_bytes;
  uint8_t spare_ready;
} FlashCacheStatus;

typedef uint8_t (*FlashCacheWriter)(const uint8_t *data, uint16_t length);

void FlashCache_Init(void);
void FlashCache_Push(FlashCacheRecordType type, const uint8_t *data,
                     uint16_t length, uint64_t timestamp_ms);
void FlashCache_PushControl(FlashCacheControlEvent event,
                            FlashCacheEventSource source, uint8_t value,
                            uint32_t argument, uint64_t timestamp_ms);
void FlashCache_Task(void);
uint8_t FlashCache_RequestFlush(void);
uint8_t FlashCache_Clear(void);
uint8_t FlashCache_SelfTest(void);
void FlashCache_GetStatus(FlashCacheStatus *status);
uint8_t FlashCache_TimeSetIso(const char *text, uint64_t uptime_ms);
uint8_t FlashCache_TimeSetUnix(uint64_t unix_ms, int16_t utc_offset_minutes,
                               uint64_t uptime_ms);
void FlashCache_TimeClear(uint64_t uptime_ms);
void FlashCache_TimeGet(uint64_t uptime_ms, FlashCacheTimeStatus *status);
uint8_t FlashCache_TimeFormat(uint64_t unix_ms, int16_t utc_offset_minutes,
                              char *text, uint16_t size);
uint8_t FlashCache_DumpStart(uint32_t max_bytes, FlashCacheDumpFormat format,
                             FlashCacheWriter writer);
void FlashCache_DumpTask(void);
void FlashCache_DumpCancel(void);
uint8_t FlashCache_DumpIsActive(void);
uint8_t FlashCache_DumpTakeResult(FlashCacheDumpResult *result,
                                  uint32_t *dumped_bytes,
                                  FlashCacheDumpFormat *format,
                                  uint8_t *had_records);

#endif
