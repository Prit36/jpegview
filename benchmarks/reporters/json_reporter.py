"""
JSON Report Exporter for JPEGView Benchmarks.
Stores machine-readable test records, hardware metadata, and raw metric distributions.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class JSONReporter:
    """Exports structured JSON benchmark telemetry and results."""

    def generate_report(
        self,
        comparison: dict[str, Any],
        all_results: dict[str, Any],
        system_info: dict[str, Any],
        out_path: Path
    ) -> Path:
        data: dict[str, Any] = {
            "schema_version": "1.0.0",
            "system_info": system_info,
            "comparison_summary": comparison,
            "target_results": all_results
        }

        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(data, indent=2), encoding="utf-8")

        print(f"[+] Exported structured JSON results to {out_path}")
        return out_path
