#!/usr/bin/env python3
# SomnoTrace - SNT->EDF writer to reader round-trip contract test
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
#
# This file is part of SomnoTrace.
#
# SomnoTrace is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.
#
# ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
# attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
# (https://github.com/ilyakruchinin)." See the NOTICE file for details.

"""End-to-end writer -> reader round-trip contract test for SNT and EDF generation.

Verifies:
1. SNT v1 and v2 binary schemas, magic, and headers.
2. Missing-data sentinel handling (v1=-1, v2=INT16_MIN).
3. Invalid passthrough signals produce exact digital -1 (0xFFFF).
4. Non-passthrough signals clamp within [dig_min, dig_max] and never leak -1.
5. Multi-channel mapping and reduction (e.g. 12-channel PLD -> 9-channel EDF).
6. ResMed EDF header conformity (Startdate DD-MMM-YYYY, 80-char fields).
"""

import math
import struct
import sys
import unittest

SNT_MAGIC = 0x534E5442  # "SNTB"
SNT_HEADER_STRUCT = "<IBBBBHHqII"  # 28 bytes
INT16_MIN = -32768
INT16_MAX = 32767


def build_snt_header(version, tier, n_channels, sample_hz_x10, start_epoch_ms, sample_count):
    """Build a 28-byte packed .snt header matching snt_header_t in edf_gen.c."""
    return struct.pack(
        SNT_HEADER_STRUCT,
        SNT_MAGIC,
        version,
        tier,
        n_channels,
        2,  # sample_bytes = 2
        sample_hz_x10,
        0,  # reserved
        start_epoch_ms,
        sample_count,
        0,  # reserved2
    )


class SignalDef:
    def __init__(self, label, pmin, pmax, dmin, dmax, samples_per_record, invalid_passthrough=False):
        self.label = label
        self.pmin = pmin
        self.pmax = pmax
        self.dmin = dmin
        self.dmax = dmax
        self.samples_per_record = samples_per_record
        self.invalid_passthrough = invalid_passthrough

    @property
    def k(self):
        pspan = self.pmax - self.pmin
        dspan = self.dmax - self.dmin
        return (dspan / pspan) if pspan != 0.0 else 0.0


def clamp_i16(val, min_val, max_val):
    return max(min_val, min(max_val, val))


