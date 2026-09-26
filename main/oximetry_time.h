/*
 * SomnoTrace - Civil time from oximeter recording names
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local civil time -> UTC epoch ms, or 0 when it is not a usable time: a year
 * outside 2015..2099, a field out of range, or a date the calendar does not
 * have (30 Feb).  The fields are LOCAL time, resolved with mktime(). */
int64_t oximetry_civil_epoch_ms(int year, int mon, int day, int hour, int min, int sec);

/* A recording name that starts YYYYMMDDhhmmss -> UTC epoch ms, or 0. */
int64_t oximetry_filename_epoch_ms(const char *name);

#ifdef __cplusplus
}
#endif
