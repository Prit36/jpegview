"""
Compute SHA-256 hashes of files in a directory using Python 3.11+ hashlib.file_digest.
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

glob_pattern = "*" if len(sys.argv) == 1 else sys.argv[1]

for path in Path.cwd().glob(glob_pattern):
    if not path.is_file():
        continue

    with path.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    print(f"{digest} *{path.name}")
