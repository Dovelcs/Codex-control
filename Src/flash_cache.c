#include "flash_cache.h"
#include "crc32.h"
#include "main.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FLASH_CMD_WRITE_ENABLE 0x06U
#define FLASH_CMD_READ_STATUS 0x05U
#define FLASH_CMD_PAGE_PROGRAM 0x02U
#define FLASH_CMD_READ_DATA 0x03U
#define FLASH_CMD_SECTOR_ERASE 0x20U
#define FLASH_CMD_JEDEC_ID 0x9FU
#define FLASH_CMD_RELEASE_POWER_DOWN 0xABU
#define FLASH_CMD_RESET_ENABLE 0x66U
#define FLASH_CMD_RESET 0x99U

#define GD25_TOTAL_SIZE 0x01000000UL
#define GD25_SECTOR_SIZE 0x1000UL
#define GD25_PAGE_SIZE 0x100UL
#define CACHE_META_ADDRESS 0x000000UL
#define CACHE_DATA_ADDRESS 0x001000UL
#define CACHE_TEST_ADDRESS (GD25_TOTAL_SIZE - GD25_SECTOR_SIZE)
#define CACHE_DATA_SECTOR_COUNT \
  ((CACHE_TEST_ADDRESS - CACHE_DATA_ADDRESS) / GD25_SECTOR_SIZE)
#define CACHE_PAGES_PER_SECTOR (GD25_SECTOR_SIZE / GD25_PAGE_SIZE)
#define CACHE_SECTOR_DATA_OFFSET GD25_PAGE_SIZE
#define CACHE_INPUT_SIZE 32768U
#define CACHE_RECORD_DATA_MAX 128U
#define CACHE_FLUSH_BYTES 1024U
#define CACHE_FLUSH_INTERVAL_MS 60000UL
#define CACHE_WRITE_RETRIES 3U
#define CACHE_ORIGINAL_MAX 1280U
#define CACHE_COMPRESSED_MAX 1304U
#define CACHE_EXTENT_MAX_SIZE 1536U
#define CACHE_DUMP_OUTPUT_SIZE 768U
#define CACHE_DUMP_TX_CHUNK 256U
#define CACHE_META_MAGIC 0x334D4346UL
#define CACHE_SECTOR_MAGIC 0x33534346UL
#define CACHE_EXTENT_MAGIC 0x33454346UL
#define CACHE_VERSION 3U
#define CACHE_CODEC_LZ4 1U

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint32_t epoch;
  uint32_t sequence;
  uint32_t session_id;
  uint32_t raw_bytes;
  uint32_t compressed_crc;
  uint32_t original_crc;
  uint16_t compressed_length;
  uint16_t original_length;
  uint16_t record_count;
  uint16_t version;
  uint8_t extent_pages;
  uint8_t codec;
  uint16_t reserved16;
  uint32_t reserved;
  uint32_t header_crc;
} CacheExtentHeader;

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint32_t version;
  uint32_t epoch;
  uint32_t generation;
  uint32_t first_sequence;
  uint32_t reserved[2];
  uint32_t crc;
} CacheSectorHeader;

typedef struct __attribute__((packed))
{
  uint64_t timestamp_ms;
  uint16_t length;
  uint8_t direction;
  uint8_t reserved;
} CacheRecordHeader;

typedef struct __attribute__((packed))
{
  uint8_t event;
  uint8_t source;
  uint8_t value;
  uint8_t reserved;
  uint32_t argument;
} CacheControlPayload;

typedef struct __attribute__((packed))
{
  uint64_t unix_ms;
  int16_t utc_offset_minutes;
  uint8_t valid;
  uint8_t reserved;
} CacheTimePayload;

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t epoch;
  uint32_t reserved[4];
  uint32_t crc;
} CacheMeta;

typedef enum
{
  CACHE_IO_IDLE = 0,
  CACHE_IO_PROGRAM_WAIT,
  CACHE_IO_SECTOR_HEADER_WAIT,
  CACHE_IO_SPARE_ERASE_WAIT
} CacheIoState;

typedef enum
{
  CACHE_DUMP_IDLE = 0,
  CACHE_DUMP_WAIT_IO,
  CACHE_DUMP_FLASH,
  CACHE_DUMP_STAGED,
  CACHE_DUMP_RAM
} CacheDumpState;

#define CACHE_EXTENT_HEADER_SIZE ((uint16_t)sizeof(CacheExtentHeader))

_Static_assert(sizeof(CacheExtentHeader) == 48U, "cache extent header size");
_Static_assert(sizeof(CacheSectorHeader) == 32U, "cache sector header size");
_Static_assert(sizeof(CacheRecordHeader) == 12U, "cache record header size");
_Static_assert(sizeof(CacheControlPayload) == 8U, "control payload size");
_Static_assert(sizeof(CacheTimePayload) == 12U, "time payload size");
_Static_assert(sizeof(CacheMeta) == 32U, "cache meta size");

static uint8_t cache_present;
static uint8_t cache_jedec[3];
static uint32_t cache_epoch = 1U;
static uint32_t cache_next_sequence = 1U;
static uint32_t cache_current_session = 1U;
static uint32_t cache_oldest_sector;
static uint32_t cache_active_sector;
static uint16_t cache_active_offset = CACHE_SECTOR_DATA_OFFSET;
static uint32_t cache_spare_sector = 1U;
static uint8_t cache_spare_ready;
static uint32_t cache_next_generation = 1U;
static uint32_t cache_committed_blocks;
static uint32_t cache_committed_records;
static uint32_t cache_committed_bytes;
static uint32_t cache_committed_original_bytes;
static uint32_t cache_committed_compressed_bytes;
static uint32_t cache_overwritten_blocks;

static uint8_t cache_input[CACHE_INPUT_SIZE];
static uint16_t cache_input_head;
static uint16_t cache_input_tail;
static uint32_t cache_input_records;
static uint32_t cache_input_bytes;
static uint32_t cache_pending_since_tick;

static uint8_t cache_original[CACHE_ORIGINAL_MAX];
static uint8_t cache_extent[CACHE_EXTENT_MAX_SIZE];
static uint16_t cache_lz4_hash[512];
static uint8_t cache_staged_valid;
static uint16_t cache_staged_records;
static uint32_t cache_staged_bytes;
static uint16_t cache_staged_original_length;
static uint16_t cache_staged_compressed_length;
static uint8_t cache_staged_pages;
static uint8_t cache_flush_all;

static CacheIoState cache_io_state;
static uint32_t cache_target_address;
static uint8_t cache_program_page;
static uint8_t cache_write_attempts;
static uint8_t cache_paused;
static uint32_t cache_removed_blocks;
static uint32_t cache_removed_records;
static uint32_t cache_removed_bytes;
static uint32_t cache_removed_original_bytes;
static uint32_t cache_removed_compressed_bytes;
static uint8_t cache_sector_header_page[GD25_PAGE_SIZE];

static uint32_t cache_rx_bytes;
static uint32_t cache_tx_bytes;
static uint32_t cache_dropped_bytes;
static uint32_t cache_dropped_records;
static uint32_t cache_write_blocks;
static uint32_t cache_erase_sectors;
static uint32_t cache_write_errors;
static uint32_t cache_event_records;

static uint8_t cache_time_valid;
static uint64_t cache_time_unix_ms;
static uint64_t cache_time_uptime_ms;
static int16_t cache_time_utc_offset;

static CacheDumpState cache_dump_state;
static FlashCacheWriter cache_dump_writer;
static FlashCacheDumpFormat cache_dump_format;
static uint32_t cache_dump_max_bytes;
static uint32_t cache_dump_snapshot_bytes;
static uint32_t cache_dump_output_bytes;
static uint32_t cache_dump_skip_bytes;
static uint32_t cache_dump_snapshot_blocks;
static uint32_t cache_dump_snapshot_oldest;
static uint8_t cache_dump_snapshot_staged;
static uint16_t cache_dump_snapshot_staged_length;
static uint32_t cache_dump_scan_sector;
static uint8_t cache_dump_scan_page;
static uint32_t cache_dump_sectors_scanned;
static uint32_t cache_dump_flash_emitted;
static uint16_t cache_dump_ram_position;
static uint16_t cache_dump_ram_end;
static uint8_t cache_dump_ram_overflow;
static uint8_t cache_dump_had_records;
static uint8_t cache_dump_compressed[CACHE_COMPRESSED_MAX];
static uint8_t cache_dump_payload[CACHE_ORIGINAL_MAX];
static uint16_t cache_dump_payload_length;
static uint16_t cache_dump_payload_offset;
static uint32_t cache_dump_payload_session;
static uint8_t cache_dump_output[CACHE_DUMP_OUTPUT_SIZE];
static uint16_t cache_dump_output_length;
static uint16_t cache_dump_output_offset;
static uint8_t cache_dump_time_valid;
static uint32_t cache_dump_time_session;
static uint64_t cache_dump_time_unix_ms;
static uint64_t cache_dump_time_uptime_ms;
static int16_t cache_dump_time_utc_offset;
static FlashCacheDumpResult cache_dump_result;
static uint32_t cache_dump_result_bytes;
static FlashCacheDumpFormat cache_dump_result_format;
static uint8_t cache_dump_result_had_records;
static uint32_t cache_dump_cancel_count;
static uint32_t cache_dump_overflow_count;

static uint32_t Cache_SectorAddress(uint32_t sector)
{
  return CACHE_DATA_ADDRESS + sector * GD25_SECTOR_SIZE;
}


static uint16_t Cache_Lz4WriteLength(uint8_t *output, uint16_t output_size,
                                     uint16_t written, uint16_t length)
{
  while (length >= 255U)
  {
    if (written >= output_size)
    {
      return 0xFFFFU;
    }
    output[written++] = 255U;
    length = (uint16_t)(length - 255U);
  }
  if (written >= output_size)
  {
    return 0xFFFFU;
  }
  output[written++] = (uint8_t)length;
  return written;
}

