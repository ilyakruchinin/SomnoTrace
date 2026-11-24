/*
 * SomnoTrace - Air 10 daily EDF settings field mapping
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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
#include <stdint.h>

typedef enum {
    AS10_UNIT_ENUM, AS10_UNIT_PRESSURE, AS10_UNIT_SECONDS,
    AS10_UNIT_MILLISECONDS, AS10_UNIT_MINUTES, AS10_UNIT_CELSIUS,
} as10_setting_unit_t;

typedef struct {
    const char *label;
    uint8_t str_index;
    as10_setting_unit_t unit;
} as10_setting_field_t;

/* Air 10 uses shared bilevel timing fields and S.AS.* AutoSet labels.
 * Preserve the source enum value rather than guessing an AS11 translation. */
static const as10_setting_field_t g_as10_setting_fields[] = {
    {"Mode", 5, AS10_UNIT_ENUM},
    {"S.C.StartPress", 6, AS10_UNIT_PRESSURE},
    {"S.C.Press", 7, AS10_UNIT_PRESSURE},
    {"S.AS.StartPress", 8, AS10_UNIT_PRESSURE},
    {"S.AS.MaxPress", 9, AS10_UNIT_PRESSURE},
    {"S.AS.MinPress", 10, AS10_UNIT_PRESSURE},
    {"S.VA.StartPress", 14, AS10_UNIT_PRESSURE},
    {"S.VA.MaxIPAP", 15, AS10_UNIT_PRESSURE},
    {"S.VA.MinEPAP", 16, AS10_UNIT_PRESSURE},
    {"S.VA.PS", 17, AS10_UNIT_PRESSURE},
    {"S.BL.StartPress", 22, AS10_UNIT_PRESSURE},
    {"S.BL.IPAP", 23, AS10_UNIT_PRESSURE},
    {"S.BL.EPAP", 24, AS10_UNIT_PRESSURE},
    {"S.EasyBreathe", 25, AS10_UNIT_ENUM},
    {"S.TiMax", 27, AS10_UNIT_SECONDS},
    {"S.TiMin", 28, AS10_UNIT_SECONDS},
    {"S.RiseEnable", 29, AS10_UNIT_ENUM},
    {"S.RiseTime", 30, AS10_UNIT_MILLISECONDS},
    {"S.Trigger", 31, AS10_UNIT_ENUM},
    {"S.Cycle", 32, AS10_UNIT_ENUM},
    {"S.AV.StartPress", 50, AS10_UNIT_PRESSURE},
    {"S.AV.EPAP", 51, AS10_UNIT_PRESSURE},
    {"S.AV.MaxPS", 52, AS10_UNIT_PRESSURE},
    {"S.AV.MinPS", 53, AS10_UNIT_PRESSURE},
    {"S.AA.StartPress", 54, AS10_UNIT_PRESSURE},
    {"S.AA.MaxEPAP", 55, AS10_UNIT_PRESSURE},
    {"S.AA.MinEPAP", 56, AS10_UNIT_PRESSURE},
    {"S.AA.MaxPS", 57, AS10_UNIT_PRESSURE},
    {"S.AA.MinPS", 58, AS10_UNIT_PRESSURE},
    {"S.AS.Comfort", 59, AS10_UNIT_ENUM},
    {"S.RampEnable", 60, AS10_UNIT_ENUM},
    {"S.RampTime", 61, AS10_UNIT_MINUTES},
    {"S.EPR.ClinEnable", 62, AS10_UNIT_ENUM},
    {"S.EPR.EPREnable", 63, AS10_UNIT_ENUM},
    {"S.EPR.Level", 64, AS10_UNIT_PRESSURE},
    {"S.EPR.EPRType", 65, AS10_UNIT_ENUM},
    {"S.SmartStart", 66, AS10_UNIT_ENUM},
    {"S.PtAccess", 67, AS10_UNIT_ENUM},
    {"S.ABFilter", 68, AS10_UNIT_ENUM},
    {"S.Mask", 69, AS10_UNIT_ENUM},
    {"S.Tube", 70, AS10_UNIT_ENUM},
    {"S.ClimateControl", 71, AS10_UNIT_ENUM},
    {"S.HumEnable", 72, AS10_UNIT_ENUM},
    {"S.HumLevel", 73, AS10_UNIT_ENUM},
    {"S.TempEnable", 74, AS10_UNIT_ENUM},
    {"S.Temp", 75, AS10_UNIT_CELSIUS},
};
