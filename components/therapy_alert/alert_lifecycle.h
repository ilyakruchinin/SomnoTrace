#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Caller serializes access. Retain the latest cycle's ordered START/STOP/ACK
 * independently of the lossy wake queue. A newer START supersedes older cycles;
 * duplicate STARTs cannot erase an ACK or fill a queue ahead of STOP. */
typedef enum { ALERT_EDGE_NONE, ALERT_EDGE_ACTIVE, ALERT_EDGE_STOPPED,
               ALERT_EDGE_DISCONNECTED, ALERT_EDGE_UNCERTAIN } alert_edge_t;
typedef enum { ALERT_DO_START, ALERT_DO_STOP, ALERT_DO_DISCONNECT,
               ALERT_DO_ACK } alert_action_t;
typedef struct {
    uint64_t sequence, cycle, stop_sequence, ack_sequence, ack_cycle;
    alert_edge_t edge;
} alert_lifecycle_t;

static inline void alert_lifecycle_start(alert_lifecycle_t *s)
{
    if (s->edge == ALERT_EDGE_ACTIVE) return;
    ++s->cycle;
    ++s->sequence;
    s->stop_sequence = 0;
    s->edge = ALERT_EDGE_ACTIVE;
}
static inline void alert_lifecycle_stop(alert_lifecycle_t *s)
{
    if (s->edge != ALERT_EDGE_ACTIVE) return;
    s->stop_sequence = ++s->sequence;
    s->edge = ALERT_EDGE_STOPPED;
}
static inline void alert_lifecycle_disconnect(alert_lifecycle_t *s, bool uncertain)
{
    ++s->sequence;
    s->edge = uncertain || s->edge == ALERT_EDGE_UNCERTAIN
            ? ALERT_EDGE_UNCERTAIN : ALERT_EDGE_DISCONNECTED;
}
static inline void alert_lifecycle_ack(alert_lifecycle_t *s)
{
    s->ack_sequence = ++s->sequence;
    s->ack_cycle = s->cycle;
}
/* Maximum four actions; cursor belongs to the state owner, snapshot is copied
 * under the producer lock. ACK ordering within a cycle is preserved. */
static inline size_t alert_lifecycle_actions(const alert_lifecycle_t *s,
    alert_lifecycle_t *cursor, alert_action_t out[4])
{
    size_t n = 0;
    bool changed_cycle = s->cycle != cursor->cycle;
    bool ack = s->ack_sequence != cursor->ack_sequence &&
               s->ack_cycle == s->cycle;
    if (s->edge == ALERT_EDGE_DISCONNECTED || s->edge == ALERT_EDGE_UNCERTAIN) {
        if (changed_cycle) out[n++] = ALERT_DO_START;
        if (ack) out[n++] = ALERT_DO_ACK;
        if (s->sequence != cursor->sequence) out[n++] = ALERT_DO_DISCONNECT;
    } else {
        if (changed_cycle) out[n++] = ALERT_DO_START;
        bool stop = s->edge == ALERT_EDGE_STOPPED &&
                    (changed_cycle || cursor->edge != ALERT_EDGE_STOPPED);
        if (ack && stop && s->ack_sequence < s->stop_sequence)
            out[n++] = ALERT_DO_ACK;
        if (stop) out[n++] = ALERT_DO_STOP;
        if (ack && (!stop || s->ack_sequence > s->stop_sequence))
            out[n++] = ALERT_DO_ACK;
    }
    *cursor = *s;
    return n;
}
