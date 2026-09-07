#!/usr/bin/env bash
# Run the release parser with the same cJSON source provided by the pinned IDF.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [[ -z "${IDF_PATH:-}" ]]; then
    exec docker run --rm --entrypoint bash \
        -v "${PROJECT_DIR}:/project:ro" -w /project \
        "espressif/idf:${IDF_TAG:-v5.5.1}" scripts/test-maintenance-release.sh
fi
CJSON_DIR="${IDF_PATH}/components/json/cJSON"
TEST_DIR="$(mktemp -d /tmp/somno-release-test.XXXXXX)"
trap 'rm -rf "${TEST_DIR}"' EXIT
cd "${PROJECT_DIR}"
cc -std=c11 -Wall -Wextra -Werror -DMAINTENANCE_HOST_TEST \
    -I "${CJSON_DIR}" -I scripts/test_include -I main \
    scripts/maintenance_release_test.c main/maintenance_release.c \
    main/maintenance_model.c "${CJSON_DIR}/cJSON.c" -lm \
    -o "${TEST_DIR}/maintenance_release_test"
"${TEST_DIR}/maintenance_release_test"
