/***************************************************************************
 *   Copyright (C) 2015 Maciej Dębski                                      *
 *   md319428@students.mimuw.edu.pl                                        *
 *                                                                         *
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

	.text
	.syntax unified
	.cpu cortex-m3
	.thumb
	.thumb_func

/* building:
 arm-none-eabi-gcc -c cc2xxx.S
 arm-none-eabi-objcopy -O binary cc2xxx.o cc2xxx_write_alg.bin
 xxd -i cc2xxx_write_alg.bin
*/

/*
 * Params:
 * r0 = workarea start, status (out)
 * r1 = workarea end
 * r2 = target address
 * r3 = count
 *
 * Clobbered:
 * r6 - temp
 * r7 - rp
 * r8 - wp, tmp
 */

wait_fifo:
	ldr 	r8, [r0, #0]	/* read wp */
	cmp 	r8, #0		/* abort if wp == 0 */
	beq 	ok
	ldr 	r7, [r0, #4]	/* read rp */
	subs	r6, r8, r7	/* number of bytes available for read in r6*/
	cmp 	r6, #3		/* wait until 4 bytes are available */
	bcc 	wait_fifo

init:

  /* load flash addr */
  ldr   r8, CC_FADDR_REG
  str   r2, [r8] /* set write addr */

  /* set fctl.write */
  ldr   r8, CC_FCTL_REG
  ldr   r6, [r8]
  orr   r6, r6, #2
  str   r6, [r8]

  /* write data */
	ldr 	r6, [r7], #0x04 /* read one word from src, increment ptr */
  ldr   r8, CC_FWDATA_REG
  str   r6, [r8]
  mov   r11, #0 // DEBUG
	ldr 	r8, CC_FCTL_REG // DEBUG
  ldr   r10, [r8] // DEBUG

busy:
  add   r11, r11, #1 // DEBUG
	ldr 	r8, CC_FCTL_REG
  ldr   r6, [r8]
	tst 	r6, #64 /* FULL */
	bne 	busy

  tst r6, #32 /* ABORT */
  bne   err_abort
  tst r6, #2 /* WRITE */
  beq   err_notwrite

	cmp 	r7, r1		/* wrap rp at end of buffer */
	it  	cs
	addcs	r7, r0, #8	/* skip loader args */
	str 	r7, [r0, #4]	/* store rp */
	subs	r3, r3, #1	/* decrement word count */
	cbz 	r3, ok	/* loop if not done */
	b	wait_fifo

err_abort:
  mov r0, 0xaa
  mov r12, 0xaa
  b error
err_notwrite:
  mov r0, 0xbb
  mov r12, 0xbb
  b error

ok:
	mov	r0, #0		/* clear r0 on success */
  b exit
error:
  /* set r0 on error to (err_code; fctl) */
  lsl r0, #16
  mov r1, #255
  and r6, r6, r1
  orr r0, r0, r6
	movs	r1, #0
	str	r1, [r0, #4]	/* set rp = 0 on error */
exit:
  ldr   r8, CC_FADDR_REG
  ldr   r4, [r8] /* set write addr */
	bkpt	#0x00

.align 2
CC_FCTL_REG:    .word 0x400d3008
CC_FADDR_REG:   .word 0x400d300c
CC_FWDATA_REG:  .word 0x400d3010
CC_FADDR_MASK:  .word 0x0001ffff
