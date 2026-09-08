/*
 * SomnoTrace - ES8311 audio codec driver for alarm tones
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Initialise the ES8311 codec, I2S bus, and PA for DAC playback.
 * Must be called after bsp_display_init (shares I2C bus pins). */
esp_err_t bsp_audio_init(void);

/* Play a square-wave beep tone.
 * freq_hz: tone frequency (e.g. 880)
 * duration_ms: how long to play
 * volume: 0-100 percent (mapped to codec DAC volume) */
esp_err_t bsp_audio_beep(int freq_hz, int duration_ms, uint8_t volume);

/* Set the global alert volume (0-100). All subsequent bsp_audio_beep()
 * calls scale their volume parameter by this value. */
void bsp_audio_set_volume(uint8_t percent);

/* Play a short test beep at the current global volume. */
esp_err_t bsp_audio_test_beep(void);
