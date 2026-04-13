#!/usr/bin/env python3
"""Convert HAS-style m68k assembly to GAS-compatible format.
- Strip leading underscore from C symbols (_Symbol -> Symbol)
- Convert $hex to 0xhex constants
Reads stdin, writes stdout."""
import sys, re

for line in sys.stdin:
    # Strip leading underscore from C symbol references.
    # Match _X where X starts with a letter, preceded by non-alphanumeric.
    line = re.sub(r'(?<![A-Za-z0-9])_([A-Za-z][A-Za-z0-9_]*)', r'\1', line)

    # Convert $hex constants to 0xhex (but not in comments after |).
    # Split line at | comment marker, only convert the code part.
    parts = line.split('|', 1)
    # Replace $XXXX with 0xXXXX where $ is preceded by #, comma, space, or (
    parts[0] = re.sub(r'(?<=[#, (])\$([0-9A-Fa-f]+)', r'0x\1', parts[0])
    line = '|'.join(parts)

    sys.stdout.write(line)
