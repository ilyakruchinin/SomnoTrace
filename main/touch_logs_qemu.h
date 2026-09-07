#pragma once

#include "log_stream.h"

/* QEMU-only service simulation. No card, network or patient files are used. */
void touch_logs_qemu_tick(void);
void touch_logs_qemu_disconnect(void);
bool touch_logs_qemu_disconnected(void);
void touch_logs_qemu_reconnect(void);
esp_err_t touch_logs_qemu_save(const log_stream_retained_filter_t *filter,
    size_t *saved, log_stream_retained_progress_fn progress, void *ctx);
