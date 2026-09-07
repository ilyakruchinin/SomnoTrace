# ESP-IDF HTTP asynchronous request cleanup

The Docker runner applies a checked, idempotent one-line fix to the container's
ESP-IDF v5.5.1 `httpd_req_async_handler_begin`. If response-header allocation fails
after the request scratch buffer was copied, the SDK must free that scratch
buffer before freeing the auxiliary structure.

`scripts/idf-sdk-patches.py` checks both async functions against the unchanged
reference in this directory before applying the fix. An unexpected SDK version
fails the build and requires review. The host SDK is never changed. All commands
through `scripts/idf.sh`, including builds from linked worktrees, use the fix.
Direct builds outside this runner must apply the same fix before deployment.

`scripts/idf_async_allocation_test.py` injects failure at every allocation into
the patched reference functions and checks retained allocations and session
ownership. The build-time identity check ties that test to the SDK being used.

The reference functions are Espressif code under Apache-2.0, extracted from
[ESP-IDF v5.5.1](https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_http_server/src/httpd_txrx.c).
