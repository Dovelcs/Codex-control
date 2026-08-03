#include "crc32.h"

uint32_t Crc32_Calculate(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFU;

  while (length-- != 0U)
  {
    crc ^= *bytes++;
    for (uint32_t bit = 0U; bit < 8U; ++bit)
    {
      crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ 0xEDB88320U)
                               : (crc >> 1U);
    }
  }

  return crc ^ 0xFFFFFFFFU;
}
