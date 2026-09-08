/*
 * SomnoTrace - SNT to EDF waveform conversion (BRP, PLD, SA2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include "edf_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t edf_find_mask_on_time(const char *events_snt_path);
int64_t edf_find_mask_off_time(const char *events_snt_path);
int64_t edf_find_zle_edge_time(const char *events_snt_path, int want_value,
                               int64_t clock_drift_ms);

static inline uint32_t edf_ms_to_samples_25hz(int64_t ms)
{
    return (ms <= 0) ? 0 : (uint32_t)((ms * 25 + 500) / 1000);
}

static inline uint32_t edf_ms_to_samples_1hz(int64_t ms)
{
    return (ms <= 0) ? 0 : (uint32_t)((ms + 500) / 1000);
}

static inline uint32_t edf_ms_to_samples_pld(int64_t ms)
{
    return (ms <= 0) ? 0 : (uint32_t)((ms + 1000) / 2000);
}

#ifdef __cplusplus
}
#endif
