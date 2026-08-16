"""
Common string and file utility helpers.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import re
from pathlib import Path


def get_all_text_between(
    filepath: Path | str | None,
    pattern_begin: str,
    pattern_end: str,
    encoding: str | None = None,
    search_str: str | None = None,
    inclusive: bool = True
) -> str:
    """Gets all text between two regex patterns."""
    if filepath is None and search_str is None:
        raise ValueError("Either filepath or search_str must be provided.")

    if filepath is not None:
        path = Path(filepath)
        if not path.exists():
            raise FileNotFoundError(f"File not found: {path}")
        search_str = path.read_text(encoding=encoding)

    if search_str is None:
        raise ValueError("Search string is empty.")

    m = re.search(f"({pattern_begin})(.*?)({pattern_end})", search_str, re.MULTILINE | re.DOTALL)
    if m is None:
        raise LookupError(f"ERROR: couldn't find {pattern_begin}")

    return m.group(0) if inclusive else m.group(2)
