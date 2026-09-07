#include "alert_history.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
static alert_history_executor_t s_exec;
static int s_write_error;
static esp_err_t remember_write(esp_err_t result)
{
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
        __atomic_store_n(&s_write_error, result, __ATOMIC_RELEASE);
    return result;
}
esp_err_t alert_history_storage_error(void)
{ return __atomic_load_n(&s_write_error, __ATOMIC_ACQUIRE); }
void alert_history_set_executor(alert_history_executor_t fn) { s_exec = fn; }
typedef struct { uint32_t id; bool test, escalated; alert_delivery_t result;
                 int action; size_t offset; alert_history_page_t *page; } request_t;
static uint32_t epoch(void) { time_t n = time(NULL); return n > 1609459200 ? (uint32_t)n : 0; }
static bool retained(const alert_history_record_t *r, uint32_t now)
{ return r->id && (!now || !r->epoch || r->epoch > now || now - r->epoch <= ALERT_HISTORY_DAYS * 86400U); }
static esp_err_t operate(void *arg)
{
    request_t *q = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open("alert_history", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint32_t seq = 0; nvs_get_u32(h, "seq", &seq);
    uint32_t now = epoch();
    if (q->action == 3) {
        bool pruned = false;
        q->page->time_valid = now != 0;
        for (uint32_t n = 0; n < ALERT_HISTORY_CAPACITY && n < seq; n++) {
            uint32_t id = seq - n;
            char key[16]; snprintf(key, sizeof(key), "r%03u", (unsigned)(id % ALERT_HISTORY_CAPACITY));
            alert_history_record_t r = {0}; size_t len = sizeof(r);
            if (nvs_get_blob(h, key, &r, &len) != ESP_OK || len != sizeof(r) || r.id != id) continue;
            if (!retained(&r, now)) {
                esp_err_t removed = nvs_erase_key(h,key);
                if (removed != ESP_OK) remember_write(removed);
                else pruned = true;
                continue;
            }
            size_t index = q->page->total++;
            if (index >= q->offset && q->page->count < ALERT_HISTORY_PAGE)
                q->page->rows[q->page->count++] = r;
        }
        if (pruned) remember_write(nvs_commit(h));
    } else {
        if (q->action == 0) q->id = seq + 1;
        char key[16]; snprintf(key, sizeof(key), "r%03u", (unsigned)(q->id % ALERT_HISTORY_CAPACITY));
        alert_history_record_t r = {0}; size_t len = sizeof(r);
        if (q->action == 0) { r.id = q->id; r.epoch = now; r.test = q->test; }
        else if (nvs_get_blob(h, key, &r, &len) != ESP_OK || len != sizeof(r) || r.id != q->id) { nvs_close(h); return ESP_ERR_NOT_FOUND; }
        if (q->action == 1) { r.result = q->result; r.escalated |= q->escalated; }
        if (q->action == 4 && r.result == ALERT_DELIVERY_PENDING) r.result = ALERT_DELIVERY_CANCELLED;
        if (q->action == 2) { r.acknowledged = true; r.ack_epoch = now; }
        err = nvs_set_blob(h, key, &r, sizeof(r));
        if (err == ESP_OK && q->action == 0) err = nvs_set_u32(h, "seq", q->id);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h); return err;
}
static esp_err_t execute(request_t *q) { esp_err_t r = s_exec ? s_exec(operate, q) : operate(q); return q->action == 3 ? r : remember_write(r); }
uint32_t alert_history_begin(bool test)
{ request_t q = { .test = test }; return execute(&q) == ESP_OK ? q.id : 0; }
void alert_history_result(uint32_t id, alert_delivery_t result, bool escalated)
{ if (id) { request_t q = { .action = 1, .id = id, .result = result, .escalated = escalated }; execute(&q); } }
void alert_history_ack(uint32_t id)
{ if (id) { request_t q = { .action = 2, .id = id }; execute(&q); } }
void alert_history_read(size_t offset, alert_history_page_t *out)
{ memset(out, 0, sizeof(*out)); request_t q = { .action = 3, .offset = offset, .page = out }; out->storage_result = execute(&q); out->write_error = alert_history_storage_error(); }

typedef struct { uint8_t hash[32]; uint32_t at; } verification_t;
typedef struct { verification_t receipt; bool write; } verify_request_t;
static esp_err_t verification(void *arg)
{
    verify_request_t *q = arg; nvs_handle_t h;
    esp_err_t e = nvs_open("alert_history", q->write ? NVS_READWRITE : NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    if (q->write) { e = nvs_set_blob(h, "verified", &q->receipt, sizeof(q->receipt)); if (e == ESP_OK) e = nvs_commit(h); }
    else { size_t len = sizeof(q->receipt); e = nvs_get_blob(h, "verified", &q->receipt, &len); if (len != sizeof(q->receipt)) e = ESP_ERR_INVALID_SIZE; }
    nvs_close(h); return e;
}
static void fingerprint(const char *server, const char *topic, uint8_t out[32])
{ char buf[132]; snprintf(buf, sizeof(buf), "%s\n%s", server, topic); mbedtls_sha256((unsigned char *)buf, strlen(buf), out, 0); memset(buf, 0, sizeof(buf)); }
void alert_history_verify(const char *server, const char *topic, bool accepted)
{ verify_request_t q = { .write = true }; fingerprint(server, topic, q.receipt.hash); q.receipt.at = accepted ? epoch() : 0; remember_write(s_exec ? s_exec(verification, &q) : verification(&q)); }
uint32_t alert_history_verified_at(const char *server, const char *topic)
{ uint8_t hash[32]; fingerprint(server, topic, hash); verify_request_t q = {0}; esp_err_t e = s_exec ? s_exec(verification, &q) : verification(&q); return e == ESP_OK && !memcmp(hash, q.receipt.hash, 32) ? q.receipt.at : 0; }

void alert_history_cancel(uint32_t id)
{ if (id) { request_t q = { .action = 4, .id = id }; execute(&q); } }