static uint16_t Cache_Lz4Compress(const uint8_t *source,
                                  uint16_t source_length,
                                  uint8_t *output,
                                  uint16_t output_size)
{
  uint16_t anchor = 0U;
  uint16_t input = 0U;
  uint16_t written = 0U;

  memset(cache_lz4_hash, 0xFF, sizeof(cache_lz4_hash));
  while (input + 3U < source_length)
  {
    uint16_t hash = (uint16_t)(((uint16_t)source[input] * 31U ^
                                (uint16_t)source[input + 1U] * 17U ^
                                (uint16_t)source[input + 2U] * 7U ^
                                source[input + 3U]) & 0x01FFU);
    uint16_t reference = cache_lz4_hash[hash];
    cache_lz4_hash[hash] = input;
    if (reference != 0xFFFFU && input > reference &&
        source[reference] == source[input] &&
        source[reference + 1U] == source[input + 1U] &&
        source[reference + 2U] == source[input + 2U] &&
        source[reference + 3U] == source[input + 3U])
    {
      uint16_t literal_length = (uint16_t)(input - anchor);
      uint16_t match_length = 4U;
      uint16_t token_position;
      uint8_t token;
      while (input + match_length < source_length &&
             source[reference + match_length] == source[input + match_length])
      {
        match_length++;
      }
      if (written >= output_size)
      {
        return 0U;
      }
      token_position = written++;
      token = (uint8_t)((literal_length < 15U ? literal_length : 15U) << 4U);
      if (literal_length >= 15U)
      {
        written = Cache_Lz4WriteLength(output, output_size, written,
                                       (uint16_t)(literal_length - 15U));
        if (written == 0xFFFFU)
        {
          return 0U;
        }
      }
      if ((uint32_t)written + literal_length + 2U > output_size)
      {
        return 0U;
      }
      memcpy(&output[written], &source[anchor], literal_length);
      written = (uint16_t)(written + literal_length);
      output[written++] = (uint8_t)(input - reference);
      output[written++] = (uint8_t)((input - reference) >> 8U);
      match_length = (uint16_t)(match_length - 4U);
      token |= (uint8_t)(match_length < 15U ? match_length : 15U);
      if (match_length >= 15U)
      {
        written = Cache_Lz4WriteLength(output, output_size, written,
                                       (uint16_t)(match_length - 15U));
        if (written == 0xFFFFU)
        {
          return 0U;
        }
      }
      output[token_position] = token;
      input = (uint16_t)(input + match_length + 4U);
      anchor = input;
      continue;
    }
    input++;
  }

  {
    uint16_t literal_length = (uint16_t)(source_length - anchor);
    uint16_t token_position;
    uint8_t token;
    if (written >= output_size)
    {
      return 0U;
    }
    token_position = written++;
    token = (uint8_t)((literal_length < 15U ? literal_length : 15U) << 4U);
    if (literal_length >= 15U)
    {
      written = Cache_Lz4WriteLength(output, output_size, written,
                                     (uint16_t)(literal_length - 15U));
      if (written == 0xFFFFU)
      {
        return 0U;
      }
    }
    if ((uint32_t)written + literal_length > output_size)
    {
      return 0U;
    }
    memcpy(&output[written], &source[anchor], literal_length);
    written = (uint16_t)(written + literal_length);
    output[token_position] = token;
  }
  return written;
}

static uint8_t Cache_Lz4Decompress(const uint8_t *source,
                                   uint16_t source_length,
                                   uint8_t *output,
                                   uint16_t output_length)
{
  uint16_t input = 0U;
  uint16_t written = 0U;

  while (input < source_length)
  {
    uint8_t token = source[input++];
    uint32_t literal_length = token >> 4U;
    uint32_t match_length;
    if (literal_length == 15U)
    {
      uint8_t value;
      do
      {
        if (input >= source_length)
        {
          return 0U;
        }
        value = source[input++];
        literal_length += value;
      } while (value == 255U);
    }
    if ((uint32_t)input + literal_length > source_length ||
        (uint32_t)written + literal_length > output_length)
    {
      return 0U;
    }
    memcpy(&output[written], &source[input], literal_length);
    input = (uint16_t)(input + literal_length);
    written = (uint16_t)(written + literal_length);
    if (input == source_length)
    {
      break;
    }
    if ((uint32_t)input + 2U > source_length)
    {
      return 0U;
    }
    {
      uint16_t distance =
          (uint16_t)(source[input] | ((uint16_t)source[input + 1U] << 8U));
      input = (uint16_t)(input + 2U);
      match_length = token & 0x0FU;
      if (match_length == 15U)
      {
        uint8_t value;
        do
        {
          if (input >= source_length)
          {
            return 0U;
          }
          value = source[input++];
          match_length += value;
        } while (value == 255U);
      }
      match_length += 4U;
      if (distance == 0U || distance > written ||
          (uint32_t)written + match_length > output_length)
      {
        return 0U;
      }
      while (match_length-- != 0U)
      {
        output[written] = output[written - distance];
        written++;
      }
    }
  }
  return input == source_length && written == output_length ? 1U : 0U;
}

static void Flash_Select(void)
{
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);
}

static void Flash_Deselect(void)
{
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
}

static uint8_t Flash_Command(uint8_t command)
{
  HAL_StatusTypeDef status;

  Flash_Select();
  status = HAL_SPI_Transmit(&hspi1, &command, 1U, 100U);
  Flash_Deselect();
  return status == HAL_OK ? 1U : 0U;
}

static uint8_t Flash_ReadStatus(uint8_t *value)
{
  uint8_t command = FLASH_CMD_READ_STATUS;
  HAL_StatusTypeDef status;

  Flash_Select();
  status = HAL_SPI_Transmit(&hspi1, &command, 1U, 100U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Receive(&hspi1, value, 1U, 100U);
  }
  Flash_Deselect();
  return status == HAL_OK ? 1U : 0U;
}

static uint8_t Flash_IsReady(uint8_t *ready)
{
  uint8_t status;

  if (Flash_ReadStatus(&status) == 0U)
  {
    return 0U;
  }
  *ready = (status & 0x01U) == 0U ? 1U : 0U;
  return 1U;
}

static uint8_t Flash_WaitReady(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint8_t ready;

  do
  {
    if (Flash_IsReady(&ready) == 0U)
    {
      return 0U;
    }
    if (ready != 0U)
    {
      return 1U;
    }
  } while ((HAL_GetTick() - start) <= timeout_ms);
  return 0U;
}

static uint8_t Flash_WriteEnable(void)
{
  return Flash_Command(FLASH_CMD_WRITE_ENABLE);
}

static uint8_t Flash_Read(uint32_t address, uint8_t *data, uint16_t length)
{
  uint8_t command[4] = {
      FLASH_CMD_READ_DATA,
      (uint8_t)(address >> 16U),
      (uint8_t)(address >> 8U),
      (uint8_t)address};
  HAL_StatusTypeDef status;

  Flash_Select();
  status = HAL_SPI_Transmit(&hspi1, command, sizeof(command), 100U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Receive(&hspi1, data, length, 2000U);
  }
  Flash_Deselect();
  return status == HAL_OK ? 1U : 0U;
}

static uint8_t Flash_StartPageProgram(uint32_t address, const uint8_t *data,
                                      uint16_t length)
{
  uint8_t command[4] = {
      FLASH_CMD_PAGE_PROGRAM,
      (uint8_t)(address >> 16U),
      (uint8_t)(address >> 8U),
      (uint8_t)address};
  HAL_StatusTypeDef status;

  if (length == 0U || length > GD25_PAGE_SIZE ||
      ((address & (GD25_PAGE_SIZE - 1U)) + length) > GD25_PAGE_SIZE ||
      Flash_WriteEnable() == 0U)
  {
    return 0U;
  }
  Flash_Select();
  status = HAL_SPI_Transmit(&hspi1, command, sizeof(command), 100U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Transmit(&hspi1, (uint8_t *)data, length, 1000U);
  }
  Flash_Deselect();
  return status == HAL_OK ? 1U : 0U;
}

static uint8_t Flash_PageProgramBlocking(uint32_t address,
                                         const uint8_t *data,
                                         uint16_t length)
{
  return Flash_StartPageProgram(address, data, length) != 0U &&
         Flash_WaitReady(2000U) != 0U;
}

static uint8_t Flash_WriteBlocking(uint32_t address, const uint8_t *data,
                                   uint32_t length)
{
  while (length != 0U)
  {
    uint32_t page_offset = address & (GD25_PAGE_SIZE - 1U);
    uint32_t chunk = GD25_PAGE_SIZE - page_offset;
    if (chunk > length)
    {
      chunk = length;
    }
    if (Flash_PageProgramBlocking(address, data, (uint16_t)chunk) == 0U)
    {
      return 0U;
    }
    address += chunk;
    data += chunk;
    length -= chunk;
  }
  return 1U;
}

static uint8_t Flash_StartSectorErase(uint32_t address)
{
  uint8_t command[4] = {
      FLASH_CMD_SECTOR_ERASE,
      (uint8_t)(address >> 16U),
      (uint8_t)(address >> 8U),
      (uint8_t)address};
  HAL_StatusTypeDef status;

  if (Flash_WriteEnable() == 0U)
  {
    return 0U;
  }
  Flash_Select();
  status = HAL_SPI_Transmit(&hspi1, command, sizeof(command), 100U);
  Flash_Deselect();
  return status == HAL_OK ? 1U : 0U;
}

static uint8_t Flash_EraseSectorBlocking(uint32_t address)
{
  return Flash_StartSectorErase(address) != 0U &&
         Flash_WaitReady(5000U) != 0U;
}

static uint8_t Flash_ReadJedec(uint8_t *id)
{
  uint8_t command = FLASH_CMD_JEDEC_ID;
  HAL_StatusTypeDef status;

  Flash_Select();
  status = HAL_SPI_Transmit(&hspi1, &command, 1U, 100U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Receive(&hspi1, id, 3U, 100U);
  }
  Flash_Deselect();
  return status == HAL_OK ? 1U : 0U;
}

static uint8_t Cache_MetaValid(const CacheMeta *meta)
{
  return meta->magic == CACHE_META_MAGIC && meta->version == CACHE_VERSION &&
         Crc32_Calculate(meta, offsetof(CacheMeta, crc)) == meta->crc;
}

static uint8_t Cache_WriteMeta(void)
{
  CacheMeta meta = {0};

  meta.magic = CACHE_META_MAGIC;
  meta.version = CACHE_VERSION;
  meta.epoch = cache_epoch;
  meta.crc = Crc32_Calculate(&meta, offsetof(CacheMeta, crc));
  return Flash_EraseSectorBlocking(CACHE_META_ADDRESS) != 0U &&
         Flash_WriteBlocking(CACHE_META_ADDRESS, (const uint8_t *)&meta,
                             sizeof(meta)) != 0U;
}

static uint8_t Cache_SectorHeaderValid(const CacheSectorHeader *header)
{
  return header->magic == CACHE_SECTOR_MAGIC &&
         header->version == CACHE_VERSION && header->epoch == cache_epoch &&
         Crc32_Calculate(header, offsetof(CacheSectorHeader, crc)) ==
             header->crc;
}

static uint8_t Cache_ReadSectorHeader(uint32_t sector,
                                      CacheSectorHeader *header)
{
  return Flash_Read(Cache_SectorAddress(sector), (uint8_t *)header,
                    sizeof(*header));
}

