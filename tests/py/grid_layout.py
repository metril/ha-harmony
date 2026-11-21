"""Test helper: pin hand-written YAML pages to `layout: grid` (the stretched grid), so tests of the grid
compiler path keep their meaning now that every other page is converted to the FIXED layout."""

import re


def grid(text: str) -> str:
    head, sep, tail = text.partition("pages:")
    if not sep:
        return text
    tail = re.sub(r"(?m)^(  - \{)(?![^\n]*layout:)", r"\1layout: grid, ", tail)
    tail = re.sub(r"^ ?\[\{(?![^\n]*layout:)", r" [{layout: grid, ", tail)
    tail = re.sub(r"(?m)^(  - id: [^\n]*)$(?!\n    layout:)", r"\1\n    layout: grid", tail)
    return head + sep + tail
