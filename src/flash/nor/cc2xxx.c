/***************************************************************************
 *   Copyright (C) 2015 by Maciej Dębski                                   *
 *   md319428@students.mimuw.edu.pl                                        *
 *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.                                        *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"

/* References
 *
 * CC2538 ROM
 * http://www.ti.com/lit/ug/swru333a/swru333a.pdf
 *
 * CC2538 Datasheet (chapter 8)
 * http://www.ti.com/lit/ug/swru319c/swru319c.pdf
 *
 * CC2538 TI driverlib documentation
 * http://www.ti.com/lit/ug/swru325a/swru325a.pdf
*/

#define CC_FLASH_BASE       0x00200000
#define CC_FLASH_TOP        0x0027FFFF
#define CC_FLASH_PAGE_SIZE  0x800

#define CC_FCTL_REG         0x400D3008
#define CC_FADDR_REG        0x400D300C
#define CC_FWDATA_REG       0x400D3010
#define CC_DIECFG0_REG      0x400D3014

#define CC_FCTL_ERASE       (1<<0)
#define CC_FCTL_WRITE       (1<<1)
#define CC_FCTL_ABORT       (1<<5)
#define CC_FCTL_FULL        (1<<6)
#define CC_FCTL_BUSY        (1<<7)

#define CC_FIRST_TIMEOUT    1
#define CC_ERASE_TIMEOUT    20

// Number of first byte in CCA page which contains lock bits.
#define CC_LOCK_BITS_OFFSET 2016

// When erasing, page number should be written to FADDR[16:9]
#define CC_FLASH_PAGE_ADDR_SHIFT 9

#define NYI LOG_ERROR("Not Implemented Yet"); return ERROR_FAIL;

struct cc2xxx_flash_bank {
  int probed;
  uint16_t chip_id;
  uint32_t flash_size_b;
};

/* flash bank cc2xxx <base> <size> 0 0 <target#> */
FLASH_BANK_COMMAND_HANDLER(cc2xxx_flash_bank_command)
{
  if (CMD_ARGC < 6)
    return ERROR_COMMAND_SYNTAX_ERROR;

  struct cc2xxx_flash_bank *cc2xxx_info = malloc(sizeof(struct cc2xxx_flash_bank));
  if (!cc2xxx_info) {
    LOG_WARNING("cannot allocate host memory");
    return ERROR_FAIL;
  }
  bank->driver_priv = cc2xxx_info;

  cc2xxx_info->probed = 0;

  return ERROR_OK;
}

static int cc2xxx_get_lock_bit_base(struct flash_bank *bank, uint32_t *lock_bit_base) {
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;
  *lock_bit_base = CC_FLASH_BASE + cc2xxx_info->flash_size_b
                   - CC_FLASH_PAGE_SIZE + CC_LOCK_BITS_OFFSET;

  if(*lock_bit_base < CC_FLASH_BASE || *lock_bit_base > CC_FLASH_TOP) {
    LOG_ERROR("invalid lock_bit_base: %08x", *lock_bit_base);
    return ERROR_FAIL;
  }

  return ERROR_OK;
}

static int cc2xxx_wait(struct flash_bank *bank, int timeout)
{
  struct target *target = bank->target;
  uint32_t fctl;
  int retval;

  while(timeout--) {
    retval = target_read_u32(target, CC_FCTL_REG, &fctl);
    if (retval != ERROR_OK) return retval;
    if((fctl & CC_FCTL_BUSY) == 0) return ERROR_OK;
    alive_sleep(1);
  }

  LOG_ERROR("timeout reached");
  return ERROR_FAIL;
}

static int cc2xxx_fctl_set(struct flash_bank *bank, uint32_t mask)
{
  struct target *target = bank->target;
  uint32_t fctl;
  int retval;

  retval = target_read_u32(target, CC_FCTL_REG, &fctl);
  if (retval != ERROR_OK) return retval;
  retval = target_write_u32(target, CC_FCTL_REG, fctl | mask);
  if (retval != ERROR_OK) return retval;

  return ERROR_OK;
}

static int cc2xxx_protect_check(struct flash_bank *bank)
{
  struct target *target = bank->target;

  uint32_t lock_bit_base;
  cc2xxx_get_lock_bit_base(bank, &lock_bit_base);

  LOG_DEBUG("lock_bit_base: %08x", lock_bit_base);

  // Each byte of lock bit page holds lock bits for 8 pages.
  int bytes_to_read = (bank->num_sectors + 7) / 8;
  uint8_t lock_bits;

  int i, j, retval;
  for (i=0;i<bytes_to_read;++i) {
    retval = target_read_u8(target, lock_bit_base + i, &lock_bits);
    if (retval != ERROR_OK) return retval;
    for(j=0;j<8 && 8*i+j < bank->num_sectors;++j) {
      // 1 - write/erase allowed, 0 - write/erase blocked
      bank->sectors[8*i + j].is_protected = lock_bits & (1<<j) ? 0 : 1;
    }
  }

  return ERROR_OK;
}

static int cc2xxx_erase(struct flash_bank *bank, int first, int last)
{
  struct target *target = bank->target;
  int retval;

  if(first > last || first < 0 || last >= bank->num_sectors) {
    LOG_ERROR("invalid erase params: %d, %d", first, last);
    return ERROR_FAIL;
  }

  int i;
  for(i=first;i<=last;++i) {
   retval = cc2xxx_wait(bank, CC_FIRST_TIMEOUT);
   if (retval != ERROR_OK) return retval;

   retval = target_write_u32(target, CC_FADDR_REG, i << CC_FLASH_PAGE_ADDR_SHIFT);
   if (retval != ERROR_OK) return retval;
   retval = cc2xxx_fctl_set(bank, CC_FCTL_ERASE);
   if (retval != ERROR_OK) return retval;

   retval = cc2xxx_wait(bank, CC_ERASE_TIMEOUT);
   if (retval != ERROR_OK) return retval;

   bank->sectors[i].is_erased = 1;
  }

  return ERROR_OK;
}

