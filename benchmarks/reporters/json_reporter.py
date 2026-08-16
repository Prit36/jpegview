"""
JSON Report Exporter for JPEGView Benchmarks.
Stores machine-readable test records, hardware metadata, and raw metric distributions.
"""

import json
from pathlib import Path
from typing import Dict, Any


class JSONReporter:
    """Exports structured JSON benchmark telemetry and results."""

    def generate_report(
        self,
        comparison: Dict[str, Any],
        all_results: Dict[str, Any],
        system_info: Dict[str, Any],
        out_path: Path
    ) -> Path:
        data = {
            "schema_version": "1.0.0",
            "system_info": system_info,
            "comparison_summary": comparison,
            "target_results": all_results
        }

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

        print(f"[+] Exported structured JSON results to {out_path}")
        return out_path
