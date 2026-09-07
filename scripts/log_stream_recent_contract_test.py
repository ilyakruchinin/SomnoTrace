#!/usr/bin/env python3
"""Safety contracts for the bounded /api/logs/recent JSON response."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/log_stream.c").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return source[match.end() : cursor - 1]


handler = function_body(SOURCE, "logs_recent_handler")

# The handler always reserves enough space for a complete suffix and checks
# the actual snprintf result before exposing its length to the HTTP server.
assert "LOGS_RECENT_SUFFIX_RESERVE" in handler
assert "needed + LOGS_RECENT_SUFFIX_RESERVE > cap - pos" in handler
assert "suffix_len < 0" in handler
assert "(size_t)suffix_len >= cap - pos" in handler
assert "pos += (size_t)suffix_len" in handler

# Capacity is checked for a complete escaped line before its comma or opening
# quote is written, so truncation can never leave malformed JSON.
size_at = handler.find("size_t escaped_len")
fit_at = handler.find("needed + LOGS_RECENT_SUFFIX_RESERVE")
comma_at = handler.find("if (chunks_sent > 0) buf[pos++] = ','")
quote_at = handler.find("buf[pos++] = '\"'")
assert -1 not in (size_at, fit_at, comma_at, quote_at)
assert size_at < fit_at < comma_at < quote_at
assert "goto buf_full" not in handler

# Each acquired ring item reaches the single return site, even if the bounded
# response becomes full while processing that item.
receive_at = handler.find("xRingbufferReceiveUpTo")
full_break_at = handler.find("response_full = true")
return_at = handler.find("vRingbufferReturnItem")
suffix_at = handler.find("int suffix_len")
assert -1 not in (receive_at, full_break_at, return_at, suffix_at)
assert receive_at < full_break_at < return_at < suffix_at
assert handler.count("xRingbufferReceiveUpTo") == 1
assert handler.count("vRingbufferReturnItem") == 1

print("recent log endpoint contract passed")
