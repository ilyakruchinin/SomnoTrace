/* SPDX-License-Identifier: Apache-2.0
 * Extracted unchanged from Espressif ESP-IDF v5.5.1 httpd_txrx.c.
 * Copyright 2015-2025 Espressif Systems (Shanghai) CO LTD.
 * Reference fixture for SDK identity and allocation fault tests only.
 * https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_http_server/src/httpd_txrx.c
 */
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out)
{
    if (r == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // alloc async req
    httpd_req_t *async = malloc(sizeof(httpd_req_t));
    if (async == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(async, r, sizeof(httpd_req_t));

    // alloc async aux
    async->aux = malloc(sizeof(struct httpd_req_aux));
    if (async->aux == NULL) {
        free(async);
        return ESP_ERR_NO_MEM;
    }
    memcpy(async->aux, r->aux, sizeof(struct httpd_req_aux));

    // Copy response header block
    struct httpd_data *hd = (struct httpd_data *) r->handle;
    struct httpd_req_aux *async_aux = (struct httpd_req_aux *) async->aux;
    struct httpd_req_aux *r_aux = (struct httpd_req_aux *) r->aux;

    if (r_aux->scratch) {
        async_aux->scratch = malloc(r_aux->scratch_cur_size);
        if (async_aux->scratch == NULL) {
            free(async_aux);
            free(async);
            return ESP_ERR_NO_MEM;
        }
        memcpy(async_aux->scratch, r_aux->scratch, r_aux->scratch_cur_size);
    } else {
        async_aux->scratch = NULL;
    }

    async_aux->resp_hdrs = calloc(hd->config.max_resp_headers, sizeof(struct resp_hdr));
    if (async_aux->resp_hdrs == NULL) {
        free(async_aux);
        free(async);
        return ESP_ERR_NO_MEM;
    }
    memcpy(async_aux->resp_hdrs, r_aux->resp_hdrs, hd->config.max_resp_headers * sizeof(struct resp_hdr));

    // Prevent the main thread from reading the rest of the request after the handler returns.
    r_aux->remaining_len = 0;

    // mark socket as "in use"
    r_aux->sd->for_async_req = true;

    *out = async;

    return ESP_OK;
}

esp_err_t httpd_req_async_handler_complete(httpd_req_t *r)
{
    if (r == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct httpd_req_aux *ra = r->aux;
    ra->sd->for_async_req = false;
    free(ra->scratch);
    ra->scratch = NULL;
    ra->scratch_cur_size = 0;
    ra->scratch_size_limit = 0;
    free(ra->resp_hdrs);
    free(r->aux);
    free(r);

    return ESP_OK;
}
