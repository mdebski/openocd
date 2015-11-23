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
 * CC2538 Datasheet
 * http://www.ti.com/lit/ug/swru319c/swru319c.pdf
 *
 * CC2538 TI driverlib documentation
 * http://www.ti.com/lit/ug/swru325a/swru325a.pdf
*/

#define CC_FLASH_BASE       0x00200000
#define CC_FLASH_TOP        0x0027FFFF

#define ERROR_NYI ERROR_FAIL

/* flash bank cc2xxx <base> <size> 0 0 <target#> */
FLASH_BANK_COMMAND_HANDLER(cc2xxx_flash_bank_command)
{
	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	return ERROR_NYI;
}

static int cc2xxx_protect_check(struct flash_bank *bank)
{
	return ERROR_NYI;
}

static int cc2xxx_erase(struct flash_bank *bank, int first, int last)
{
	return ERROR_NYI;
}

static int cc2xxx_protect(struct flash_bank *bank, int set, int first, int last)
{
	return ERROR_NYI;
}

static int cc2xxx_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	return ERROR_NYI;
}

static int cc2xxx_probe(struct flash_bank *bank)
{
	return ERROR_NYI;
}

static int cc2xxx_info(struct flash_bank *bank, char *buf, int buf_size)
{
	return ERROR_NYI;
}

static int cc2xxx_auto_probe(struct flash_bank *bank)
{
	return ERROR_NYI;
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
