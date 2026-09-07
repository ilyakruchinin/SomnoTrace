/* Internal pure-state helpers shared by the service and host unit tests. */

#pragma once

#include "first_run_setup.h"

void first_run_setup_model_defaults(first_run_setup_state_t *state);
bool first_run_setup_model_is_valid(const first_run_setup_state_t *state);
bool first_run_setup_model_apply(first_run_setup_state_t *state,
                                 first_run_setup_step_t step,
                                 first_run_setup_update_t update);
bool first_run_setup_model_reconcile(
    first_run_setup_state_t *state,
    const first_run_setup_observed_t *observed,
    bool had_persisted_state);