static void Cache_PrepareSectorHeader(uint32_t generation,
                                      uint32_t first_sequence)
{
  CacheSectorHeader header = {0};

  memset(cache_sector_header_page, 0xFF, sizeof(cache_sector_header_page));
  header.magic = CACHE_SECTOR_MAGIC;
  header.version = CACHE_VERSION;
  header.epoch = cache_epoch;
  header.generation = generation;
  header.first_sequence = first_sequence;
  header.crc = Crc32_Calculate(&header, offsetof(CacheSectorHeader, crc));
  memcpy(cache_sector_header_page, &header, sizeof(header));
}

static uint8_t Cache_WriteSectorHeaderBlocking(uint32_t sector,
                                               uint32_t generation,
                                               uint32_t first_sequence)
{
  Cache_PrepareSectorHeader(generation, first_sequence);
  return Flash_PageProgramBlocking(Cache_SectorAddress(sector),
                                   cache_sector_header_page,
                                   sizeof(cache_sector_header_page));
}

static uint8_t Cache_ExtentHeaderValid(const CacheExtentHeader *header)
{
  uint16_t extent_size =
      (uint16_t)((CACHE_EXTENT_HEADER_SIZE + header->compressed_length +
                  GD25_PAGE_SIZE - 1U) &
                 ~(GD25_PAGE_SIZE - 1U));

  return header->magic == CACHE_EXTENT_MAGIC &&
         header->version == CACHE_VERSION && header->epoch == cache_epoch &&
         header->codec == CACHE_CODEC_LZ4 &&
         header->compressed_length != 0U &&
         header->compressed_length <= CACHE_COMPRESSED_MAX &&
         header->original_length != 0U &&
         header->original_length <= CACHE_ORIGINAL_MAX &&
         header->extent_pages != 0U &&
         header->extent_pages * GD25_PAGE_SIZE == extent_size &&
         Crc32_Calculate(header, offsetof(CacheExtentHeader, header_crc)) ==
             header->header_crc;
}

static uint8_t Cache_ReadExtentHeader(uint32_t address,
                                      CacheExtentHeader *header)
{
  return Flash_Read(address, (uint8_t *)header, sizeof(*header));
}

static uint8_t Cache_ReadValidExtent(uint32_t address,
                                     CacheExtentHeader *header,
                                     uint8_t *compressed,
                                     uint8_t *original)
{
  if (Cache_ReadExtentHeader(address, header) == 0U ||
      Cache_ExtentHeaderValid(header) == 0U ||
      Flash_Read(address + CACHE_EXTENT_HEADER_SIZE, compressed,
                 header->compressed_length) == 0U ||
      Crc32_Calculate(compressed, header->compressed_length) !=
          header->compressed_crc ||
      Cache_Lz4Decompress(compressed, header->compressed_length, original,
                          header->original_length) == 0U)
  {
    return 0U;
  }
  return Crc32_Calculate(original, header->original_length) ==
                 header->original_crc
             ? 1U
             : 0U;
}

static uint8_t Cache_AreaErased(uint32_t address, uint32_t length)
{
  uint8_t buffer[64];

  while (length != 0U)
  {
    uint16_t chunk = length > sizeof(buffer) ? sizeof(buffer) : (uint16_t)length;
    if (Flash_Read(address, buffer, chunk) == 0U)
    {
      return 0U;
    }
    for (uint16_t index = 0U; index < chunk; index++)
    {
      if (buffer[index] != 0xFFU)
      {
        return 0U;
      }
    }
    address += chunk;
    length -= chunk;
  }
  return 1U;
}

static uint8_t Cache_SectorErased(uint32_t sector)
{
  return Cache_AreaErased(Cache_SectorAddress(sector), GD25_SECTOR_SIZE);
}

static uint16_t Cache_RingUsed(void)
{
  return cache_input_head >= cache_input_tail
             ? (uint16_t)(cache_input_head - cache_input_tail)
             : (uint16_t)(CACHE_INPUT_SIZE - cache_input_tail +
                          cache_input_head);
}

static uint16_t Cache_RingFree(void)
{
  return (uint16_t)(CACHE_INPUT_SIZE - Cache_RingUsed() - 1U);
}

static uint8_t Cache_RecordIsUart(uint8_t type)
{
  return type == FLASH_CACHE_DIRECTION_RX ||
         type == FLASH_CACHE_DIRECTION_TX;
}

static uint16_t Cache_RingAdvance(uint16_t position, uint16_t length)
{
  return (uint16_t)((position + length) % CACHE_INPUT_SIZE);
}

static void Cache_RingCopyFrom(uint16_t position, uint8_t *data,
                               uint16_t length)
{
  while (length != 0U)
  {
    uint16_t chunk = (uint16_t)(CACHE_INPUT_SIZE - position);
    if (chunk > length)
    {
      chunk = length;
    }
    memcpy(data, &cache_input[position], chunk);
    position = Cache_RingAdvance(position, chunk);
    data += chunk;
    length = (uint16_t)(length - chunk);
  }
}

static void Cache_RingWrite(const uint8_t *data, uint16_t length)
{
  while (length != 0U)
  {
    uint16_t chunk = (uint16_t)(CACHE_INPUT_SIZE - cache_input_head);
    if (chunk > length)
    {
      chunk = length;
    }
    memcpy(&cache_input[cache_input_head], data, chunk);
    cache_input_head = Cache_RingAdvance(cache_input_head, chunk);
    data += chunk;
    length = (uint16_t)(length - chunk);
  }
}

static uint8_t Cache_RingPeekRecord(uint16_t position,
                                    CacheRecordHeader *header)
{
  Cache_RingCopyFrom(position, (uint8_t *)header, sizeof(*header));
  return header->length != 0U && header->length <= CACHE_RECORD_DATA_MAX &&
         header->direction <= FLASH_CACHE_RECORD_TIME_SYNC;
}

static uint8_t Cache_DropOldestInput(void)
{
  CacheRecordHeader header;

  if (cache_input_tail == cache_input_head ||
      Cache_RingPeekRecord(cache_input_tail, &header) == 0U)
  {
    cache_input_tail = cache_input_head;
    cache_input_records = 0U;
    cache_input_bytes = 0U;
    return 0U;
  }
  if (cache_dump_state != CACHE_DUMP_IDLE &&
      cache_dump_state != CACHE_DUMP_WAIT_IO &&
      cache_dump_ram_position != cache_dump_ram_end &&
      cache_input_tail == cache_dump_ram_position)
  {
    cache_dump_ram_overflow = 1U;
  }
  cache_input_tail = Cache_RingAdvance(
      cache_input_tail, (uint16_t)(sizeof(header) + header.length));
  cache_input_records--;
  if (Cache_RecordIsUart(header.direction) != 0U)
  {
    cache_input_bytes -= header.length;
    cache_dropped_bytes += header.length;
  }
  cache_dropped_records++;
  return 1U;
}

static void Cache_AddRecord(FlashCacheRecordType type,
                            const uint8_t *data, uint16_t length,
                            uint64_t timestamp_ms)
{
  CacheRecordHeader header;
  uint16_t stored_length = (uint16_t)(sizeof(header) + length);
  uint32_t pending_before = cache_input_records + cache_staged_records;

  while (Cache_RingFree() < stored_length)
  {
    if (Cache_DropOldestInput() == 0U)
    {
      cache_dropped_records++;
      if (Cache_RecordIsUart((uint8_t)type) != 0U)
      {
        cache_dropped_bytes += length;
      }
      return;
    }
  }

  header.timestamp_ms = timestamp_ms;
  header.length = length;
  header.direction = (uint8_t)type;
  header.reserved = 0U;
  Cache_RingWrite((const uint8_t *)&header, sizeof(header));
  Cache_RingWrite(data, length);
  cache_input_records++;
  if (Cache_RecordIsUart((uint8_t)type) != 0U)
  {
    cache_input_bytes += length;
  }
  else
  {
    cache_event_records++;
  }
  if (pending_before == 0U)
  {
    cache_pending_since_tick = HAL_GetTick();
  }
}

static uint8_t Cache_StageBlock(void)
{
  CacheExtentHeader extent_header = {0};
  CacheRecordHeader record_header;
  uint16_t original_length = 0U;
  uint16_t compressed_length;
  uint16_t records = 0U;
  uint32_t raw_bytes = 0U;
  uint16_t position = cache_input_tail;

  if (cache_staged_valid != 0U || cache_input_records == 0U)
  {
    return 0U;
  }

  while (position != cache_input_head)
  {
    uint16_t record_size;
    if (Cache_RingPeekRecord(position, &record_header) == 0U)
    {
      cache_input_tail = cache_input_head;
      cache_input_records = 0U;
      cache_input_bytes = 0U;
      break;
    }
    record_size = (uint16_t)(sizeof(record_header) + record_header.length);
    if ((uint32_t)original_length + record_size > CACHE_ORIGINAL_MAX)
    {
      break;
    }
    Cache_RingCopyFrom(position, &cache_original[original_length], record_size);
    position = Cache_RingAdvance(position, record_size);
    original_length = (uint16_t)(original_length + record_size);
    records++;
    if (Cache_RecordIsUart(record_header.direction) != 0U)
    {
      raw_bytes += record_header.length;
      if (raw_bytes >= CACHE_FLUSH_BYTES)
      {
        break;
      }
    }
  }

  if (records == 0U)
  {
    return 0U;
  }

  memset(cache_extent, 0xFF, sizeof(cache_extent));
  compressed_length = Cache_Lz4Compress(
      cache_original, original_length,
      &cache_extent[CACHE_EXTENT_HEADER_SIZE], CACHE_COMPRESSED_MAX);
  if (compressed_length == 0U || compressed_length > CACHE_COMPRESSED_MAX)
  {
    cache_write_errors++;
    cache_paused = 1U;
    return 0U;
  }

  extent_header.magic = CACHE_EXTENT_MAGIC;
  extent_header.epoch = cache_epoch;
  extent_header.sequence = cache_next_sequence;
  extent_header.session_id = cache_current_session;
  extent_header.raw_bytes = raw_bytes;
  extent_header.compressed_crc = Crc32_Calculate(
      &cache_extent[CACHE_EXTENT_HEADER_SIZE], compressed_length);
  extent_header.original_crc = Crc32_Calculate(cache_original, original_length);
  extent_header.compressed_length = compressed_length;
  extent_header.original_length = original_length;
  extent_header.record_count = records;
  extent_header.version = CACHE_VERSION;
  extent_header.extent_pages = (uint8_t)(
      (CACHE_EXTENT_HEADER_SIZE + compressed_length + GD25_PAGE_SIZE - 1U) /
      GD25_PAGE_SIZE);
  extent_header.codec = CACHE_CODEC_LZ4;
  extent_header.header_crc = Crc32_Calculate(
      &extent_header, offsetof(CacheExtentHeader, header_crc));
  memcpy(cache_extent, &extent_header, sizeof(extent_header));

  cache_input_tail = position;
  cache_input_records -= records;
  cache_input_bytes -= raw_bytes;
  cache_staged_valid = 1U;
  cache_staged_records = records;
  cache_staged_bytes = raw_bytes;
  cache_staged_original_length = original_length;
  cache_staged_compressed_length = compressed_length;
  cache_staged_pages = extent_header.extent_pages;
  return 1U;
}