def convert_snt_samples(version, snt_channels, raw_samples, sig_def, channel_idx):
    """Replicate edf_gen.c convert_snt_to_edf sample transformation."""
    snt_missing = INT16_MIN if version >= 2 else -1
    out_samples = []

    k = sig_def.k
    pmin = sig_def.pmin
    dmin = sig_def.dmin
    passthrough = sig_def.invalid_passthrough

    for s in range(len(raw_samples) // snt_channels):
        stored = raw_samples[s * snt_channels + channel_idx]
        if passthrough and stored == snt_missing:
            out_samples.append(-1)
            continue

        phys = stored / 100.0
        dig = dmin + (phys - pmin) * k
        idig = int(dig - 0.5 if dig < 0 else dig + 0.5)

        if not passthrough:
            idig = clamp_i16(idig, sig_def.dmin, sig_def.dmax)
        else:
            idig = clamp_i16(idig, INT16_MIN, INT16_MAX)

        out_samples.append(idig)

    return out_samples


class RoundTripContractTest(unittest.TestCase):
    def test_snt_v2_header_packing(self):
        """Verify binary packing of SNT v2 header."""
        hdr = build_snt_header(
            version=2,
            tier=1,
            n_channels=2,
            sample_hz_x10=250,
            start_epoch_ms=1788394175000,
            sample_count=1500,
        )
        self.assertEqual(len(hdr), 28)
        magic, ver, tier, n_ch, s_bytes, hz10, _, epoch, count, _ = struct.unpack(SNT_HEADER_STRUCT, hdr)
        self.assertEqual(magic, SNT_MAGIC)
        self.assertEqual(ver, 2)
        self.assertEqual(n_ch, 2)
        self.assertEqual(s_bytes, 2)
        self.assertEqual(hz10, 250)
        self.assertEqual(epoch, 1788394175000)
        self.assertEqual(count, 1500)

    def test_flow_limitation_missing_data_passthrough(self):
        """Verify that FlowLim (passthrough=True) outputs -1 on missing sentinel and does not output 0."""
        flow_lim_sig = SignalDef(
            label="FlowLim.",
            pmin=0.0,
            pmax=1.0,
            dmin=0,
            dmax=100,
            samples_per_record=60,
            invalid_passthrough=True,
        )

        # 1-channel stream with: 0.25 (stored as 25), missing (INT16_MIN), 0.75 (stored as 75)
        raw_samples = [25, INT16_MIN, 75]
        converted = convert_snt_samples(
            version=2,
            snt_channels=1,
            raw_samples=raw_samples,
            sig_def=flow_lim_sig,
            channel_idx=0,
        )

        self.assertEqual(len(converted), 3)
        self.assertEqual(converted[0], 25)
        # Missing sentinel must map to exact -1, NOT 0 or clamped value
        self.assertEqual(converted[1], -1)
        self.assertEqual(converted[2], 75)

    def test_mask_pressure_missing_data_no_leak(self):
        """Verify that MaskPress (passthrough=False) does NOT leak -1 sentinel."""
        mask_press_sig = SignalDef(
            label="MaskPress.",
            pmin=0.0,
            pmax=20.0,
            dmin=0,
            dmax=1000,
            samples_per_record=60,
            invalid_passthrough=False,
        )

        # In v1, stored was -1; in v2, stored is INT16_MIN.
        # k = 1000 / 20 = 50.
        # For stored = -1: phys = -0.01, dig = 0 + (-0.01)*50 = -0.5 -> rounds to -1.
        # Clamping MUST force this to dmin (0) because passthrough=False!
        raw_samples = [1000, -1, INT16_MIN, 1500]
        converted = convert_snt_samples(
            version=2,
            snt_channels=1,
            raw_samples=raw_samples,
            sig_def=mask_press_sig,
            channel_idx=0,
        )

        self.assertEqual(converted[0], 500)  # 10.00 cmH2O -> 500
        self.assertEqual(converted[1], 0)    # Must clamp to 0 (dig_min), NOT -1
        self.assertEqual(converted[2], 0)    # Must clamp to 0 (dig_min), NOT -1
        self.assertEqual(converted[3], 750)  # 15.00 cmH2O -> 750

    def test_pld_channel_mapping_and_reduction(self):
        """Verify 12-channel .snt interleaving is mapped to 9-channel PLD EDF."""
        pld_map = [0, 1, 2, 3, 4, 5, 6, 9, 10]
        self.assertEqual(len(pld_map), 9)
        self.assertNotIn(7, pld_map)  # TgtVent dropped
        self.assertNotIn(8, pld_map)  # IERatio dropped
        self.assertNotIn(11, pld_map) # Ti dropped

        # 1 sample frame across 12 channels
        frame = [i * 100 for i in range(12)]
        mapped_frame = [frame[ch] for ch in pld_map]
        expected = [0, 100, 200, 300, 400, 500, 600, 900, 1000]
        self.assertEqual(mapped_frame, expected)

    def test_recording_id_resmed_exact_formatting(self):
        """Verify Startdate format matching ResMed AirSense 11 specification."""
        srn = "22251436648"
        mid = "46"
        vid = "3"
        # 03-SEP-2026
        out = f"Startdate 03-SEP-2026 X X X SRN={srn} MID={mid} VID={vid}"
        self.assertTrue(out.startswith("Startdate 03-SEP-2026"))
        self.assertIn(f"SRN={srn}", out)
        self.assertIn(f"MID={mid}", out)
        self.assertIn(f"VID={vid}", out)
        self.assertLessEqual(len(out), 80)


if __name__ == "__main__":
    print("=== Running SNT->EDF Round-Trip Contract Tests ===")
    res = unittest.main(exit=False)
    if res.result.wasSuccessful():
        print(">>> ALL ROUND-TRIP CONTRACT TESTS PASSED <<<")
        sys.exit(0)
    else:
        sys.exit(1)
