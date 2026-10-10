/*
 * SomnoTrace - SomnoStage stub (no model injected — fork/dev builds)
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

/* Compiled when components/somno_ml/model.enc is absent.  All hooks become
 * no-ops so forks keep every non-model feature without the proprietary blob. */

#include "somno_ml.h"

#include <string.h>

bool somno_ml_available(void) { return false; }

int somno_ml_init(void) { return -1; }

const char *somno_ml_model_semver(void) { return "-"; }

int somno_ml_enqueue(const somno_ml_job_t *job)
{
    (void)job;
    return -1;
}

int somno_ml_enqueue_ring(const char *recording_id)
{
    (void)recording_id;
    return -1;
}

int somno_ml_enqueue_as11(const char *day_dir, const char *file_prefix)
{
    (void)day_dir;
    (void)file_prefix;
    return -1;
}

int somno_ml_score_file(const somno_ml_job_t *job)
{
    (void)job;
    return -1;
}

int somno_ml_reconcile(void) { return 0; }

int somno_ml_reconcile_recent(int days)
{
    (void)days;
    return 0;
}

void somno_ml_live_state(somno_ml_live_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->stage = -1;
}

int somno_ml_sst_validate(const char *path, char *semver_out, size_t semver_len)
{
    (void)path;
    (void)semver_out;
    (void)semver_len;
    return -1;
}