static void Cache_CollectRemovedStats(uint32_t sector)
{
  CacheSectorHeader sector_header;
  CacheExtentHeader header;
  uint8_t page = 1U;

  cache_removed_blocks = 0U;
  cache_removed_records = 0U;
  cache_removed_bytes = 0U;
  cache_removed_original_bytes = 0U;
  cache_removed_compressed_bytes = 0U;
  if (Cache_ReadSectorHeader(sector, &sector_header) == 0U ||
      Cache_SectorHeaderValid(&sector_header) == 0U)
  {
    return;
  }
  while (page < CACHE_PAGES_PER_SECTOR)
  {
    uint32_t address = Cache_SectorAddress(sector) +
                       (uint32_t)page * GD25_PAGE_SIZE;
    if (Cache_ReadExtentHeader(address, &header) != 0U &&
        Cache_ExtentHeaderValid(&header) != 0U &&
        page + header.extent_pages <= CACHE_PAGES_PER_SECTOR)
    {
      cache_removed_blocks++;
      cache_removed_records += header.record_count;
      cache_removed_bytes += header.raw_bytes;
      cache_removed_original_bytes += header.original_length;
      cache_removed_compressed_bytes += header.compressed_length;
      page = (uint8_t)(page + header.extent_pages);
    }
    else
    {
      page++;
    }
  }
}

static void Cache_ApplyRemovedStats(uint32_t sector)
{
  if (cache_removed_blocks > cache_committed_blocks)
  {
    cache_committed_blocks = 0U;
  }
  else
  {
    cache_committed_blocks -= cache_removed_blocks;
  }
  if (cache_removed_records > cache_committed_records)
  {
    cache_committed_records = 0U;
  }
  else
  {
    cache_committed_records -= cache_removed_records;
  }
  if (cache_removed_bytes > cache_committed_bytes)
  {
    cache_committed_bytes = 0U;
  }
  else
  {
    cache_committed_bytes -= cache_removed_bytes;
  }
  if (cache_removed_original_bytes > cache_committed_original_bytes)
  {
    cache_committed_original_bytes = 0U;
  }
  else
  {
    cache_committed_original_bytes -= cache_removed_original_bytes;
  }
  if (cache_removed_compressed_bytes > cache_committed_compressed_bytes)
  {
    cache_committed_compressed_bytes = 0U;
  }
  else
  {
    cache_committed_compressed_bytes -= cache_removed_compressed_bytes;
  }
  if (cache_removed_blocks != 0U)
  {
    cache_overwritten_blocks += cache_removed_blocks;
    if (cache_oldest_sector == sector)
    {
      cache_oldest_sector = (sector + 1U) % CACHE_DATA_SECTOR_COUNT;
    }
  }
  cache_removed_blocks = 0U;
  cache_removed_records = 0U;
  cache_removed_bytes = 0U;
  cache_removed_original_bytes = 0U;
  cache_removed_compressed_bytes = 0U;
}

static void Cache_WriteFailed(void)
{
  cache_io_state = CACHE_IO_IDLE;
  cache_write_errors++;
  cache_write_attempts++;
  cache_active_offset = GD25_SECTOR_SIZE;
  if (cache_write_attempts >= CACHE_WRITE_RETRIES)
  {
    cache_paused = 1U;
  }
}

static uint8_t Cache_StartProgramPage(void)
{
  uint32_t address = cache_target_address +
                     (uint32_t)cache_program_page * GD25_PAGE_SIZE;

  if (Flash_StartPageProgram(address,
                             &cache_extent[(uint16_t)cache_program_page *
                                           GD25_PAGE_SIZE],
                             GD25_PAGE_SIZE) == 0U)
  {
    return 0U;
  }
  cache_io_state = CACHE_IO_PROGRAM_WAIT;
  return 1U;
}

static uint8_t Cache_StartExtentProgram(void)
{
  cache_program_page = cache_staged_pages > 1U ? 1U : 0U;
  return Cache_StartProgramPage();
}

static uint8_t Cache_StartStagedWrite(void)
{
  uint16_t extent_size = (uint16_t)(cache_staged_pages * GD25_PAGE_SIZE);

  if ((uint32_t)cache_active_offset + extent_size > GD25_SECTOR_SIZE)
  {
    if (cache_spare_ready == 0U)
    {
      return 0U;
    }
    cache_active_sector = cache_spare_sector;
    cache_active_offset = CACHE_SECTOR_DATA_OFFSET;
    cache_spare_sector =
        (cache_active_sector + 1U) % CACHE_DATA_SECTOR_COUNT;
    cache_spare_ready = 0U;
    cache_target_address = Cache_SectorAddress(cache_active_sector) +
                           cache_active_offset;
    Cache_PrepareSectorHeader(cache_next_generation++, cache_next_sequence);
    if (Flash_StartPageProgram(Cache_SectorAddress(cache_active_sector),
                               cache_sector_header_page,
                               sizeof(cache_sector_header_page)) == 0U)
    {
      return 0U;
    }
    cache_io_state = CACHE_IO_SECTOR_HEADER_WAIT;
    return 1U;
  }

  cache_target_address = Cache_SectorAddress(cache_active_sector) +
                         cache_active_offset;
  return Cache_StartExtentProgram();
}

static uint8_t Cache_VerifyStagedBlock(void)
{
  CacheExtentHeader header;

  return Cache_ReadValidExtent(cache_target_address, &header,
                               cache_dump_compressed,
                               cache_dump_payload) != 0U &&
         header.sequence == cache_next_sequence &&
         header.session_id == cache_current_session;
}

static void Cache_WriteCompleted(void)
{
  if (cache_committed_blocks == 0U)
  {
    cache_oldest_sector = cache_active_sector;
  }
  cache_committed_blocks++;
  cache_committed_records += cache_staged_records;
  cache_committed_bytes += cache_staged_bytes;
  cache_committed_original_bytes += cache_staged_original_length;
  cache_committed_compressed_bytes += cache_staged_compressed_length;
  cache_write_blocks++;
  cache_next_sequence++;
  if (cache_next_sequence == 0U)
  {
    cache_next_sequence = 1U;
  }
  cache_active_offset = (uint16_t)(cache_active_offset +
                                   cache_staged_pages * GD25_PAGE_SIZE);
  cache_staged_valid = 0U;
  cache_staged_records = 0U;
  cache_staged_bytes = 0U;
  cache_staged_original_length = 0U;
  cache_staged_compressed_length = 0U;
  cache_staged_pages = 0U;
  cache_staged_original_length = 0U;
  cache_staged_compressed_length = 0U;
  cache_staged_pages = 0U;
  cache_write_attempts = 0U;
  cache_io_state = CACHE_IO_IDLE;

  if (cache_input_records == 0U)
  {
    cache_pending_since_tick = 0U;
    cache_flush_all = 0U;
  }
  else
  {
    cache_pending_since_tick = HAL_GetTick();
  }
}

static void Cache_FormatUint64(uint64_t value, char *text)
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

static uint8_t Cache_IsLeapYear(uint16_t year)
{
  return ((year % 4U) == 0U &&
          ((year % 100U) != 0U || (year % 400U) == 0U))
             ? 1U
             : 0U;
}

static uint8_t Cache_DaysInMonth(uint16_t year, uint8_t month)
{
  static const uint8_t days[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                 31U, 31U, 30U, 31U, 30U, 31U};

  if (month == 0U || month > 12U)
  {
    return 0U;
  }
  return month == 2U && Cache_IsLeapYear(year) != 0U
             ? 29U
             : days[month - 1U];
}

static uint8_t Cache_ParseDigits(const char *text, uint8_t count,
                                 uint32_t *value)
{
  uint32_t result = 0U;

  for (uint8_t index = 0U; index < count; index++)
  {
    if (text[index] < '0' || text[index] > '9')
    {
      return 0U;
    }
    result = result * 10U + (uint32_t)(text[index] - '0');
  }
  *value = result;
  return 1U;
}

static uint8_t Cache_ParseUtcOffset(const char *text, int16_t *minutes)
{
  uint32_t hours;
  uint32_t mins;
  int16_t value;

  if (text == NULL || strlen(text) != 6U ||
      (text[0] != '+' && text[0] != '-') || text[3] != ':' ||
      Cache_ParseDigits(&text[1], 2U, &hours) == 0U ||
      Cache_ParseDigits(&text[4], 2U, &mins) == 0U || hours > 14U ||
      mins > 59U || (hours == 14U && mins != 0U))
  {
    return 0U;
  }
  value = (int16_t)(hours * 60U + mins);
  *minutes = text[0] == '-' ? (int16_t)-value : value;
  return 1U;
}

static uint8_t Cache_DateToUnixMs(uint16_t year, uint8_t month, uint8_t day,
                                  uint8_t hour, uint8_t minute,
                                  uint8_t second, uint16_t millisecond,
                                  int16_t utc_offset, uint64_t *unix_ms)
{
  uint32_t days = 0U;
  uint64_t local_ms;
  int64_t utc_ms;

  if (year < 1970U || year > 2099U || day == 0U ||
      day > Cache_DaysInMonth(year, month) || hour > 23U || minute > 59U ||
      second > 59U || millisecond > 999U)
  {
    return 0U;
  }
  for (uint16_t current = 1970U; current < year; current++)
  {
    days += Cache_IsLeapYear(current) != 0U ? 366U : 365U;
  }
  for (uint8_t current = 1U; current < month; current++)
  {
    days += Cache_DaysInMonth(year, current);
  }
  days += day - 1U;
  local_ms = ((uint64_t)days * 86400ULL + (uint64_t)hour * 3600ULL +
              (uint64_t)minute * 60ULL + second) *
                 1000ULL +
             millisecond;
  utc_ms = (int64_t)local_ms - (int64_t)utc_offset * 60000LL;
  if (utc_ms < 0)
  {
    return 0U;
  }
  *unix_ms = (uint64_t)utc_ms;
  return 1U;
}

