#!/usr/bin/env python3
"""Contracts for allocation-free native timezone search."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/timezone_catalog.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/timezone_catalog.h").read_text(encoding="utf-8")

for symbol in (
    "timezone_catalog_search", "timezone_catalog_lookup",
    "timezone_catalog_search_source", "timezone_catalog_lookup_source",
    "utc_offset", "abbreviation", "posix",
):
    assert symbol in HEADER, f"timezone catalog API omits {symbol}"

for forbidden in ("malloc(", "calloc(", "realloc(", "cJSON"):
    assert forbidden not in SOURCE, f"timezone catalog allocates via {forbidden}"

assert "_binary_zones_json_start" in SOURCE
assert "_binary_zones_json_end" in SOURCE
assert "folded" in SOURCE and "character == ' '" in SOURCE
assert "POSIX signs describe what is added to local time to obtain UTC" in SOURCE

print("timezone catalog contract passed")
