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
#include <target/algorithm.h>
#include <target/armv7m.h>

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
#define CC_FCTL_UPPER       (1<<9)
#define CC_FCTL_CACHE       ((1<<2) | (1<<3))

#define CC_FIRST_TIMEOUT    1
#define CC_ERASE_TIMEOUT    20

// Number of first byte in CCA page which contains lock bits.
#define CC_LOCK_BITS_OFFSET 2016

// When erasing, page number should be written to FADDR[16:9]

// But... Contrary to what the datasheet (8.10.1.2 -> FLASH_CTRL_FADDR) says,
// this register seems to be right shifted by two on *write* not read.
// That is, to write to bit b you need to write 1<<(b+2). This makes some sense
// as that way you operate all the time on byte-addresses not word-addresses.
// Also, bits 0, 1 are ignored on write - as flash writes need to be word-aligned.
#define CC_FLASH_PAGE_ADDR_SHIFT 11

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

static int cc2xxx_flash_addr_sanity_check(uint32_t addr) {
  if(addr > CC_FLASH_TOP || addr < CC_FLASH_BASE) {
    LOG_ERROR("invalid flash address: %08x", addr);
    return ERROR_FAIL;
  }
  return ERROR_OK;
}

static int cc2xxx_get_upper_page_base(struct flash_bank *bank, uint32_t *upper_page_base) {
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;
  *upper_page_base = CC_FLASH_BASE + cc2xxx_info->flash_size_b
                   - CC_FLASH_PAGE_SIZE;
  return cc2xxx_flash_addr_sanity_check(*upper_page_base);
}

static int cc2xxx_get_lock_bit_base(struct flash_bank *bank, uint32_t *lock_bit_base) {
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;
  *lock_bit_base = CC_FLASH_BASE + cc2xxx_info->flash_size_b
                   - CC_FLASH_PAGE_SIZE + CC_LOCK_BITS_OFFSET;
  return cc2xxx_flash_addr_sanity_check(*lock_bit_base);
}