static uint8_t Cache_FormatIso(uint64_t unix_ms, int16_t utc_offset,
                               char *text, size_t size)
{
  int64_t local_ms = (int64_t)unix_ms + (int64_t)utc_offset * 60000LL;
  uint32_t days;
  uint32_t day_ms;
  uint16_t year = 1970U;
  uint8_t month = 1U;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t millisecond;
  uint16_t offset_value;
  char sign;

  if (local_ms < 0)
  {
    return 0U;
  }
  days = (uint32_t)((uint64_t)local_ms / 86400000ULL);
  day_ms = (uint32_t)((uint64_t)local_ms % 86400000ULL);
  while (year <= 2099U)
  {
    uint16_t year_days = Cache_IsLeapYear(year) != 0U ? 366U : 365U;
    if (days < year_days)
    {
      break;
    }
    days -= year_days;
    year++;
  }
  if (year > 2099U)
  {
    return 0U;
  }
  while (month <= 12U)
  {
    uint8_t month_days = Cache_DaysInMonth(year, month);
    if (days < month_days)
    {
      break;
    }
    days -= month_days;
    month++;
  }
  day = (uint8_t)(days + 1U);
  hour = (uint8_t)(day_ms / 3600000UL);
  day_ms %= 3600000UL;
  minute = (uint8_t)(day_ms / 60000UL);
  day_ms %= 60000UL;
  second = (uint8_t)(day_ms / 1000UL);
  millisecond = (uint16_t)(day_ms % 1000UL);
  sign = utc_offset < 0 ? '-' : '+';
  offset_value = (uint16_t)(utc_offset < 0 ? -utc_offset : utc_offset);
  return snprintf(text, size,
                  "%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02u:%02u",
                  (unsigned int)year, (unsigned int)month,
                  (unsigned int)day, (unsigned int)hour,
                  (unsigned int)minute, (unsigned int)second,
                  (unsigned int)millisecond, sign,
                  (unsigned int)(offset_value / 60U),
                  (unsigned int)(offset_value % 60U)) > 0
             ? 1U
             : 0U;
}

static uint8_t Cache_DumpAppend(const void *data, uint16_t length)
{
  if ((uint32_t)cache_dump_output_length + length > CACHE_DUMP_OUTPUT_SIZE)
  {
    return 0U;
  }
  memcpy(&cache_dump_output[cache_dump_output_length], data, length);
  cache_dump_output_length = (uint16_t)(cache_dump_output_length + length);
  return 1U;
}

static uint8_t Cache_DumpAppendText(const char *text)
{
  return Cache_DumpAppend(text, (uint16_t)strlen(text));
}

static uint8_t Cache_DumpAppendPrefix(uint32_t session_id,
                                      uint64_t uptime_ms)
{
  char text[128];
  char uptime[21];

  Cache_FormatUint64(uptime_ms, uptime);
  if (cache_dump_time_valid != 0U &&
      cache_dump_time_session == session_id &&
      uptime_ms >= cache_dump_time_uptime_ms)
  {
    char iso[40];
    uint64_t unix_ms = cache_dump_time_unix_ms +
                       (uptime_ms - cache_dump_time_uptime_ms);
    if (Cache_FormatIso(unix_ms, cache_dump_time_utc_offset,
                        iso, sizeof(iso)) != 0U)
    {
      (void)snprintf(text, sizeof(text), "[S%08lu %s +%s ms]",
                     (unsigned long)session_id, iso, uptime);
      return Cache_DumpAppendText(text);
    }
  }
  (void)snprintf(text, sizeof(text), "[S%08lu UNSYNC +%s ms]",
                 (unsigned long)session_id, uptime);
  return Cache_DumpAppendText(text);
}

static uint8_t Cache_DumpAppendEscapedText(const uint8_t *data,
                                           uint16_t length)
{
  uint16_t index = 0U;
  char escaped[16];

  while (index < length)
  {
    uint8_t byte = data[index];

    if (byte == '\r' && index + 1U < length && data[index + 1U] == '\n')
    {
      if (Cache_DumpAppendText("\r\n") == 0U)
      {
        return 0U;
      }
      index = (uint16_t)(index + 2U);
      continue;
    }
    if (byte == '\n')
    {
      if (Cache_DumpAppendText("\r\n") == 0U)
      {
        return 0U;
      }
      index++;
      continue;
    }
    if (byte == '\b')
    {
      uint16_t run = 1U;
      while (index + run < length && data[index + run] == '\b')
      {
        run++;
      }
      if (run >= 4U)
      {
        (void)snprintf(escaped, sizeof(escaped), "<BS x%u>",
                       (unsigned int)run);
        if (Cache_DumpAppendText(escaped) == 0U)
        {
          return 0U;
        }
      }
      else
      {
        for (uint16_t count = 0U; count < run; count++)
        {
          if (Cache_DumpAppendText("<BS>") == 0U)
          {
            return 0U;
          }
        }
      }
      index = (uint16_t)(index + run);
      continue;
    }
    if (byte >= 0x20U && byte <= 0x7EU)
    {
      if (Cache_DumpAppend(&data[index], 1U) == 0U)
      {
        return 0U;
      }
    }
    else
    {
      const char *token = NULL;
      switch (byte)
      {
        case 0x00U: token = "<NUL>"; break;
        case '\r': token = "<CR>"; break;
        case '\t': token = "<TAB>"; break;
        case 0x1BU: token = "<ESC>"; break;
        case 0x7FU: token = "<DEL>"; break;
        default:
          (void)snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
          token = escaped;
          break;
      }
      if (Cache_DumpAppendText(token) == 0U)
      {
        return 0U;
      }
    }
    index++;
  }
  return Cache_DumpAppendText("\r\n\r\n");
}

static const char *Cache_ControlName(uint8_t event)
{
  switch (event)
  {
    case FLASH_CACHE_CONTROL_RELAY: return "relay";
    case FLASH_CACHE_CONTROL_SOC_POWER: return "soc power";
    case FLASH_CACHE_CONTROL_RESET_ASSERT: return "reset assert";
    case FLASH_CACHE_CONTROL_RESET_RELEASE: return "reset release";
    case FLASH_CACHE_CONTROL_RESET_PULSE: return "reset pulse";
    case FLASH_CACHE_CONTROL_LOADER: return "loader";
    case FLASH_CACHE_CONTROL_MASKROM: return "maskrom";
    default: return "unknown";
  }
}

static uint8_t Cache_DumpPrepareRecord(uint32_t session_id,
                                       const CacheRecordHeader *record,
                                       const uint8_t *data)
{
  uint8_t is_uart = Cache_RecordIsUart(record->direction);
  uint16_t data_offset = 0U;
  uint16_t length = record->length;

  cache_dump_output_length = 0U;
  cache_dump_output_offset = 0U;
  if (cache_dump_time_session != session_id)
  {
    cache_dump_time_session = session_id;
    cache_dump_time_valid = 0U;
  }

  if (record->direction == FLASH_CACHE_RECORD_TIME_SYNC &&
      record->length == sizeof(CacheTimePayload))
  {
    CacheTimePayload payload;
    memcpy(&payload, data, sizeof(payload));
    cache_dump_time_valid = payload.valid;
    cache_dump_time_unix_ms = payload.unix_ms;
    cache_dump_time_uptime_ms = record->timestamp_ms;
    cache_dump_time_utc_offset = payload.utc_offset_minutes;
  }

  if (is_uart != 0U)
  {
    if (cache_dump_skip_bytes >= length)
    {
      cache_dump_skip_bytes -= length;
      return 1U;
    }
    if (cache_dump_skip_bytes != 0U)
    {
      data_offset = (uint16_t)cache_dump_skip_bytes;
      cache_dump_skip_bytes = 0U;
    }
    length = (uint16_t)(length - data_offset);
    cache_dump_output_bytes += length;
  }
  else if (cache_dump_skip_bytes != 0U ||
           cache_dump_format == FLASH_CACHE_DUMP_FORMAT_RAW)
  {
    return 1U;
  }

  cache_dump_had_records = 1U;
  if (cache_dump_format == FLASH_CACHE_DUMP_FORMAT_RAW)
  {
    return Cache_DumpAppend(&data[data_offset], length);
  }

  if (Cache_DumpAppendPrefix(session_id, record->timestamp_ms) == 0U)
  {
    return 0U;
  }
  if (is_uart != 0U)
  {
    char line[80];
    const char *direction =
        record->direction == FLASH_CACHE_DIRECTION_RX
            ? "RX UART2->DBG"
            : "TX DBG->UART2";
    (void)snprintf(line, sizeof(line), "[%s][len=%u]\r\n", direction,
                   (unsigned int)length);
    if (Cache_DumpAppendText(line) == 0U)
    {
      return 0U;
    }
    return Cache_DumpAppendEscapedText(&data[data_offset], length);
  }

  if (record->direction == FLASH_CACHE_RECORD_CONTROL &&
      record->length == sizeof(CacheControlPayload))
  {
    CacheControlPayload payload;
    char line[128];
    const char *source;
    memcpy(&payload, data, sizeof(payload));
    source = payload.source == FLASH_CACHE_SOURCE_SW3 ? "SW3" : "CLI";
    if (payload.event == FLASH_CACHE_CONTROL_RESET_PULSE)
    {
      (void)snprintf(line, sizeof(line),
                     "[EVENT %s][reset pulse=completed duration=%lu ms]\r\n\r\n",
                     source, (unsigned long)payload.argument);
    }
    else if (payload.event == FLASH_CACHE_CONTROL_RESET_ASSERT ||
             payload.event == FLASH_CACHE_CONTROL_RESET_RELEASE)
    {
      (void)snprintf(line, sizeof(line), "[EVENT %s][reset=%s]\r\n\r\n",
                     source,
                     payload.event == FLASH_CACHE_CONTROL_RESET_ASSERT
                         ? "asserted"
                         : "released");
    }
    else
    {
      (void)snprintf(line, sizeof(line), "[EVENT %s][%s=%s]\r\n\r\n",
                     source, Cache_ControlName(payload.event),
                     payload.value != 0U ? "on" : "off");
    }
    return Cache_DumpAppendText(line);
  }

  if (record->direction == FLASH_CACHE_RECORD_TIME_SYNC &&
      record->length == sizeof(CacheTimePayload))
  {
    CacheTimePayload payload;
    char line[96];
    memcpy(&payload, data, sizeof(payload));
    if (payload.valid != 0U)
    {
      char iso[40];
      if (Cache_FormatIso(payload.unix_ms, payload.utc_offset_minutes,
                          iso, sizeof(iso)) == 0U)
      {
        return 0U;
      }
      (void)snprintf(line, sizeof(line), "[TIME][synchronized=%s]\r\n\r\n",
                     iso);
    }
    else
    {
      (void)snprintf(line, sizeof(line), "[TIME][cleared]\r\n\r\n");
    }
    return Cache_DumpAppendText(line);
  }
  return 0U;
}

