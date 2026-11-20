/*
  storage.c - Saved registers in the last sector of the flash

  The data is stored with a magic number, its size and a checksum, so
  erased or old data is not used. Writes are delayed by the caller (one
  second after the last change), so the sector is not worn out by a burst
  of register writes.

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "blinky.h"

#define STORAGE_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE) // Last sector
#define STORAGE_MAGIC  0x56534350u                                 // "VSCP"

typedef struct {
  uint32_t magic;
  uint16_t size;
  uint16_t checksum;
} storage_header_t;

static uint16_t
checksum(const uint8_t *p, size_t size)
{
  uint16_t sum = 0x5555;
  for (size_t i = 0; i < size; i++) {
    sum = (uint16_t) ((sum << 1) | (sum >> 15)) ^ p[i];
  }
  return sum;
}

bool
storage_load(void *data, size_t size)
{
  const uint8_t *flash           = (const uint8_t *) (XIP_BASE + STORAGE_OFFSET);
  const storage_header_t *header = (const storage_header_t *) flash;
  if ((STORAGE_MAGIC != header->magic) || (size != header->size) ||
      (checksum(flash + sizeof(*header), size) != header->checksum)) {
    return false;
  }
  memcpy(data, flash + sizeof(storage_header_t), size);
  return true;
}

void
storage_save(const void *data, size_t size)
{
  static uint8_t page[FLASH_PAGE_SIZE * 2]; // Header + data, whole pages
  storage_header_t header = { STORAGE_MAGIC, (uint16_t) size, checksum(data, size) };
  if (sizeof(header) + size > sizeof(page)) {
    printf("Storage: too much data (%u bytes)\n", (unsigned) size);
    return;
  }
  memset(page, 0xff, sizeof(page));
  memcpy(page, &header, sizeof(header));
  memcpy(page + sizeof(header), data, size);

  // The flash can not be read (code run from it) while it is written
  uint32_t flags = save_and_disable_interrupts();
  flash_range_erase(STORAGE_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(STORAGE_OFFSET, page, sizeof(page));
  restore_interrupts(flags);
}
