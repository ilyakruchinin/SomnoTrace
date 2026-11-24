# SomnoTrace - SomnoStage ML component
#
# Model-presence gate: when model.enc + key_a.inc + key_b.inc exist (injected
# by scripts/inject-model.sh for dev, or by the release pipeline for official
# builds), the real runtime is compiled and the blob is embedded into the app
# image.  Otherwise a stub is compiled and somno_ml_available() is false —
# every non-model feature keeps working for forks.

set(_sst_model "${CMAKE_CURRENT_LIST_DIR}/model.enc")
set(_sst_ka    "${CMAKE_CURRENT_LIST_DIR}/key_a.inc")
set(_sst_kb    "${CMAKE_CURRENT_LIST_DIR}/key_b.inc")

if(EXISTS "${_sst_model}" AND EXISTS "${_sst_ka}" AND EXISTS "${_sst_kb}")
    idf_component_register(
        SRCS "somno_ml.c" "somno_ml_keys.c" "somno_ml_worker.c"
             "somno_ml_model.c" "somno_ml_features.c" "somno_stage_csv.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
        REQUIRES mbedtls fatfs json esp_timer
        EMBED_FILES "model.enc")

    # The blob must decrypt before any flash-writing task runs (flash-cache
    # constraint): somno_ml_init() is called early in app_main.
    message(STATUS "somno_ml: model.enc present — building WITH SomnoStage model")
else()
    idf_component_register(
        SRCS "somno_ml_stub.c"
        INCLUDE_DIRS "include")
    message(STATUS "somno_ml: no model.enc — building stub (fork-safe)")
endif()

if(SOMNOTRACE_STACK_AUDIT)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage -Wframe-larger-than=3072)
endif()