static uint8_t Cache_DumpProcessPayloadRecord(void)
{
  CacheRecordHeader record;

  if (cache_dump_payload_offset >= cache_dump_payload_length)
  {
    return 1U;
  }
  if ((uint32_t)cache_dump_payload_offset + sizeof(record) >
      cache_dump_payload_length)
  {
    return 0U;
  }
  memcpy(&record, &cache_dump_payload[cache_dump_payload_offset],
         sizeof(record));
  cache_dump_payload_offset =
      (uint16_t)(cache_dump_payload_offset + sizeof(record));
  if (record.length == 0U || record.length > CACHE_RECORD_DATA_MAX ||
      record.direction > FLASH_CACHE_RECORD_TIME_SYNC ||
      (uint32_t)cache_dump_payload_offset + record.length >
          cache_dump_payload_length)
  {
    return 0U;
  }
  if (Cache_DumpPrepareRecord(
          cache_dump_payload_session, &record,
          &cache_dump_payload[cache_dump_payload_offset]) == 0U)
  {
    return 0U;
  }
  cache_dump_payload_offset =
      (uint16_t)(cache_dump_payload_offset + record.length);
  return 1U;
}

void FlashCache_Init(void)
{
  CacheMeta meta = {0};
  CacheSectorHeader sector_header;
  CacheExtentHeader extent_header;
  uint32_t minimum_sequence = 0xFFFFFFFFUL;
  uint32_t latest_sequence = 0U;
  uint32_t latest_session = 0U;
  uint32_t latest_generation = 0U;
  uint32_t latest_sector = 0U;
  uint16_t latest_offset = CACHE_SECTOR_DATA_OFFSET;
  uint8_t have_layout = 0U;
  uint8_t meta_valid = 0U;

  cache_present = 0U;
  cache_input_head = 0U;
  cache_input_tail = 0U;
  cache_input_records = 0U;
  cache_input_bytes = 0U;
  cache_pending_since_tick = 0U;
  cache_staged_valid = 0U;
  cache_staged_records = 0U;
  cache_staged_bytes = 0U;
  cache_staged_original_length = 0U;
  cache_staged_compressed_length = 0U;
  cache_staged_pages = 0U;
  cache_flush_all = 0U;
  cache_io_state = CACHE_IO_IDLE;
  cache_write_attempts = 0U;
  cache_paused = 0U;
  cache_committed_blocks = 0U;
  cache_committed_records = 0U;
  cache_committed_bytes = 0U;
  cache_committed_original_bytes = 0U;
  cache_committed_compressed_bytes = 0U;
  cache_overwritten_blocks = 0U;
  cache_rx_bytes = 0U;
  cache_tx_bytes = 0U;
  cache_dropped_bytes = 0U;
  cache_dropped_records = 0U;
  cache_write_blocks = 0U;
  cache_erase_sectors = 0U;
  cache_write_errors = 0U;
  cache_event_records = 0U;
  cache_time_valid = 0U;
  cache_time_unix_ms = 0U;
  cache_time_uptime_ms = 0U;
  cache_time_utc_offset = 0;
  cache_dump_state = CACHE_DUMP_IDLE;
  cache_dump_writer = NULL;
  cache_dump_format = FLASH_CACHE_DUMP_FORMAT_RAW;
  cache_dump_max_bytes = 0U;
  cache_dump_snapshot_bytes = 0U;
  cache_dump_output_bytes = 0U;
  cache_dump_output_length = 0U;
  cache_dump_output_offset = 0U;
  cache_dump_ram_overflow = 0U;
  cache_dump_result = FLASH_CACHE_DUMP_NONE;
  cache_dump_cancel_count = 0U;
  cache_dump_overflow_count = 0U;
  cache_epoch = 1U;
  cache_next_sequence = 1U;
  cache_current_session = 1U;
  cache_oldest_sector = 0U;
  cache_active_sector = 0U;
  cache_active_offset = CACHE_SECTOR_DATA_OFFSET;
  cache_spare_sector = 1U;
  cache_spare_ready = 0U;
  cache_next_generation = 1U;

  (void)Flash_Command(FLASH_CMD_RELEASE_POWER_DOWN);
  HAL_Delay(1U);
  (void)Flash_Command(FLASH_CMD_RESET_ENABLE);
  (void)Flash_Command(FLASH_CMD_RESET);
  HAL_Delay(1U);

  if (Flash_ReadJedec(cache_jedec) == 0U ||
      (cache_jedec[0] == 0x00U && cache_jedec[1] == 0x00U &&
       cache_jedec[2] == 0x00U) ||
      (cache_jedec[0] == 0xFFU && cache_jedec[1] == 0xFFU &&
       cache_jedec[2] == 0xFFU))
  {
    return;
  }
  cache_present = 1U;

  if (Flash_Read(CACHE_META_ADDRESS, (uint8_t *)&meta, sizeof(meta)) != 0U &&
      Cache_MetaValid(&meta) != 0U)
  {
    meta_valid = 1U;
    cache_epoch = meta.epoch;
  }
  if (meta_valid == 0U && Cache_WriteMeta() == 0U)
  {
    cache_write_errors++;
  }

  if (meta_valid != 0U)
  {
    for (uint32_t sector = 0U; sector < CACHE_DATA_SECTOR_COUNT; sector++)
    {
      uint8_t page = 1U;
      uint16_t sector_end = CACHE_SECTOR_DATA_OFFSET;
      if (Cache_ReadSectorHeader(sector, &sector_header) == 0U ||
          Cache_SectorHeaderValid(&sector_header) == 0U)
      {
        continue;
      }
      have_layout = 1U;
      while (page < CACHE_PAGES_PER_SECTOR)
      {
        uint32_t address = Cache_SectorAddress(sector) +
                           (uint32_t)page * GD25_PAGE_SIZE;
        if (Cache_ReadValidExtent(address, &extent_header,
                                  cache_dump_compressed,
                                  cache_dump_payload) != 0U &&
            page + extent_header.extent_pages <= CACHE_PAGES_PER_SECTOR)
        {
          cache_committed_blocks++;
          cache_committed_records += extent_header.record_count;
          cache_committed_bytes += extent_header.raw_bytes;
          cache_committed_original_bytes += extent_header.original_length;
          cache_committed_compressed_bytes += extent_header.compressed_length;
          if (extent_header.sequence >= latest_sequence)
          {
            latest_sequence = extent_header.sequence;
          }
          if (extent_header.sequence < minimum_sequence)
          {
            minimum_sequence = extent_header.sequence;
            cache_oldest_sector = sector;
          }
          if (extent_header.session_id > latest_session)
          {
            latest_session = extent_header.session_id;
          }
          page = (uint8_t)(page + extent_header.extent_pages);
          sector_end = (uint16_t)(page * GD25_PAGE_SIZE);
        }
        else
        {
          page++;
        }
      }
      if (sector_header.generation >= latest_generation)
      {
        latest_generation = sector_header.generation;
        latest_sector = sector;
        latest_offset = sector_end;
      }
    }
  }

  if (have_layout == 0U)
  {
    if (Flash_EraseSectorBlocking(Cache_SectorAddress(0U)) == 0U ||
        Flash_EraseSectorBlocking(Cache_SectorAddress(1U)) == 0U ||
        Cache_WriteSectorHeaderBlocking(0U, 1U, 1U) == 0U)
    {
      cache_write_errors++;
      cache_paused = 1U;
      return;
    }
    cache_erase_sectors += 2U;
    cache_active_sector = 0U;
    cache_active_offset = CACHE_SECTOR_DATA_OFFSET;
    cache_spare_sector = 1U;
    cache_spare_ready = 1U;
    cache_next_generation = 2U;
  }
  else
  {
    cache_active_sector = latest_sector;
    cache_active_offset = latest_offset;
    if (cache_active_offset < GD25_SECTOR_SIZE &&
        Cache_AreaErased(Cache_SectorAddress(cache_active_sector) +
                             cache_active_offset,
                         GD25_SECTOR_SIZE - cache_active_offset) == 0U)
    {
      cache_active_offset = GD25_SECTOR_SIZE;
    }
    cache_spare_sector =
        (cache_active_sector + 1U) % CACHE_DATA_SECTOR_COUNT;
    cache_next_generation = latest_generation + 1U;
    if (cache_next_generation == 0U)
    {
      cache_next_generation = 1U;
    }
    if (Cache_SectorErased(cache_spare_sector) == 0U)
    {
      Cache_CollectRemovedStats(cache_spare_sector);
      if (Flash_EraseSectorBlocking(Cache_SectorAddress(cache_spare_sector)) ==
          0U)
      {
        cache_write_errors++;
        cache_paused = 1U;
        return;
      }
      Cache_ApplyRemovedStats(cache_spare_sector);
      cache_erase_sectors++;
    }
    cache_spare_ready = 1U;
  }

  cache_current_session = latest_session + 1U;
  if (cache_current_session == 0U)
  {
    cache_current_session = 1U;
  }
  if (cache_committed_blocks != 0U)
  {
    cache_next_sequence = latest_sequence + 1U;
    if (cache_next_sequence == 0U)
    {
      cache_next_sequence = 1U;
    }
    cache_overwritten_blocks =
        latest_sequence > cache_committed_blocks
            ? latest_sequence - cache_committed_blocks
            : 0U;
  }
}

uint8_t FlashCache_TimeSetIso(const char *text, uint64_t uptime_ms)
{
  uint32_t year;
  uint32_t month;
  uint32_t day;
  uint32_t hour;
  uint32_t minute;
  uint32_t second;
  uint32_t millisecond;
  int16_t offset;
  uint64_t unix_ms;

  if (text == NULL || strlen(text) != 29U || text[4] != '-' ||
      text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text[19] != '.' ||
      Cache_ParseDigits(&text[0], 4U, &year) == 0U ||
      Cache_ParseDigits(&text[5], 2U, &month) == 0U ||
      Cache_ParseDigits(&text[8], 2U, &day) == 0U ||
      Cache_ParseDigits(&text[11], 2U, &hour) == 0U ||
      Cache_ParseDigits(&text[14], 2U, &minute) == 0U ||
      Cache_ParseDigits(&text[17], 2U, &second) == 0U ||
      Cache_ParseDigits(&text[20], 3U, &millisecond) == 0U ||
      Cache_ParseUtcOffset(&text[23], &offset) == 0U ||
      Cache_DateToUnixMs((uint16_t)year, (uint8_t)month, (uint8_t)day,
                         (uint8_t)hour, (uint8_t)minute, (uint8_t)second,
                         (uint16_t)millisecond, offset, &unix_ms) == 0U)
  {
    return 0U;
  }
  return FlashCache_TimeSetUnix(unix_ms, offset, uptime_ms);
}

