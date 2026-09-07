/* Bounded NVS delivery receipts. No topic, URL, message body or credentials. */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define ALERT_HISTORY_CAPACITY 512
#define ALERT_HISTORY_PAGE 8
#define ALERT_HISTORY_DAYS 30

typedef enum { ALERT_DELIVERY_PENDING, ALERT_DELIVERY_ACCEPTED,
               ALERT_DELIVERY_FAILED, ALERT_DELIVERY_SCREEN, ALERT_DELIVERY_CANCELLED } alert_delivery_t;
typedef struct {
    uint32_t id, epoch, ack_epoch;
    uint8_t result, test, escalated, acknowledged;
} alert_history_record_t;
typedef struct {
    alert_history_record_t rows[ALERT_HISTORY_PAGE];
    size_t count, total;
    bool time_valid;
    esp_err_t storage_result;
    esp_err_t write_error; /* sticky last failed write this boot; reads cannot hide it */
} alert_history_page_t;
typedef esp_err_t (*alert_history_executor_t)(esp_err_t (*fn)(void *), void *);
void alert_history_set_executor(alert_history_executor_t fn);
uint32_t alert_history_begin(bool test);
void alert_history_result(uint32_t id, alert_delivery_t result, bool escalated);
void alert_history_ack(uint32_t id);
void alert_history_read(size_t offset, alert_history_page_t *out);
/* A server acceptance receipt is not evidence a phone received or read it. */
void alert_history_verify(const char *server, const char *topic, bool accepted);
uint32_t alert_history_verified_at(const char *server, const char *topic);

void alert_history_cancel(uint32_t id); /* cancellation never acknowledges */

esp_err_t alert_history_storage_error(void);
