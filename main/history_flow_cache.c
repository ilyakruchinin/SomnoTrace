#include "history_flow_cache.h"
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>

#define FLOW_MAGIC 0x46505952U /* FPYR; disposable, versioned cache */
#define FLOW_VERSION 1U
static const uint32_t s_widths[] = {4, 16, 64};
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version, tier, channels, sample_bytes;
    uint16_t hz_x10, reserved;
    int64_t start_ms;
    uint32_t count, reserved2;
} source_header_t;
typedef struct __attribute__((packed)) {
    uint32_t magic, version;
    source_header_t source;
    uint64_t source_bytes;
    int64_t source_mtime;
    uint32_t source_crc;
    uint32_t offsets[3], counts[3];
    uint32_t crc;
} pyramid_header_t;
typedef struct __attribute__((packed)) {
    int16_t low, high;
    uint64_t valid; /* One bit per source second; gaps never become samples. */
    uint32_t crc;
} pyramid_record_t;

/* Independent forward buffers prevent level changes and raw edge fallbacks
 * from turning every logical record into a seek. Keep this per-call scratch
 * bounded (1 KiB of payload); never retain data across a source lease/epoch. */
#define FLOW_READ_RECORDS 16U
#define FLOW_READ_PAIRS 64U
typedef struct {
    uint32_t first, count;
    pyramid_record_t records[FLOW_READ_RECORDS];
} pyramid_read_buffer_t;
typedef struct {
    uint32_t first, count;
    int16_t pairs[FLOW_READ_PAIRS][2];
} raw_read_buffer_t;