uint8_t FlashCache_TimeSetUnix(uint64_t unix_ms, int16_t utc_offset_minutes,
                               uint64_t uptime_ms)
{
  CacheTimePayload payload = {0};
  char iso[40];

  if (utc_offset_minutes < -840 || utc_offset_minutes > 840 ||
      Cache_FormatIso(unix_ms, utc_offset_minutes, iso, sizeof(iso)) == 0U)
  {
    return 0U;
  }
  cache_time_valid = 1U;
  cache_time_unix_ms = unix_ms;
  cache_time_uptime_ms = uptime_ms;
  cache_time_utc_offset = utc_offset_minutes;
  payload.unix_ms = unix_ms;
  payload.utc_offset_minutes = utc_offset_minutes;
  payload.valid = 1U;
  if (cache_present != 0U)
  {
    Cache_AddRecord(FLASH_CACHE_RECORD_TIME_SYNC,
                    (const uint8_t *)&payload, sizeof(payload), uptime_ms);
  }
  return 1U;
}

void FlashCache_TimeClear(uint64_t uptime_ms)
{
  CacheTimePayload payload = {0};

  cache_time_valid = 0U;
  cache_time_unix_ms = 0U;
  cache_time_uptime_ms = uptime_ms;
  cache_time_utc_offset = 0;
  if (cache_present != 0U)
  {
    Cache_AddRecord(FLASH_CACHE_RECORD_TIME_SYNC,
                    (const uint8_t *)&payload, sizeof(payload), uptime_ms);
  }
}

void FlashCache_TimeGet(uint64_t uptime_ms, FlashCacheTimeStatus *status)
{
  if (status == NULL)
  {
    return;
  }
  status->valid = cache_time_valid;
  status->utc_offset_minutes = cache_time_utc_offset;
  status->unix_ms =
      cache_time_valid != 0U && uptime_ms >= cache_time_uptime_ms
          ? cache_time_unix_ms + (uptime_ms - cache_time_uptime_ms)
          : 0U;
}

uint8_t FlashCache_TimeFormat(uint64_t unix_ms, int16_t utc_offset_minutes,
                              char *text, uint16_t size)
{
  return text != NULL && size != 0U
             ? Cache_FormatIso(unix_ms, utc_offset_minutes, text, size)
             : 0U;
}

void FlashCache_PushControl(FlashCacheControlEvent event,
                            FlashCacheEventSource source, uint8_t value,
                            uint32_t argument, uint64_t timestamp_ms)
{
  CacheControlPayload payload = {0};

  if (cache_present == 0U || event < FLASH_CACHE_CONTROL_RELAY ||
      event > FLASH_CACHE_CONTROL_MASKROM ||
      source > FLASH_CACHE_SOURCE_SW3)
  {
    return;
  }
  payload.event = (uint8_t)event;
  payload.source = (uint8_t)source;
  payload.value = value != 0U ? 1U : 0U;
  payload.argument = argument;
  Cache_AddRecord(FLASH_CACHE_RECORD_CONTROL,
                  (const uint8_t *)&payload, sizeof(payload), timestamp_ms);
}

void FlashCache_Push(FlashCacheRecordType type, const uint8_t *data,
                     uint16_t length, uint64_t timestamp_ms)
{
  if (cache_present == 0U || data == NULL ||
      type > FLASH_CACHE_RECORD_TIME_SYNC)
  {
    return;
  }

  if (type == FLASH_CACHE_DIRECTION_RX)
  {
    cache_rx_bytes += length;
  }
  else if (type == FLASH_CACHE_DIRECTION_TX)
  {
    cache_tx_bytes += length;
  }

  while (length != 0U)
  {
    uint16_t chunk = length > CACHE_RECORD_DATA_MAX ? CACHE_RECORD_DATA_MAX
                                                    : length;
    Cache_AddRecord(type, data, chunk, timestamp_ms);
    data += chunk;
    length = (uint16_t)(length - chunk);
  }
}

void FlashCache_Task(void)
{
  uint8_t ready;

  if (cache_present == 0U)
  {
    return;
  }

  if (cache_io_state != CACHE_IO_IDLE)
  {
    if (Flash_IsReady(&ready) == 0U)
    {
      Cache_WriteFailed();
      return;
    }
    if (ready == 0U)
    {
      return;
    }

    if (cache_io_state == CACHE_IO_SPARE_ERASE_WAIT)
    {
      Cache_ApplyRemovedStats(cache_spare_sector);
      cache_erase_sectors++;
      cache_spare_ready = 1U;
      cache_io_state = CACHE_IO_IDLE;
      return;
    }

    if (cache_io_state == CACHE_IO_SECTOR_HEADER_WAIT)
    {
      if (Cache_StartExtentProgram() == 0U)
      {
        Cache_WriteFailed();
      }
      return;
    }

    if (cache_program_page != 0U)
    {
      if (cache_program_page + 1U < cache_staged_pages)
      {
        cache_program_page++;
      }
      else
      {
        cache_program_page = 0U;
      }
      if (Cache_StartProgramPage() == 0U)
      {
        Cache_WriteFailed();
      }
      return;
    }

    if (Cache_VerifyStagedBlock() != 0U)
    {
      Cache_WriteCompleted();
    }
    else
    {
      Cache_WriteFailed();
    }
    return;
  }

  if (cache_dump_state != CACHE_DUMP_IDLE)
  {
    return;
  }

  if (cache_paused != 0U)
  {
    return;
  }

  if (cache_spare_ready == 0U)
  {
    if (Cache_SectorErased(cache_spare_sector) != 0U)
    {
      cache_spare_ready = 1U;
      return;
    }
    Cache_CollectRemovedStats(cache_spare_sector);
    if (Flash_StartSectorErase(Cache_SectorAddress(cache_spare_sector)) == 0U)
    {
      Cache_WriteFailed();
      return;
    }
    cache_io_state = CACHE_IO_SPARE_ERASE_WAIT;
    return;
  }

  if (cache_staged_valid == 0U)
  {
    uint8_t threshold = cache_input_bytes >= CACHE_FLUSH_BYTES ? 1U : 0U;
    uint8_t timeout =
        cache_input_records != 0U &&
                (HAL_GetTick() - cache_pending_since_tick) >=
                    CACHE_FLUSH_INTERVAL_MS
            ? 1U
            : 0U;
    if (threshold == 0U && timeout == 0U && cache_flush_all == 0U)
    {
      return;
    }
    if (Cache_StageBlock() == 0U)
    {
      cache_flush_all = 0U;
      return;
    }
  }

  if (Cache_StartStagedWrite() == 0U)
  {
    Cache_WriteFailed();
  }
}

uint8_t FlashCache_RequestFlush(void)
{
  if (cache_present == 0U || cache_dump_state != CACHE_DUMP_IDLE ||
      (cache_input_records == 0U && cache_staged_valid == 0U))
  {
    return 0U;
  }
  cache_flush_all = 1U;
  cache_paused = 0U;
  cache_write_attempts = 0U;
  return 1U;
}

uint8_t FlashCache_Clear(void)
{
  uint8_t result;
  uint64_t now;
  uint64_t current_unix = 0U;

  if (cache_present == 0U || cache_io_state != CACHE_IO_IDLE ||
      cache_dump_state != CACHE_DUMP_IDLE)
  {
    return 0U;
  }

  now = App_GetUptimeMilliseconds();
  if (cache_time_valid != 0U && now >= cache_time_uptime_ms)
  {
    current_unix = cache_time_unix_ms + (now - cache_time_uptime_ms);
  }

  cache_epoch++;
  if (cache_epoch == 0U)
  {
    cache_epoch = 1U;
  }
  cache_input_head = 0U;
  cache_input_tail = 0U;
  cache_input_records = 0U;
  cache_input_bytes = 0U;
  cache_pending_since_tick = 0U;
  cache_staged_valid = 0U;
  cache_staged_records = 0U;
  cache_staged_bytes = 0U;
  cache_flush_all = 0U;
  cache_paused = 0U;
  cache_write_attempts = 0U;
  cache_committed_blocks = 0U;
  cache_committed_records = 0U;
  cache_committed_bytes = 0U;
  cache_committed_original_bytes = 0U;
  cache_committed_compressed_bytes = 0U;
  cache_overwritten_blocks = 0U;
  cache_oldest_sector = 0U;
  cache_active_sector = 0U;
  cache_active_offset = CACHE_SECTOR_DATA_OFFSET;
  cache_spare_sector = 1U;
  cache_spare_ready = 0U;
  cache_next_generation = 2U;
  cache_next_sequence = 1U;
  cache_event_records = 0U;
  result = Cache_WriteMeta() != 0U &&
                   Flash_EraseSectorBlocking(Cache_SectorAddress(0U)) != 0U &&
                   Flash_EraseSectorBlocking(Cache_SectorAddress(1U)) != 0U &&
                   Cache_WriteSectorHeaderBlocking(0U, 1U, 1U) != 0U
               ? 1U
               : 0U;
  if (result != 0U)
  {
    cache_erase_sectors += 2U;
    cache_spare_ready = 1U;
  }
  if (result != 0U && cache_time_valid != 0U)
  {
    (void)FlashCache_TimeSetUnix(current_unix, cache_time_utc_offset, now);
  }
  return result;
}

uint8_t FlashCache_SelfTest(void)
{
  uint8_t written[GD25_PAGE_SIZE];
  uint8_t readback[GD25_PAGE_SIZE];
  uint8_t result = 0U;

  if (cache_present == 0U || cache_io_state != CACHE_IO_IDLE ||
      cache_dump_state != CACHE_DUMP_IDLE)
  {
    return 0U;
  }
  for (uint16_t index = 0U; index < GD25_PAGE_SIZE; index++)
  {
    written[index] = (uint8_t)(index ^ 0xA5U);
  }
  memset(readback, 0, sizeof(readback));
  if (Flash_EraseSectorBlocking(CACHE_TEST_ADDRESS) != 0U &&
      Flash_WriteBlocking(CACHE_TEST_ADDRESS, written, sizeof(written)) != 0U &&
      Flash_Read(CACHE_TEST_ADDRESS, readback, sizeof(readback)) != 0U &&
      memcmp(written, readback, sizeof(written)) == 0)
  {
    result = 1U;
  }
  if (Flash_EraseSectorBlocking(CACHE_TEST_ADDRESS) == 0U)
  {
    result = 0U;
  }
  return result;
}

