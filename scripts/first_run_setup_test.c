/* Host unit tests for first-run setup state transitions and reconciliation. */

#include "first_run_setup.h"
#include "first_run_setup_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BIT(step) ((uint8_t)(1U << (unsigned)(step)))

static void test_step_contract(void)
{
    static const char *const expected[] = {
        "Wi-Fi", "Time", "AirSense", "Card", "Alerts", "Uploads",
    };
    assert(FIRST_RUN_SETUP_STEP_COUNT == 6);
    for (unsigned i = 0; i < FIRST_RUN_SETUP_STEP_COUNT; i++) {
        assert(strcmp(first_run_setup_step_name((first_run_setup_step_t)i),
                      expected[i]) == 0);
    }
    assert(first_run_setup_step_name(FIRST_RUN_SETUP_STEP_FINISHED) == NULL);
    assert(!first_run_setup_step_can_skip(FIRST_RUN_SETUP_STEP_CARD));
    assert(first_run_setup_step_can_skip(FIRST_RUN_SETUP_STEP_WIFI));
    assert(first_run_setup_step_can_skip(FIRST_RUN_SETUP_STEP_TIME));
    assert(first_run_setup_step_can_skip(FIRST_RUN_SETUP_STEP_AIRSENSE));
    assert(first_run_setup_step_can_skip(FIRST_RUN_SETUP_STEP_ALERTS));
    assert(first_run_setup_step_can_skip(FIRST_RUN_SETUP_STEP_UPLOADS));
}

static void test_explicit_transitions(void)
{
    first_run_setup_state_t state;
    first_run_setup_model_defaults(&state);
    assert(first_run_setup_model_is_valid(&state));
    assert(state.current_step == FIRST_RUN_SETUP_STEP_WIFI);
    assert(!first_run_setup_is_finished(&state));

    first_run_setup_state_t unchanged = state;
    assert(!first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_CARD, FIRST_RUN_SETUP_UPDATE_SKIP));
    assert(memcmp(&state, &unchanged, sizeof(state)) == 0);

    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_WIFI, FIRST_RUN_SETUP_UPDATE_SKIP));
    assert(state.current_step == FIRST_RUN_SETUP_STEP_TIME);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_WIFI)) != 0);

    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_TIME, FIRST_RUN_SETUP_UPDATE_SKIP));
    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_AIRSENSE, FIRST_RUN_SETUP_UPDATE_SKIP));
    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_CARD,
        FIRST_RUN_SETUP_UPDATE_CONTINUE_WITHOUT_RECORDING));
    assert(state.continue_without_recording);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) == 0);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) == 0);
    assert(first_run_setup_step_is_resolved(
        &state, FIRST_RUN_SETUP_STEP_CARD));
    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_ALERTS, FIRST_RUN_SETUP_UPDATE_SKIP));
    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_UPLOADS, FIRST_RUN_SETUP_UPDATE_SKIP));
    assert(first_run_setup_is_finished(&state));
    assert(state.current_step == FIRST_RUN_SETUP_STEP_FINISHED);

    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_CARD, FIRST_RUN_SETUP_UPDATE_COMPLETE));
    assert(!state.continue_without_recording);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) != 0);
}

static void test_fresh_reconciliation(void)
{
    first_run_setup_state_t state;
    first_run_setup_model_defaults(&state);
    first_run_setup_observed_t observed = {
        .wifi_configured = true,
        .card_present = true,
    };
    assert(first_run_setup_model_reconcile(&state, &observed, false));
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_WIFI)) != 0);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) != 0);
    assert(state.skipped_mask == 0);
    assert(!state.continue_without_recording);
    assert(state.current_step == FIRST_RUN_SETUP_STEP_TIME);
    assert(!first_run_setup_is_finished(&state));
}

static void test_established_installation_reconciliation(void)
{
    first_run_setup_state_t state;
    first_run_setup_model_defaults(&state);
    first_run_setup_observed_t observed = {
        .established_installation = true,
        .wifi_configured = true,
        .airsense_paired = true,
        .card_present = true,
    };
    assert(first_run_setup_model_reconcile(&state, &observed, false));
    assert(first_run_setup_is_finished(&state));
    assert(state.current_step == FIRST_RUN_SETUP_STEP_FINISHED);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_WIFI)) != 0);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_AIRSENSE)) != 0);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) != 0);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_TIME)) != 0);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_ALERTS)) != 0);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_UPLOADS)) != 0);
}

static void test_established_installation_without_card(void)
{
    first_run_setup_state_t state;
    first_run_setup_model_defaults(&state);
    first_run_setup_observed_t observed = {
        .established_installation = true,
    };
    assert(first_run_setup_model_reconcile(&state, &observed, false));
    assert(first_run_setup_is_finished(&state));
    assert(state.continue_without_recording);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) == 0);
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) == 0);
}

static void test_persisted_progress_is_not_ambushed(void)
{
    first_run_setup_state_t state;
    first_run_setup_model_defaults(&state);
    assert(first_run_setup_model_apply(
        &state, FIRST_RUN_SETUP_STEP_WIFI, FIRST_RUN_SETUP_UPDATE_SKIP));

    first_run_setup_observed_t observed = {
        .established_installation = true,
        .card_present = true,
    };
    assert(first_run_setup_model_reconcile(&state, &observed, true));
    assert((state.completed_mask & BIT(FIRST_RUN_SETUP_STEP_CARD)) != 0);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_WIFI)) != 0);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_TIME)) == 0);
    assert((state.skipped_mask & BIT(FIRST_RUN_SETUP_STEP_AIRSENSE)) == 0);
    assert(state.current_step == FIRST_RUN_SETUP_STEP_TIME);
    assert(!first_run_setup_is_finished(&state));
}

static void test_corrupt_state_rejected(void)
{
    first_run_setup_state_t state;
    first_run_setup_model_defaults(&state);
    state.skipped_mask = BIT(FIRST_RUN_SETUP_STEP_CARD);
    assert(!first_run_setup_model_is_valid(&state));

    first_run_setup_model_defaults(&state);
    state.current_step = FIRST_RUN_SETUP_STEP_FINISHED;
    assert(!first_run_setup_model_is_valid(&state));

    first_run_setup_model_defaults(&state);
    state.schema_version++;
    assert(!first_run_setup_model_is_valid(&state));
}

int main(void)
{
    test_step_contract();
    test_explicit_transitions();
    test_fresh_reconciliation();
    test_established_installation_reconciliation();
    test_established_installation_without_card();
    test_persisted_progress_is_not_ambushed();
    test_corrupt_state_rejected();
    puts("first-run setup state tests passed");
    return 0;
}