static uint32_t crc_bytes(uint32_t crc, const void *bytes, size_t n)
{
    const uint8_t *p = bytes;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
static bool stopped(history_flow_cancel_fn cancel, void *context)
{ return cancel && cancel(context); }
static bool cache_path(const char *source, char *path, size_t size, bool temporary)
{
    int n = snprintf(path, size, "%s.fpy%s", source, temporary ? ".tmp" : "");
    return n > 0 && (size_t)n < size;
}
static bool source_info(FILE *file, const char *path, pyramid_header_t *h)
{
    struct stat st;
    memset(h, 0, sizeof(*h));
    if (stat(path, &st) || st.st_size < (off_t)sizeof(h->source) ||
        fread(&h->source, sizeof(h->source), 1, file) != 1) return false;
    source_header_t *s = &h->source;
    if (s->magic != 0x534e5442U || s->version != 2 || s->tier != 1 ||
        s->channels != 2 || s->sample_bytes != 2 || s->hz_x10 != 10 ||
        !s->count || s->count > 7U * 86400U || s->start_ms <= 0 ||
        st.st_size != (off_t)(sizeof(*s) + (uint64_t)s->count * 4U)) return false;
    h->magic = FLOW_MAGIC;
    h->version = FLOW_VERSION;
    h->source_bytes = st.st_size;
    h->source_mtime = st.st_mtime;
    uint32_t offset = sizeof(*h);
    for (size_t i = 0; i < 3; ++i) {
        h->offsets[i] = offset;
        h->counts[i] = (s->count + s_widths[i] - 1) / s_widths[i];
        offset += h->counts[i] * sizeof(pyramid_record_t);
    }
    return true;
}
static int source_crc(FILE *file, pyramid_header_t *h,
                      history_flow_cancel_fn cancel, void *context)
{
    if (fseek(file, 0, SEEK_SET)) return 1;
    uint8_t chunk[512];
    uint64_t left = h->source_bytes;
    uint32_t crc = 0;
    while (left) {
        if (stopped(cancel, context)) return -1;
        size_t n = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
        if (fread(chunk, 1, n, file) != n) return 1;
        crc = crc_bytes(crc, chunk, n);
        left -= n;
    }
    h->source_crc = crc;
    return 0;
}
static bool read_header(FILE *file, const pyramid_header_t *source,
                         pyramid_header_t *cached, bool verified)
{
    struct stat st;
    if (fread(cached, sizeof(*cached), 1, file) != 1 ||
        cached->crc != crc_bytes(0, cached, offsetof(pyramid_header_t, crc)) ||
        cached->magic != FLOW_MAGIC || cached->version != FLOW_VERSION ||
        memcmp(&cached->source, &source->source, sizeof(source->source)) ||
        cached->source_bytes != source->source_bytes ||
        cached->source_mtime != source->source_mtime ||
        (!verified && cached->source_crc != source->source_crc) ||
        memcmp(cached->offsets, source->offsets, sizeof(source->offsets)) ||
        memcmp(cached->counts, source->counts, sizeof(source->counts)) ||
        fstat(fileno(file), &st)) return false;
    return st.st_size == (off_t)(cached->offsets[2] +
        cached->counts[2] * sizeof(pyramid_record_t));
}
static bool valid_record(const pyramid_record_t *r)
{
    return r->crc == crc_bytes(0, r, offsetof(pyramid_record_t, crc)) &&
        (!r->valid || (r->low != INT16_MIN && r->high >= r->low));
}
int history_flow_cache_build(const char *source, history_flow_cancel_fn cancel,
                             void *context)
{
    char path[416], temp[416];
    if (!cache_path(source, path, sizeof(path), false) ||
        !cache_path(source, temp, sizeof(temp), true)) return 1;
    FILE *raw = fopen(source, "rb");
    if (!raw) return 1;
    pyramid_header_t h, old;
    int result = source_info(raw, source, &h) ? source_crc(raw, &h, cancel, context) : 1;
    FILE *file = NULL;
    if (result) goto done;
    file = fopen(path, "rb");
    if (file && read_header(file, &h, &old, false)) {
        bool intact = true;
        for (uint32_t i = 0; i < h.counts[0] + h.counts[1] + h.counts[2]; ++i) {
            if (stopped(cancel, context)) { result = -1; goto done; }
            pyramid_record_t r;
            if (fread(&r, sizeof(r), 1, file) != 1 || !valid_record(&r)) {
                intact = false;
                break;
            }
        }
        if (intact) goto done;
    }
    if (file) fclose(file);
    file = fopen(temp, "wb");
    if (!file) { result = 1; goto done; }
    /* No valid header exists until all three levels have been flushed. */
    pyramid_header_t empty = {0};
    if (fwrite(&empty, sizeof(empty), 1, file) != 1) { result = 1; goto done; }
    for (size_t level = 0; level < 3; ++level) {
        if (fseek(raw, sizeof(source_header_t), SEEK_SET)) { result = 1; goto done; }
        uint32_t width = s_widths[level];
        for (uint32_t first = 0; first < h.source.count; first += width) {
            if (stopped(cancel, context)) { result = -1; goto done; }
            int16_t pairs[64][2];
            uint32_t n = h.source.count - first;
            if (n > width) n = width;
            if (fread(pairs, sizeof(pairs[0]), n, raw) != n) { result = 1; goto done; }
            pyramid_record_t r = {.low = INT16_MAX, .high = INT16_MIN};
            for (uint32_t i = 0; i < n; ++i) {
                int16_t low = pairs[i][0], high = pairs[i][1];
                if (low == INT16_MIN && high == INT16_MIN) continue;
                if (low == INT16_MIN) low = high;
                if (high == INT16_MIN) high = low;
                if (low > high) { result = 1; goto done; }
                if (low < r.low) r.low = low;
                if (high > r.high) r.high = high;
                r.valid |= UINT64_C(1) << i;
            }
            r.crc = crc_bytes(0, &r, offsetof(pyramid_record_t, crc));
            if (fwrite(&r, sizeof(r), 1, file) != 1) { result = 1; goto done; }
        }
    }
    /* A source change during a non-device import cannot publish a stale cache. */
    pyramid_header_t after;
    if (fseek(raw, 0, SEEK_SET) || !source_info(raw, source, &after)) { result = 1; goto done; }
    result = source_crc(raw, &after, cancel, context);
    if (result || memcmp(&h, &after, sizeof(h))) { if (!result) result = 1; goto done; }
    h.crc = crc_bytes(0, &h, offsetof(pyramid_header_t, crc));
    if (fseek(file, 0, SEEK_SET) || fwrite(&h, sizeof(h), 1, file) != 1 ||
        fflush(file) || fsync(fileno(file))) { result = 1; goto done; }
    if (fclose(file)) { file = NULL; result = 1; goto done; }
    file = NULL;
    if (stopped(cancel, context)) { result = -1; goto done; }
    /* FAT may require unlinking the old derived file before rename. A crash
     * in that gap is just a cache miss; the source is never renamed/deleted. */
    if (rename(temp, path)) {
        if (errno != EEXIST || unlink(path) || rename(temp, path)) result = 1;
    }
done:
    if (file) fclose(file);
    fclose(raw);
    unlink(temp);
    return result;
}

int history_flow_cache_read(const char *source, int64_t start_ms, int64_t end_ms,
                            size_t bins, history_flow_bin_t *out,
                            bool source_verified, history_flow_cancel_fn cancel,
                            void *context, uint32_t *records_read)
{
    if (!out || !bins || bins > 480 || end_ms <= start_ms) return 1;
    char path[416];
    if (!cache_path(source, path, sizeof(path), false)) return 1;
    FILE *file = fopen(path, "rb");
    if (!file) return 1;
    FILE *raw = fopen(source, "rb");
    pyramid_header_t h, cached;
    int result = 1;
    if (!raw || !source_info(raw, source, &h)) goto done;
    if (!source_verified) {
        result = source_crc(raw, &h, cancel, context);
        if (result) goto done;
    }
    result = 1;
    if (!read_header(file, &h, &cached, source_verified)) goto done;
    for (size_t i = 0; i < bins; ++i)
        out[i] = (history_flow_bin_t){.low = INT16_MAX, .high = INT16_MIN};
    if (records_read) *records_read = 0;
    int64_t delta = start_ms - h.source.start_ms;
    uint32_t first = delta <= 0 ? 0 : (uint32_t)((delta + 999) / 1000);
    delta = end_ms - h.source.start_ms;
    uint32_t end = delta <= 0 ? 0 : (uint32_t)((delta + 999) / 1000);
    if (end > h.source.count) end = h.source.count;
    pyramid_read_buffer_t levels[3] = {0};
    raw_read_buffer_t edges = {0};
    while (first < end) {
        if (stopped(cancel, context)) { result = -1; goto done; }
        int64_t t = h.source.start_ms + (int64_t)first * 1000;
        size_t bin = (uint64_t)(t - start_ms) * bins / (uint64_t)(end_ms - start_ms);
        uint32_t n = 1;
        int16_t low = INT16_MIN, high = INT16_MIN;
        bool summarized = false;
        for (int level = 2; level >= 0; --level) {
            uint32_t width = s_widths[level];
            if (first % width || width > end - first) continue;
            int64_t last = t + (int64_t)(width - 1) * 1000;
            if ((uint64_t)(last - start_ms) * bins / (uint64_t)(end_ms - start_ms) != bin) continue;
            uint32_t index = first / width;
            pyramid_read_buffer_t *buffer = &levels[level];
            if (!buffer->count || index < buffer->first ||
                index - buffer->first >= buffer->count) {
                if (stopped(cancel, context)) { result = -1; goto done; }
                buffer->first = index;
                buffer->count = h.counts[level] - index;
                if (buffer->count > FLOW_READ_RECORDS) buffer->count = FLOW_READ_RECORDS;
                uint32_t offset = h.offsets[level] + index * sizeof(pyramid_record_t);
                if (fseek(file, offset, SEEK_SET) ||
                    fread(buffer->records, sizeof(pyramid_record_t), buffer->count, file) !=
                        buffer->count) { result = 1; goto done; }
            }
            pyramid_record_t r = buffer->records[index - buffer->first];
            if (!valid_record(&r)) { result = 1; goto done; }
            if (records_read) ++*records_read;
            uint64_t full = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
            if (r.valid && r.valid != full) continue; /* descend across gaps */
            n = width;
            if (r.valid) { low = r.low; high = r.high; }
            summarized = true;
            break;
        }
        if (!summarized) {
            if (!edges.count || first < edges.first || first - edges.first >= edges.count) {
                if (stopped(cancel, context)) { result = -1; goto done; }
                edges.first = first;
                edges.count = end - first;
                if (edges.count > FLOW_READ_PAIRS) edges.count = FLOW_READ_PAIRS;
                if (fseek(raw, sizeof(source_header_t) + (long)first * 4, SEEK_SET) ||
                    fread(edges.pairs, sizeof(edges.pairs[0]), edges.count, raw) != edges.count) {
                    result = 1; goto done;
                }
            }
            const int16_t *pair = edges.pairs[first - edges.first];
            if (records_read) ++*records_read;
            low = pair[0]; high = pair[1];
            if (low == INT16_MIN) low = high;
            if (high == INT16_MIN) high = low;
        }
        if (low != INT16_MIN && bin < bins) {
            if (low < out[bin].low) out[bin].low = low;
            if (high > out[bin].high) out[bin].high = high;
            out[bin].count += n;
        }
        first += n;
    }
    result = 0;
done:
    if (raw) fclose(raw);
    fclose(file);
    return result;
}