static int cc2xxx_wait(struct flash_bank *bank, int timeout)
{
  struct target *target = bank->target;
  uint32_t fctl;
  int retval;

  while(timeout--) {
    retval = target_read_u32(target, CC_FCTL_REG, &fctl);
    if (retval != ERROR_OK) return retval;
    if(fctl & CC_FCTL_ABORT) {
      LOG_ERROR("Operation aborted by flash controller.");
      return ERROR_FAIL;
    }
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

static int cc2xxx_fctl_clear(struct flash_bank *bank, uint32_t mask)
{
  struct target *target = bank->target;
  uint32_t fctl;
  int retval;

  LOG_DEBUG("clear");

  retval = target_read_u32(target, CC_FCTL_REG, &fctl);
  if (retval != ERROR_OK) return retval;
  retval = target_write_u32(target, CC_FCTL_REG, fctl & ~mask);
  if (retval != ERROR_OK) return retval;

  LOG_DEBUG("end clear");

  return ERROR_OK;
}

static int cc2xxx_protect_check(struct flash_bank *bank)
{
  struct target *target = bank->target;
  int i, j, retval;

  uint32_t lock_bit_base;
  retval = cc2xxx_get_lock_bit_base(bank, &lock_bit_base);
  if (retval != ERROR_OK) return retval;

  LOG_DEBUG("lock_bit_base: %08x", lock_bit_base);

  // Each byte of lock bit page holds lock bits for 8 pages.
  int bytes_to_read = (bank->num_sectors + 7) / 8;
  uint8_t lock_bits;

  for (i=0;i<bytes_to_read;++i) {
    retval = target_read_u8(target, lock_bit_base + i, &lock_bits);
    if (retval != ERROR_OK) return retval;
    for(j=0;j<8 && 8*i+j < bank->num_sectors - 1;++j) {
      // 1 - write/erase allowed, 0 - write/erase blocked
      bank->sectors[8*i + j].is_protected = lock_bits & (1<<j) ? 0 : 1;
    }
  }

  // Last page is protected only by a bit in FCTL, we will always lift the
  // protection on erase/write.
  bank->sectors[bank->num_sectors-1].is_protected = 0;

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

   if(i == bank->num_sectors - 1) {
    // Special handling of last page - set UPPER_PAGE_ACCESS.
    // Erasing is quite safe - it sets config bytes to 0xff, which means
    // write/erase allowed to all pages, jtag enabled.
    retval = cc2xxx_fctl_set(bank, CC_FCTL_ERASE | CC_FCTL_UPPER);
   } else {
    retval = cc2xxx_fctl_set(bank, CC_FCTL_ERASE);
   }
   if (retval != ERROR_OK) return retval;

   retval = cc2xxx_wait(bank, CC_ERASE_TIMEOUT);
   if (retval != ERROR_OK) return retval;

   if(i == bank->num_sectors - 1) {
    // Clear UPPER_PAGE_ACCESS.
    retval = cc2xxx_fctl_clear(bank, CC_FCTL_UPPER);
    if (retval != ERROR_OK) return retval;
   }

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
	struct target *target = bank->target;
  struct cc2xxx_flash_bank *cc2xxx_info = bank->driver_priv;
  // See note at CC_FLASH_PAGE_ADDR_SHIFT definition.
	uint32_t addr = ((CC_FLASH_TOP - CC_FLASH_BASE) & offset);
	struct working_area *target_buf;
	struct working_area *target_write_alg;
	uint32_t buf_size = 8192;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_info;
  int retval, retval2 = ERROR_OK;
  uint32_t upper_page_base, lock_bit_base;
  retval = cc2xxx_get_upper_page_base(bank, &upper_page_base);
  if (retval != ERROR_OK) return retval;
  retval = cc2xxx_get_lock_bit_base(bank, &lock_bit_base);
  if (retval != ERROR_OK) return retval;

  if (offset + CC_FLASH_BASE > CC_FLASH_TOP || offset > cc2xxx_info->flash_size_b) {
		LOG_ERROR("Invalid offset: %d", offset);
		return ERROR_FAIL;
  }

  LOG_DEBUG("write offset = %08x, count = %x", offset, count);

  if (offset + CC_FLASH_BASE + count > lock_bit_base) {
		LOG_ERROR("Attempting direct write to lock bits (%d), disallowing.", offset);
		return ERROR_FAIL;
  }

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x3) {
		LOG_WARNING("offset 0x%" PRIx32 " breaks required 4-byte alignment",
					offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	if (count & 0x3) {
		LOG_WARNING("Padding %d bytes to keep 4-byte write size",
					       count & 3);
		count = (count + 3) & ~3;
  }

  if (offset + CC_FLASH_BASE + count > upper_page_base) {
    // Enable upper page access if needed. Hopefully checks above will
    // protect us from breaking stuff there. This won't work if write algorithm
    // (the one executed on target) isn't sane ex. writes to random locations...
    // When modifying it, test first with those lines commented out!
    LOG_INFO("Will write to upper page, setting access bit.");
    retval = cc2xxx_fctl_set(bank, CC_FCTL_UPPER);
    if (retval != ERROR_OK) return retval;
  }

  // Disable cache
  retval = cc2xxx_fctl_clear(bank, CC_FCTL_CACHE);
  if (retval != ERROR_OK) return retval;

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

  static const uint8_t cc2xxx_write_alg[] = {
    0xdf, 0xf8, 0x74, 0x80, 0xc8, 0xf8, 0x00, 0x20, 0xd0, 0xf8, 0x00, 0x80,
    0xb8, 0xf1, 0x00, 0x0f, 0x27, 0xd0, 0x47, 0x68, 0xb8, 0xeb, 0x07, 0x06,
    0x03, 0x2e, 0xf5, 0xd3, 0xdf, 0xf8, 0x54, 0x80, 0xd8, 0xf8, 0x00, 0x60,
    0x46, 0xf0, 0x02, 0x06, 0xc8, 0xf8, 0x00, 0x60, 0x57, 0xf8, 0x04, 0x6b,
    0xdf, 0xf8, 0x48, 0x80, 0xc8, 0xf8, 0x00, 0x60, 0xdf, 0xf8, 0x38, 0x80,
    0xd8, 0xf8, 0x00, 0x60, 0x16, 0xf0, 0x40, 0x0f, 0xf8, 0xd1, 0x16, 0xf0,
    0x20, 0x0f, 0x0d, 0xd1, 0x16, 0xf0, 0x02, 0x0f, 0x0a, 0xd0, 0x8f, 0x42,
    0x28, 0xbf, 0x00, 0xf1, 0x08, 0x07, 0x47, 0x60, 0x01, 0x3b, 0x03, 0xb1,
    0xd2, 0xe7, 0x4f, 0xf0, 0x00, 0x00, 0x03, 0xe0, 0x4f, 0xf0, 0x01, 0x00,
    0x00, 0x21, 0x41, 0x60, 0x00, 0xbe, 0x00, 0xbf, 0x08, 0x30, 0x0d, 0x40,
    0x0c, 0x30, 0x0d, 0x40, 0x10, 0x30, 0x0d, 0x40
  };

	retval = target_alloc_working_area(target, sizeof(cc2xxx_write_alg),
      &target_write_alg);
  if(retval != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};
	retval = target_alloc_working_area(target, buf_size, &target_buf);
  if(retval != ERROR_OK) {
    target_free_working_area(target, target_write_alg);
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};

  retval = target_write_buffer(target, target_write_alg->address,
      sizeof(cc2xxx_write_alg), cc2xxx_write_alg);
  if(retval != ERROR_OK) return retval;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* buffer start, status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* buffer end */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* target address */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* count */

	buf_set_u32(reg_params[0].value, 0, 32, target_buf->address);
	buf_set_u32(reg_params[1].value, 0, 32, target_buf->address + target_buf->size);
	buf_set_u32(reg_params[2].value, 0, 32, addr);
	buf_set_u32(reg_params[3].value, 0, 32, count / 4);

  // This will handle copying data to sram
  retval = target_run_flash_async_algorithm(target, buffer, count, 1,
      0, NULL, 4, reg_params,
      target_buf->address, buf_size,
      target_write_alg->address, 0,
      &armv7m_info);

	uint32_t error = buf_get_u32(reg_params[0].value, 0, 32);

  // Clean up before checking error

  if (offset + CC_FLASH_BASE + count > upper_page_base) {
    retval2 = cc2xxx_fctl_clear(bank, CC_FCTL_UPPER);
      LOG_INFO("Clearing upper page access bit.");
    if(retval2 != ERROR_OK) {
      LOG_WARNING("error cleaning upper page lock bit, danger!");
    }
  }

  target_free_working_area(target, target_buf);
  target_free_working_area(target, target_write_alg);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

  if(retval != ERROR_OK || error != 0) {
    LOG_ERROR("write algorithm error: %08x, retval: %d", error, retval);
    return ERROR_FAIL;
  }
  if(retval2 != ERROR_OK) return retval2;

  return ERROR_OK;
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
  bank->num_sectors = (cc2xxx_info->flash_size_b / CC_FLASH_PAGE_SIZE);

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
