/*
 * microdevt - Microcontroller Development Toolkit
 *
 * Copyright (c) 2017, Krzysztof Witek
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "LICENSE".
 *
*/

#ifndef _X86_TIMER_H_
#define _X86_TIMER_H_

#include <stdint.h>

void __timer_subsystem_init(void);
void __timer_subsystem_stop(void);
static inline void __timer_subsystem_start(void)
{
	return __timer_subsystem_init();
}

static inline uint8_t __timer_subsystem_is_runing(void)
{
	return 1;
}

static inline void __timer_subsystem_reset(void)
{
	__timer_subsystem_stop();
	__timer_subsystem_start();
}

#endif