void FlashCache_GetStatus(FlashCacheStatus *status)
{
  uint32_t pending_bytes = cache_input_bytes + cache_staged_bytes;

  if (status == NULL)
  {
    return;
  }
  memset(status, 0, sizeof(*status));
  status->present = cache_present;
  status->jedec_id[0] = cache_jedec[0];
  status->jedec_id[1] = cache_jedec[1];
  status->jedec_id[2] = cache_jedec[2];
  status->busy = cache_io_state != CACHE_IO_IDLE ? 1U : 0U;
  status->paused = cache_paused;
  status->dump_active = cache_dump_state != CACHE_DUMP_IDLE ? 1U : 0U;
  status->dump_format = (uint8_t)cache_dump_format;
  status->session_id = cache_current_session;
  status->committed_bytes = cache_committed_bytes;
  status->committed_original_bytes = cache_committed_original_bytes;
  status->committed_compressed_bytes = cache_committed_compressed_bytes;
  status->pending_bytes = pending_bytes;
  status->committed_records = cache_committed_records;
  status->pending_records = cache_input_records + cache_staged_records;
  status->committed_blocks = cache_committed_blocks;
  status->pending_age_ms = cache_input_records + cache_staged_records != 0U
                               ? HAL_GetTick() - cache_pending_since_tick
                               : 0U;
  status->rx_bytes = cache_rx_bytes;
  status->tx_bytes = cache_tx_bytes;
  status->dropped_bytes = cache_dropped_bytes;
  status->dropped_records = cache_dropped_records;
  status->overwritten_blocks = cache_overwritten_blocks;
  status->write_blocks = cache_write_blocks;
  status->erase_sectors = cache_erase_sectors;
  status->write_errors = cache_write_errors;
  status->event_records = cache_event_records;
  status->dump_snapshot_bytes = cache_dump_snapshot_bytes;
  status->dump_output_bytes = cache_dump_output_bytes;
  status->dump_cancel_count = cache_dump_cancel_count;
  status->dump_overflow_count = cache_dump_overflow_count;
  status->active_free_bytes =
      cache_active_offset < GD25_SECTOR_SIZE
          ? GD25_SECTOR_SIZE - cache_active_offset
          : 0U;
  status->spare_ready = cache_spare_ready;
}


static void Cache_DumpFinish(FlashCacheDumpResult result)
{
  cache_dump_state = CACHE_DUMP_IDLE;
  cache_dump_writer = NULL;
  cache_dump_output_length = 0U;
  cache_dump_output_offset = 0U;
  cache_dump_result = result;
  cache_dump_result_bytes = cache_dump_output_bytes;
  cache_dump_result_format = cache_dump_format;
  cache_dump_result_had_records = cache_dump_had_records;
  if (result == FLASH_CACHE_DUMP_CANCELLED)
  {
    cache_dump_cancel_count++;
  }
  else if (result == FLASH_CACHE_DUMP_OVERFLOW)
  {
    cache_dump_overflow_count++;
  }
}

static void Cache_DumpCreateSnapshot(void)
{
  uint32_t total = cache_committed_bytes + cache_staged_bytes +
                   cache_input_bytes;

  cache_dump_snapshot_bytes =
      cache_dump_max_bytes != 0U && total > cache_dump_max_bytes
          ? cache_dump_max_bytes
          : total;
  cache_dump_skip_bytes = total - cache_dump_snapshot_bytes;
  cache_dump_snapshot_blocks = cache_committed_blocks;
  cache_dump_snapshot_oldest = cache_oldest_sector;
  cache_dump_snapshot_staged = cache_staged_valid;
  cache_dump_snapshot_staged_length = cache_staged_original_length;
  cache_dump_scan_sector = cache_dump_snapshot_oldest;
  cache_dump_scan_page = 1U;
  cache_dump_sectors_scanned = 0U;
  cache_dump_flash_emitted = 0U;
  cache_dump_ram_position = cache_input_tail;
  cache_dump_ram_end = cache_input_head;
  cache_dump_ram_overflow = 0U;
  cache_dump_had_records = 0U;
  cache_dump_payload_length = 0U;
  cache_dump_payload_offset = 0U;
  cache_dump_time_valid = 0U;
  cache_dump_time_session = 0U;
  cache_dump_state = CACHE_DUMP_FLASH;
}

uint8_t FlashCache_DumpStart(uint32_t max_bytes, FlashCacheDumpFormat format,
                             FlashCacheWriter writer)
{
  if (cache_present == 0U || writer == NULL ||
      cache_dump_state != CACHE_DUMP_IDLE ||
      cache_dump_result != FLASH_CACHE_DUMP_NONE ||
      format > FLASH_CACHE_DUMP_FORMAT_TEXT)
  {
    return 0U;
  }
  cache_dump_writer = writer;
  cache_dump_format = format;
  cache_dump_max_bytes = max_bytes;
  cache_dump_snapshot_bytes = 0U;
  cache_dump_output_bytes = 0U;
  cache_dump_output_length = 0U;
  cache_dump_output_offset = 0U;
  cache_dump_had_records = 0U;
  cache_dump_state = CACHE_DUMP_WAIT_IO;
  return 1U;
}

void FlashCache_DumpTask(void)
{
  if (cache_dump_state == CACHE_DUMP_IDLE)
  {
    return;
  }
  if (cache_dump_ram_overflow != 0U)
  {
    Cache_DumpFinish(FLASH_CACHE_DUMP_OVERFLOW);
    return;
  }
  if (cache_dump_output_offset < cache_dump_output_length)
  {
    uint16_t length =
        (uint16_t)(cache_dump_output_length - cache_dump_output_offset);
    if (length > CACHE_DUMP_TX_CHUNK)
    {
      length = CACHE_DUMP_TX_CHUNK;
    }
    if (cache_dump_writer(&cache_dump_output[cache_dump_output_offset],
                          length) != 0U)
    {
      cache_dump_output_offset =
          (uint16_t)(cache_dump_output_offset + length);
      if (cache_dump_output_offset == cache_dump_output_length)
      {
        cache_dump_output_offset = 0U;
        cache_dump_output_length = 0U;
      }
    }
    return;
  }

  if (cache_dump_state == CACHE_DUMP_WAIT_IO)
  {
    if (cache_io_state == CACHE_IO_IDLE)
    {
      Cache_DumpCreateSnapshot();
    }
    return;
  }

  if (cache_dump_payload_offset < cache_dump_payload_length)
  {
    if (Cache_DumpProcessPayloadRecord() == 0U)
    {
      Cache_DumpFinish(FLASH_CACHE_DUMP_ERROR);
    }
    return;
  }

  if (cache_dump_state == CACHE_DUMP_FLASH)
  {
    CacheExtentHeader header;
    uint32_t address;

    if (cache_dump_flash_emitted >= cache_dump_snapshot_blocks ||
        cache_dump_sectors_scanned >= CACHE_DATA_SECTOR_COUNT)
    {
      cache_dump_state = CACHE_DUMP_STAGED;
      return;
    }
    if (cache_dump_scan_page >= CACHE_PAGES_PER_SECTOR)
    {
      cache_dump_scan_sector =
          (cache_dump_scan_sector + 1U) % CACHE_DATA_SECTOR_COUNT;
      cache_dump_scan_page = 1U;
      cache_dump_sectors_scanned++;
      return;
    }
    address = Cache_SectorAddress(cache_dump_scan_sector) +
              (uint32_t)cache_dump_scan_page * GD25_PAGE_SIZE;
    if (Cache_ReadExtentHeader(address, &header) != 0U &&
        Cache_ExtentHeaderValid(&header) != 0U &&
        cache_dump_scan_page + header.extent_pages <=
            CACHE_PAGES_PER_SECTOR)
    {
      cache_dump_scan_page =
          (uint8_t)(cache_dump_scan_page + header.extent_pages);
      if (Cache_ReadValidExtent(address, &header, cache_dump_compressed,
                                cache_dump_payload) != 0U)
      {
        cache_dump_flash_emitted++;
        cache_dump_payload_length = header.original_length;
        cache_dump_payload_offset = 0U;
        cache_dump_payload_session = header.session_id;
      }
    }
    else
    {
      cache_dump_scan_page++;
    }
    return;
  }

  if (cache_dump_state == CACHE_DUMP_STAGED)
  {
    cache_dump_state = CACHE_DUMP_RAM;
    if (cache_dump_snapshot_staged != 0U)
    {
      CacheExtentHeader header;
      memcpy(&header, cache_extent, sizeof(header));
      if (Cache_Lz4Decompress(&cache_extent[CACHE_EXTENT_HEADER_SIZE],
                              header.compressed_length, cache_dump_payload,
                              header.original_length) == 0U)
      {
        Cache_DumpFinish(FLASH_CACHE_DUMP_ERROR);
        return;
      }
      cache_dump_payload_length = header.original_length;
      cache_dump_payload_offset = 0U;
      cache_dump_payload_session = header.session_id;
    }
    return;
  }

  if (cache_dump_state == CACHE_DUMP_RAM)
  {
    CacheRecordHeader record;
    uint8_t data[CACHE_RECORD_DATA_MAX];

    if (cache_dump_ram_position == cache_dump_ram_end)
    {
      Cache_DumpFinish(FLASH_CACHE_DUMP_COMPLETE);
      return;
    }
    if (Cache_RingPeekRecord(cache_dump_ram_position, &record) == 0U)
    {
      Cache_DumpFinish(FLASH_CACHE_DUMP_ERROR);
      return;
    }
    cache_dump_ram_position =
        Cache_RingAdvance(cache_dump_ram_position, sizeof(record));
    Cache_RingCopyFrom(cache_dump_ram_position, data, record.length);
    cache_dump_ram_position =
        Cache_RingAdvance(cache_dump_ram_position, record.length);
    if (Cache_DumpPrepareRecord(cache_current_session, &record, data) == 0U)
    {
      Cache_DumpFinish(FLASH_CACHE_DUMP_ERROR);
    }
  }
}

void FlashCache_DumpCancel(void)
{
  if (cache_dump_state != CACHE_DUMP_IDLE)
  {
    Cache_DumpFinish(FLASH_CACHE_DUMP_CANCELLED);
  }
}

uint8_t FlashCache_DumpIsActive(void)
{
  return cache_dump_state != CACHE_DUMP_IDLE ? 1U : 0U;
}

uint8_t FlashCache_DumpTakeResult(FlashCacheDumpResult *result,
                                  uint32_t *dumped_bytes,
                                  FlashCacheDumpFormat *format,
                                  uint8_t *had_records)
{
  if (cache_dump_result == FLASH_CACHE_DUMP_NONE)
  {
    return 0U;
  }
  if (result != NULL)
  {
    *result = cache_dump_result;
  }
  if (dumped_bytes != NULL)
  {
    *dumped_bytes = cache_dump_result_bytes;
  }
  if (format != NULL)
  {
    *format = cache_dump_result_format;
  }
  if (had_records != NULL)
  {
    *had_records = cache_dump_result_had_records;
  }
  cache_dump_result = FLASH_CACHE_DUMP_NONE;
  return 1U;
}
