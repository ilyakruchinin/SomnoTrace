/*
 * SomnoTrace - Pure first-run setup state transitions
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "first_run_setup_internal.h"

#include <stddef.h>

#define STEP_BIT(step) ((uint8_t)(1U << (unsigned)(step)))
#define ALL_STEP_BITS  ((uint8_t)((1U << FIRST_RUN_SETUP_STEP_COUNT) - 1U))

_Static_assert(FIRST_RUN_SETUP_STEP_COUNT <= 8,
               "first-run step masks need a wider stored schema");

static uint8_t resolved_mask(const first_run_setup_state_t *state)
{
    uint8_t mask = (uint8_t)(state->completed_mask | state->skipped_mask);
    if (state->continue_without_recording) {
        mask |= STEP_BIT(FIRST_RUN_SETUP_STEP_CARD);
    }
    return (uint8_t)(mask & ALL_STEP_BITS);
}

static bool is_named_step(first_run_setup_step_t step)
{
    return step >= FIRST_RUN_SETUP_STEP_WIFI &&
           step < FIRST_RUN_SETUP_STEP_COUNT;
}

static void advance_after(first_run_setup_state_t *state,
                          first_run_setup_step_t resolved_step)
{
    uint8_t resolved = resolved_mask(state);
    if (resolved == ALL_STEP_BITS) {
        state->current_step = FIRST_RUN_SETUP_STEP_FINISHED;
        return;
    }

    for (unsigned offset = 1; offset <= FIRST_RUN_SETUP_STEP_COUNT; offset++) {
        unsigned candidate = ((unsigned)resolved_step + offset) %
                             FIRST_RUN_SETUP_STEP_COUNT;
        if ((resolved & STEP_BIT(candidate)) == 0) {
            state->current_step = (first_run_setup_step_t)candidate;
            return;
        }
    }
}

void first_run_setup_model_defaults(first_run_setup_state_t *state)
{
    if (!state) return;
    *state = (first_run_setup_state_t) {
        .schema_version = FIRST_RUN_SETUP_SCHEMA_VERSION,
        .current_step = FIRST_RUN_SETUP_STEP_WIFI,
        .completed_mask = 0,
        .skipped_mask = 0,
        .continue_without_recording = false,
    };
}

const char *first_run_setup_step_name(first_run_setup_step_t step)
{
    static const char *const names[FIRST_RUN_SETUP_STEP_COUNT] = {
        [FIRST_RUN_SETUP_STEP_WIFI] = "Wi-Fi",
        [FIRST_RUN_SETUP_STEP_TIME] = "Time",
        [FIRST_RUN_SETUP_STEP_AIRSENSE] = "AirSense",
        [FIRST_RUN_SETUP_STEP_CARD] = "Card",
        [FIRST_RUN_SETUP_STEP_ALERTS] = "Alerts",
        [FIRST_RUN_SETUP_STEP_UPLOADS] = "Uploads",
    };
    return is_named_step(step) ? names[step] : NULL;
}

bool first_run_setup_step_can_skip(first_run_setup_step_t step)
{
    switch (step) {
        case FIRST_RUN_SETUP_STEP_WIFI:
        case FIRST_RUN_SETUP_STEP_TIME:
        case FIRST_RUN_SETUP_STEP_AIRSENSE:
        case FIRST_RUN_SETUP_STEP_ALERTS:
        case FIRST_RUN_SETUP_STEP_UPLOADS:
            return true;
        case FIRST_RUN_SETUP_STEP_CARD:
        case FIRST_RUN_SETUP_STEP_FINISHED:
        default:
            return false;
    }
}

bool first_run_setup_step_is_resolved(const first_run_setup_state_t *state,
                                      first_run_setup_step_t step)
{
    if (!state || !is_named_step(step)) return false;
    return (resolved_mask(state) & STEP_BIT(step)) != 0;
}

bool first_run_setup_is_finished(const first_run_setup_state_t *state)
{
    return state && resolved_mask(state) == ALL_STEP_BITS;
}

bool first_run_setup_model_is_valid(const first_run_setup_state_t *state)
{
    if (!state || state->schema_version != FIRST_RUN_SETUP_SCHEMA_VERSION) {
        return false;
    }
    if ((unsigned)state->current_step > FIRST_RUN_SETUP_STEP_FINISHED) {
        return false;
    }
    if (((state->completed_mask | state->skipped_mask) & ~ALL_STEP_BITS) != 0) {
        return false;
    }
    if ((state->completed_mask & state->skipped_mask) != 0) return false;
    if ((state->skipped_mask & STEP_BIT(FIRST_RUN_SETUP_STEP_CARD)) != 0) {
        return false;
    }
    if (state->continue_without_recording &&
        (state->completed_mask & STEP_BIT(FIRST_RUN_SETUP_STEP_CARD)) != 0) {
        return false;
    }
    if (state->current_step == FIRST_RUN_SETUP_STEP_FINISHED &&
        !first_run_setup_is_finished(state)) {
        return false;
    }
    return true;
}

bool first_run_setup_model_apply(first_run_setup_state_t *state,
                                 first_run_setup_step_t step,
                                 first_run_setup_update_t update)
{
    if (!first_run_setup_model_is_valid(state) || !is_named_step(step)) {
        return false;
    }

    uint8_t bit = STEP_BIT(step);
    switch (update) {
        case FIRST_RUN_SETUP_UPDATE_SELECT:
            state->current_step = step;
            return true;

        case FIRST_RUN_SETUP_UPDATE_COMPLETE:
            state->completed_mask |= bit;
            state->skipped_mask &= (uint8_t)~bit;
            if (step == FIRST_RUN_SETUP_STEP_CARD) {
                state->continue_without_recording = false;
            }
            break;

        case FIRST_RUN_SETUP_UPDATE_SKIP:
            if (!first_run_setup_step_can_skip(step)) return false;
            state->skipped_mask |= bit;
            state->completed_mask &= (uint8_t)~bit;
            break;

        case FIRST_RUN_SETUP_UPDATE_CONTINUE_WITHOUT_RECORDING:
            if (step != FIRST_RUN_SETUP_STEP_CARD) return false;
            state->completed_mask &= (uint8_t)~bit;
            state->skipped_mask &= (uint8_t)~bit;
            state->continue_without_recording = true;
            break;

        default:
            return false;
    }

    advance_after(state, step);
    return first_run_setup_model_is_valid(state);
}

static void complete_observed(first_run_setup_state_t *state,
                              first_run_setup_step_t step)
{
    uint8_t bit = STEP_BIT(step);
    state->completed_mask |= bit;
    state->skipped_mask &= (uint8_t)~bit;
    if (step == FIRST_RUN_SETUP_STEP_CARD) {
        state->continue_without_recording = false;
    }
}

bool first_run_setup_model_reconcile(
    first_run_setup_state_t *state,
    const first_run_setup_observed_t *observed,
    bool had_persisted_state)
{
    if (!first_run_setup_model_is_valid(state) || !observed) return false;

    const bool present[FIRST_RUN_SETUP_STEP_COUNT] = {
        [FIRST_RUN_SETUP_STEP_WIFI] = observed->wifi_configured,
        [FIRST_RUN_SETUP_STEP_TIME] = observed->time_configured,
        [FIRST_RUN_SETUP_STEP_AIRSENSE] = observed->airsense_paired,
        [FIRST_RUN_SETUP_STEP_CARD] = observed->card_present,
        [FIRST_RUN_SETUP_STEP_ALERTS] = observed->alerts_configured,
        [FIRST_RUN_SETUP_STEP_UPLOADS] = observed->uploads_configured,
    };

    for (unsigned step = 0; step < FIRST_RUN_SETUP_STEP_COUNT; step++) {
        if (present[step]) {
            complete_observed(state, (first_run_setup_step_t)step);
        }
    }

    /* Only a caller-affirmed legacy installation with no setup record gets
     * its absent services resolved automatically. A partial persisted wizard
     * always remains resumable and is never silently completed on upgrade. */
    if (!had_persisted_state && observed->established_installation) {
        for (unsigned step = 0; step < FIRST_RUN_SETUP_STEP_COUNT; step++) {
            if (present[step]) continue;
            if (step == FIRST_RUN_SETUP_STEP_CARD) {
                state->continue_without_recording = true;
            } else {
                state->skipped_mask |= STEP_BIT(step);
                state->completed_mask &= (uint8_t)~STEP_BIT(step);
            }
        }
    }

    if (first_run_setup_is_finished(state)) {
        state->current_step = FIRST_RUN_SETUP_STEP_FINISHED;
    } else if (state->current_step == FIRST_RUN_SETUP_STEP_FINISHED ||
               first_run_setup_step_is_resolved(state, state->current_step)) {
        first_run_setup_step_t anchor =
            state->current_step == FIRST_RUN_SETUP_STEP_FINISHED
                ? FIRST_RUN_SETUP_STEP_UPLOADS
                : state->current_step;
        advance_after(state, anchor);
    }

    return first_run_setup_model_is_valid(state);
}
