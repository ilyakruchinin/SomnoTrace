#include "maintenance_model.h"
#include <string.h>
void maintenance_hold_reset(maintenance_hold_t *h)
{
    if (h)
        memset(h, 0, sizeof(*h));
}
void maintenance_hold_press(maintenance_hold_t *h, uint32_t now)
{
    if (h && !h->pressed) {
        h->pressed = true;
        h->started_ms = now;
        h->observed_ms = now;
    }
}
uint32_t maintenance_hold_elapsed(const maintenance_hold_t *h, uint32_t now)
{
    return h && h->pressed ? (uint32_t)(now - h->started_ms) : 0;
}
bool maintenance_hold_ready(maintenance_hold_t *h, uint32_t now, bool allowed)
{
    if (!allowed) {
        maintenance_hold_reset(h);
        return false;
    }
    if (!h || !h->pressed)
        return false;
    if ((uint32_t)(now - h->observed_ms) > MAINTENANCE_HOLD_OBSERVATION_MS) {
        maintenance_hold_reset(h);
        return false;
    }
    h->observed_ms = now;
    if (maintenance_hold_elapsed(h, now) < MAINTENANCE_HOLD_MS) return false;
    maintenance_hold_reset(h);
    return true;
}
bool maintenance_day_valid(const char *s)
{
    if (!s || strlen(s) != 8)
        return false;
    for (size_t i = 0; i < 8; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    unsigned year =
        (unsigned)(s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + s[3] - '0';
    unsigned month = (s[4] - '0') * 10 + s[5] - '0', day = (s[6] - '0') * 10 + s[7] - '0';
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2000 || month < 1 || month > 12)
        return false;
    unsigned limit =
        days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day >= 1 && day <= limit;
}
bool maintenance_sd_image_name_valid(const char *s)
{
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n < 16 || n >= MAINTENANCE_NAME_MAX || strncmp(s, "somnotrace-", 11) ||
        strcmp(s + n - 4, ".bin"))
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.' ||
              s[i] == '+'))
            return false;
    return strstr(s, "..") == NULL;
}
bool maintenance_generated_edf_name(const char *s, bool root)
{
    if (!s || strchr(s, '/') || strchr(s, '\\'))
        return false;
    if (root)
        return strcmp(s, "STR.edf") == 0;
    static const char *suffix[] = {"_BRP.edf", "_PLD.edf", "_SA2.edf", "_EVE.edf", "_CSL.edf"};
    size_t n = strlen(s);
    /* EDF generation uses session_timestamp(): YYYYMMDD_HHMMSS. A personal
     * file with an EDF suffix is not evidence that SomnoTrace generated it. */
    if (n != 23 || s[8] != '_')
        return false;
    char day[9];
    memcpy(day, s, 8);
    day[8] = 0;
    if (!maintenance_day_valid(day))
        return false;
    for (size_t i = 9; i < 15; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    if ((s[9] - '0') * 10 + s[10] - '0' > 23 || (s[11] - '0') * 10 + s[12] - '0' > 59 ||
        (s[13] - '0') * 10 + s[14] - '0' > 59)
        return false;
    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); i++) {
        size_t k = strlen(suffix[i]);
        if (n > k && strcmp(s + n - k, suffix[i]) == 0)
            return true;
    }
    return false;
}
uint64_t maintenance_estimated_nights(uint64_t free_bytes, uint64_t largest, size_t samples)
{
    return samples && largest ? free_bytes / largest : 0;
}