static int cc2xxx_protect(struct flash_bank *bank, int set, int first, int last)
{
  NYI;
}

static int cc2xxx_write(struct flash_bank *bank, const uint8_t *buffer,
    uint32_t offset, uint32_t count)
{
  NYI;
}

static int cc2xxx_fetch_info(struct flash_bank *bank) {
  struct target *target = bank->target;
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;

  uint32_t diecfg0;
  int retval = target_read_u32(target, CC_DIECFG0_REG, &diecfg0);
  if (retval != ERROR_OK) return retval;

  cc2xxx_info->chip_id = (uint16_t) (diecfg0 >> 16);

  switch ((diecfg0 & ((1<<6)|(1<<5)|(1<<4))) >> 4) {
    case 0x04:
      cc2xxx_info->flash_size_b = 512 * 1024; break;
    case 0x03:
      cc2xxx_info->flash_size_b = 384 * 1024; break;
    case 0x02:
      cc2xxx_info->flash_size_b = 256 * 1024; break;
    case 0x01:
      cc2xxx_info->flash_size_b = 128 * 1024; break;
    case 0x00:
      cc2xxx_info->flash_size_b = 64 * 1024; break;
    default:
      LOG_ERROR("Unknown flash size. diecfg0: 0x%08x", diecfg0);
      return ERROR_FAIL;
  }

  return ERROR_OK;
}

static int cc2xxx_probe(struct flash_bank *bank)
{
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;

  int retval = cc2xxx_fetch_info(bank);
  if (retval != ERROR_OK) return retval;

  LOG_INFO("chip id: 0x%04hx", cc2xxx_info->chip_id);
  LOG_INFO("flash size: %d bytes", cc2xxx_info->flash_size_b);

  if(cc2xxx_info->flash_size_b % CC_FLASH_PAGE_SIZE != 0) {
    LOG_ERROR("Incorrect flash or page size: %d or %d",
        cc2xxx_info->flash_size_b, CC_FLASH_PAGE_SIZE);
    return ERROR_FAIL;
  }

  bank->base = CC_FLASH_BASE;
  bank->size = cc2xxx_info->flash_size_b;
  // Do not touch the last page - it's Lock Bit & CCA Page
  bank->num_sectors = (cc2xxx_info->flash_size_b / CC_FLASH_PAGE_SIZE) - 1;

  bank->sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
  if (!bank->sectors) {
    LOG_WARNING("cannot allocate host memory");
    return ERROR_FAIL;
  }

  int i;
  for (i=0;i<bank->num_sectors;++i) {
    bank->sectors[i].offset = i * CC_FLASH_PAGE_SIZE;
    bank->sectors[i].size = CC_FLASH_PAGE_SIZE;
    bank->sectors[i].is_erased = -1;
    bank->sectors[i].is_protected = 1;
  }

  uint32_t lock_bit_base;
  cc2xxx_get_lock_bit_base(bank, &lock_bit_base);
  uint32_t val;
  target_read_u32(bank->target, lock_bit_base - 4, &val);
  LOG_DEBUG("entry point: 0x%08x", val);
  target_read_u32(bank->target, lock_bit_base - 8, &val);
  LOG_DEBUG("image valid (yes if 0): 0x%08x", val);

  cc2xxx_info->probed = 1;

  return ERROR_OK;
}

static int cc2xxx_info(struct flash_bank *bank, char *buf, int buf_size)
{
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;

  int retval = cc2xxx_fetch_info(bank);
  if (retval != ERROR_OK) return retval;

  int wrote = 0;

  switch (cc2xxx_info->chip_id) {
   case 0xB964:
    retval = snprintf(buf + wrote, buf_size - wrote, "TI CC2538");
    if(retval < 0) return ERROR_FAIL;
    wrote += retval;
    break;
   default:
    retval = snprintf(buf + wrote, buf_size - wrote, "Unknown");
    if(retval < 0) return ERROR_FAIL;
    wrote += retval;
    break;
  }

  retval = snprintf(buf + wrote, buf_size - wrote, " - %d KB", cc2xxx_info->flash_size_b / 1024);
  if(retval < 0) return ERROR_FAIL;
  wrote += retval;

  return ERROR_OK;
}

static int cc2xxx_auto_probe(struct flash_bank *bank)
{
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;
  if (cc2xxx_info->probed)
    return ERROR_OK;
  return cc2xxx_probe(bank);
}

static const struct command_registration cc2xxx_exec_command_handlers[] = {
  COMMAND_REGISTRATION_DONE
};

static const struct command_registration cc2xxx_command_handlers[] = {
  {
    .name = "cc2xxx",
    .mode = COMMAND_ANY,
    .help = "cc2xxx flash command group",
    .usage = "",
    .chain = cc2xxx_exec_command_handlers,
  },
  COMMAND_REGISTRATION_DONE
};

struct flash_driver cc2xxx_flash = {
  .name = "cc2xxx",
  .commands = cc2xxx_command_handlers,
  .flash_bank_command = cc2xxx_flash_bank_command,
  .erase = cc2xxx_erase,
  .protect = cc2xxx_protect,
  .write = cc2xxx_write,
  .read = default_flash_read,
  .probe = cc2xxx_probe,
  .erase_check = default_flash_blank_check,
  .protect_check = cc2xxx_protect_check,
  .info = cc2xxx_info,
  .auto_probe = cc2xxx_auto_probe,
};
