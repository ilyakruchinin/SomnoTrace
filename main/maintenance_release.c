#include "maintenance_release.h"
#include "cJSON.h"
#include "maintenance_model.h"
#include <stdio.h>
#include <string.h>
static void copy(char *out, size_t cap, const char *value)
{
    snprintf(out, cap, "%s", value ? value : "");
}
esp_err_t maintenance_release_parse(const char *json, size_t length, maintenance_release_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!json || !length)
        return ESP_ERR_INVALID_ARG;
    if (length > MAINTENANCE_RELEASE_RESPONSE_MAX)
        return ESP_ERR_INVALID_SIZE;
    bool quoted = false, escape = false;
    unsigned tokens = 0, depth = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = json[i];
        if (quoted) {
            if (escape)
                escape = false;
            else if (ch == '\\')
                escape = true;
            else if (ch == '"')
                quoted = false;
            continue;
        }
        if (ch == '"')
            quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 16 || ++tokens > 256)
                return ESP_ERR_INVALID_SIZE;
        } else if (ch == '}' || ch == ']') {
            if (!depth)
                return ESP_ERR_INVALID_ARG;
            depth--;
        } else if (ch == ',' || ch == ':') {
            if (++tokens > 256)
                return ESP_ERR_INVALID_SIZE;
        }
    }
    if (quoted || depth)
        return ESP_ERR_INVALID_ARG;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    while (end < json + length &&
           (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t' || *end == 0))
        end++;
    cJSON *release = cJSON_GetArrayItem(root, 0);
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(release, "tag_name"));
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) != 1 || !tag || !*tag ||
        strlen(tag) >= sizeof(out->version) || end != json + length) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    copy(out->version, sizeof(out->version), tag);
    const char *date =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(release, "published_at"));
    const char *notes = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(release, "body"));
    copy(out->published, sizeof(out->published), date ? date : "Publication time unavailable");
    copy(out->notes, sizeof(out->notes), notes ? notes : "No release notes supplied");
    out->notes_truncated = notes && strlen(notes) >= sizeof(out->notes);
    if (out->notes_truncated)
        copy(out->notes + sizeof(out->notes) - 22, 22, "\n[Notes truncated]");
    cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(release, "assets"))
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(asset, "name"));
        const char *url =
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url"));
        /* Asset naming is a candidate filter. The embedded board descriptor and
         * ESP-IDF image verification remain authoritative before boot selection. */
        if (name && url && strstr(name, "7b") && maintenance_sd_image_name_valid(name) &&
            strlen(url) < sizeof(out->url) && !strncmp(url, "https://", 8)) {
            copy(out->url, sizeof(out->url), url);
            out->compatible_asset = true;
            break;
        }
    }
    out->valid = true;
    cJSON_Delete(root);
    return ESP_OK;
}
