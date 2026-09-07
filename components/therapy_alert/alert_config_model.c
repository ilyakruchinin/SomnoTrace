#include "therapy_alert.h"
#include <string.h>
esp_err_t therapy_alert_validate_config(const therapy_alert_config_t *cfg)
{
    if (!cfg || cfg->win_start > 1439 || cfg->win_end > 1439 ||
        cfg->delay1 > 180 || cfg->delay2 > 180 || cfg->ntfy_prio < 1 || cfg->ntfy_prio > 5 ||
        !memchr(cfg->ntfy_srv, 0, sizeof(cfg->ntfy_srv)) ||
        !memchr(cfg->ntfy_topic, 0, sizeof(cfg->ntfy_topic))) return ESP_ERR_INVALID_ARG;
    if (strncmp(cfg->ntfy_srv, "https://", 8) && strncmp(cfg->ntfy_srv, "http://", 7)) return ESP_ERR_INVALID_ARG;
    const char *host = strstr(cfg->ntfy_srv, "://") + 3;
    if (!*host || strpbrk(host, " \r\n?#@")) return ESP_ERR_INVALID_ARG;
    for (const char *p = cfg->ntfy_topic; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' )) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
