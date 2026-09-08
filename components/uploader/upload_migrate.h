/*
 * SomnoTrace - One-shot migration from the legacy day-level upload state
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

#include <stdbool.h>
#include "esp_err.h"

/* ══════════════════════════════════════════════════════════════════════
 *  DECOMMISSION AFTER v0.7.x
 *
 *  This file exists only to carry devices across the switch from day-level
 *  to per-group upload tracking.  It reads the old
 *  upload_state/upload_state.json, translates every day a backend had marked
 *  "ok" into per-group OK entries for the groups currently on the card, and
 *  renames the legacy file to *.migrated so it runs exactly once.
 *
 *  Without it, the first boot after upgrading would re-upload up to
 *  max_days of history to every backend.  With it, an already-synced device
 *  uploads nothing.
 *
 *  To remove: delete this file pair, drop upload_migrate.c from
 *  components/uploader/CMakeLists.txt, and delete the single call in
 *  uploader_init().  Nothing else references it.
 * ══════════════════════════════════════════════════════════════════════ */

/* Runs the migration if the legacy file is present.  Returns true if a
 * migration was performed.  Safe to call on every boot. */
bool upload_migrate_legacy_state(void);
