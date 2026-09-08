/*
 * SomnoTrace - AS11 timezone and noon-day mapping
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"

/* ────────────────────────────────────────────────────────────────────
 *  Why this module exists
 *
 *  The AS11 defines its reporting day as noon-to-noon *in the device's own
 *  timezone*, and stamps every Summary record's PeriodStart at exactly noon
 *  local to itself.  The ESP has its own, independently configured timezone.
 *
 *  Deriving the noon-day by applying ESP local time to an AS11 timestamp is
 *  therefore wrong whenever the two zones differ: if the AS11 is one hour
 *  east of the ESP, AS11 noon is 11:00 ESP-local, which reads as "before
 *  noon" and rolls every record back a whole day.  The visible damage is
 *  MaskOn/MaskOff values above 1440 (which OSCAR rejects as card
 *  corruption), summary data attached to the wrong day, and the current day
 *  never appearing covered — see issue #75.
 *
 *  A previous fix handled the few-minutes NTP-drift variant of this by
 *  labelling from the raw AS11 clock; that is not enough, because with a
 *  timezone difference the raw AS11 value itself is no longer at ESP-local
 *  noon.
 * ──────────────────────────────────────────────────────────────────── */

/* Seconds east of UTC.  Set from the AS11's reported TimeZoneOffset or
 * derived from a Summary PeriodStart; used to map AS11 timestamps onto the
 * device's own noon-day boundaries. */
typedef int32_t as11_offset_t;

/* Record the AS11's UTC offset.  `src` is a short provenance string for the
 * log ("settings", "period_start"). */
void as11_time_set_offset(as11_offset_t offset_s, const char *src);

/* Returns true and fills *out if an AS11 offset is known. */
bool as11_time_get_offset(as11_offset_t *out);

/* Parse the AS11's TimeZoneOffset ("-05:00") out of a settings JSON tree, as
 * returned by the Get RPC and stored in <session>_settings.json.  Accepts
 * either the bare {"SettingProfiles":...} form or the {"FlowGenerator":...}
 * wrapper.  On success the value is cached via as11_time_set_offset(). */
bool as11_time_offset_from_settings(const cJSON *settings, as11_offset_t *out);

/* Derive the AS11's UTC offset from a Summary PeriodStart.
 *
 * PeriodStart is noon in the AS11's zone, so the UTC time-of-day gives the
 * offset directly: offset = 12:00 - utc_time_of_day.  The result is snapped
 * to a 15-minute boundary and disambiguated against the ESP's own offset
 * (noon at +13:00 and at -11:00 share the same UTC time-of-day).  Returns
 * false if the timestamp does not look like a noon stamp, in which case the
 * caller should fall back rather than trust a derived value. */
bool as11_time_offset_from_period_start(int64_t period_start_ms,
                                        as11_offset_t *out);

/* Noon-day label ("YYYYMMDD") for an arbitrary AS11-clock timestamp, using
 * the AS11's own timezone and noon boundary.  Falls back to ESP local time
 * when no offset is known (legacy behaviour). */
void as11_time_noon_day(int64_t as11_epoch_ms, char *out, size_t out_len);

/* Noon-day label for a Summary PeriodStart.  Prefers an offset derived from
 * the timestamp itself, so a device that changed timezone or DST part-way
 * through the 30-day window still labels each record correctly. */
void as11_time_noon_day_for_period_start(int64_t period_start_ms,
                                         char *out, size_t out_len);

/* ESP-local noon (epoch seconds) of a "YYYYMMDD" label.
 *
 * Mask on/off minutes must be expressed relative to ESP-local noon even
 * though the day bucket comes from the AS11: consumers reconstruct absolute
 * session times as (local noon of the record's Date) + minutes, and compare
 * them against DATALOG filenames, which are written in ESP local time.
 * Returns 0 if the label cannot be parsed. */
int64_t as11_time_local_noon_epoch(const char *day_label);

/* Return the local epoch ms at which the noon-day period containing
 * as11_epoch_ms closes (i.e. the following noon at 12:00:00).
 * Handles DST spring-forward (23h) and fall-back (25h) days seamlessly
 * via civil calendar field normalization. */
int64_t as11_time_noon_period_end_ms(int64_t as11_epoch_ms);

/* Days since the Unix epoch for a "YYYYMMDD" label — the STR.edf Date
 * signal.  Returns -1 if the label cannot be parsed. */
int as11_time_day_number(const char *day_label);

/* Parse an ISO 8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SS.mmmZ") into epoch ms.
 * Returns -1 on parse failure. */
int64_t as11_time_parse_iso8601_ms(const char *iso_str);

/* Format date/time strings for EDF header from epoch ms.
 * date_out receives "DD.MM.YY", time_out receives "HH.MM.SS". */
void as11_time_format_edf_datetime(int64_t epoch_ms,
                                   char *date_out, int date_len,
                                   char *time_out, int time_len);

/* Format recording ID string for EDF header.
 * Format: "Startdate DD-MMM-YYYY X X X SRN=<srn> MID=<mid> VID=<vid>" */
void as11_time_format_recording_id(char *out, size_t out_len,
                                   int64_t epoch_ms,
                                   const char *srn, const char *mid, const char *vid);

/* Format a session timestamp prefix: "YYYYMMDD_HHMMSS" */
void as11_time_format_session_prefix(int64_t epoch_ms, char *out, size_t out_len);
