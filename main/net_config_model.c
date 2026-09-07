/* SomnoTrace Rev C: bounded network configuration validation. */
#include "net_provision.h"
#include <string.h>
#include <stdint.h>

static bool ipv4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            if (++digits > 3 || n > 255) return false;
        }
        if (!digits || (i < 3 ? *s++ != '.' : *s != 0)) return false;
        v = (v << 8) | n;
    }
    *out = v;
    return true;
}

esp_err_t netprov_validate_config(const struct netprov_config *cfg)
{
    if (!cfg || !memchr(cfg->hostname, 0, sizeof(cfg->hostname))) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; ++i) {
        const struct netprov_wifi_cred *w = &cfg->wifi[i];
        if (!memchr(w->ssid, 0, sizeof(w->ssid)) || !memchr(w->pass, 0, sizeof(w->pass))) return ESP_ERR_INVALID_ARG;
        size_t n = strlen(w->pass);
        if (w->ssid[0] && n && (n < 8 || n > 64)) return ESP_ERR_INVALID_ARG;
        if (n == 64) for (size_t j = 0; j < n; j++) {
            char c=w->pass[j];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; w->ssid[0] && j < i; ++j)
            if (!strcmp(w->ssid, cfg->wifi[j].ssid)) return ESP_ERR_INVALID_ARG;
        if (!w->ipv4.manual) continue;
        uint32_t ip, mask, gw, dns;
        if (!memchr(w->ipv4.address, 0, 16) || !memchr(w->ipv4.netmask, 0, 16) ||
            !memchr(w->ipv4.gateway, 0, 16) || !memchr(w->ipv4.dns, 0, 16) ||
            !ipv4(w->ipv4.address, &ip) || !ipv4(w->ipv4.netmask, &mask) ||
            !ipv4(w->ipv4.gateway, &gw) || !ipv4(w->ipv4.dns, &dns)) return ESP_ERR_INVALID_ARG;
        uint32_t host = ~mask;
        if (!mask || !host || (host & (host + 1)) || !ip || !gw || !dns ||
            (ip & mask) != (gw & mask) || !(ip & host) || (ip & host) == host ||
            (ip >> 24) >= 224 || (dns >> 24) >= 224) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
