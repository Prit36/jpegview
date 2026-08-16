"""
Interactive HTML Dashboard Report Generator for JPEGView Benchmarks.
Produces a modern, dark-theme visual report with interactive charts and breakdowns.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class HTMLReporter:
    """Generates modern interactive HTML reports with Chart.js visualizations."""

    def generate_report(
        self,
        comparison: dict[str, Any],
        all_results: dict[str, Any],
        system_info: dict[str, Any],
        out_path: Path
    ) -> Path:
        baseline: str = comparison.get("baseline_target", "original-fork")
        targets: list[str] = comparison.get("targets", [])

        target_labels_json = json.dumps(targets)

        ttfp_means = [
            all_results.get(t, {}).get("e2e", {}).get("large_image_load", {}).get("ttfp_ms", {}).get("mean", 0.0)
            for t in targets
        ]

        fps_means = [
            all_results.get(t, {}).get("e2e", {}).get("folder_navigation", {}).get("fps", {}).get("mean", 0.0)
            for t in targets
        ]

        html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>JPEGView Performance Benchmark Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    :root {{
      --bg-main: #0d1117;
      --bg-card: #161b22;
      --border-color: #30363d;
      --text-main: #c9d1d9;
      --text-muted: #8b949e;
      --accent-blue: #58a6ff;
      --accent-green: #3fb950;
      --accent-red: #f85149;
      --accent-yellow: #d29922;
    }}
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      background-color: var(--bg-main);
      color: var(--text-main);
      line-height: 1.5;
      padding: 24px;
    }}
    .container {{ max-width: 1280px; margin: 0 auto; }}
    header {{
      margin-bottom: 24px;
      padding-bottom: 16px;
      border-bottom: 1px solid var(--border-color);
      display: flex;
      justify-content: space-between;
      align-items: center;
    }}
    h1 {{ font-size: 24px; font-weight: 600; color: #f0f6fc; }}
    .badge {{
      display: inline-block;
      padding: 4px 10px;
      border-radius: 12px;
      font-size: 12px;
      font-weight: 600;
      background-color: rgba(88, 166, 255, 0.15);
      color: var(--accent-blue);
      border: 1px solid rgba(88, 166, 255, 0.4);
    }}
    .grid-2 {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(450px, 1fr));
      gap: 20px;
      margin-bottom: 24px;
    }}
    .card {{
      background-color: var(--bg-card);
      border: 1px solid var(--border-color);
      border-radius: 8px;
      padding: 20px;
    }}
    .card-title {{
      font-size: 16px;
      font-weight: 600;
      color: #f0f6fc;
      margin-bottom: 16px;
      display: flex;
      align-items: center;
      justify-content: space-between;
    }}
    .stat-row {{
      display: flex;
      justify-content: space-between;
      padding: 8px 0;
      border-bottom: 1px solid rgba(48, 54, 61, 0.5);
      font-size: 14px;
    }}
    .stat-label {{ color: var(--text-muted); }}
    .stat-value {{ font-weight: 600; font-family: monospace; }}
    table {{
      width: 100%;
      border-collapse: collapse;
      font-size: 14px;
      margin-top: 10px;
    }}
    th, td {{
      padding: 10px 14px;
      text-align: left;
      border-bottom: 1px solid var(--border-color);
    }}
    th {{
      background-color: rgba(22, 27, 34, 0.8);
      color: var(--text-muted);
      font-weight: 600;
    }}
    tr:hover td {{ background-color: rgba(56, 139, 253, 0.05); }}
    .chart-container {{ position: relative; height: 260px; width: 100%; }}
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div>
        <h1>JPEGView Systematic Performance Benchmark</h1>
        <p style="color: var(--text-muted); font-size: 14px; margin-top: 4px;">
          Systematic regression testing and latency analysis across git branches
        </p>
      </div>
      <div>
        <span class="badge">Baseline: {baseline}</span>
      </div>
    </header>

    <div class="card" style="margin-bottom: 24px;">
      <div class="card-title">Hardware & System Telemetry</div>
      <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 16px;">
        <div class="stat-row"><span class="stat-label">CPU:</span><span class="stat-value">{system_info.get('cpu_model', 'N/A')}</span></div>
        <div class="stat-row"><span class="stat-label">Cores:</span><span class="stat-value">{system_info.get('cpu_cores_logical', 0)} Logical</span></div>
        <div class="stat-row"><span class="stat-label">RAM:</span><span class="stat-value">{system_info.get('ram_total_gb', 0)} GB Total</span></div>
        <div class="stat-row"><span class="stat-label">AVX2:</span><span class="stat-value">{system_info.get('avx2_supported')}</span></div>
        <div class="stat-row"><span class="stat-label">OS:</span><span class="stat-value">{system_info.get('os_name')} {system_info.get('os_release')}</span></div>
      </div>
    </div>

    <div class="grid-2">
      <div class="card">
        <div class="card-title">Large Image TTFP (Lower is Better) <span>Latency (ms)</span></div>
        <div class="chart-container">
          <canvas id="chartTtfp"></canvas>
        </div>
      </div>
      <div class="card">
        <div class="card-title">Folder Navigation Throughput (Higher is Better) <span>Speed (FPS)</span></div>
        <div class="chart-container">
          <canvas id="chartFps"></canvas>
        </div>
      </div>
    </div>

    <div class="card" style="margin-bottom: 24px;">
      <div class="card-title">Target Comparison Matrix</div>
      <table>
        <thead>
          <tr>
            <th>Target</th>
            <th>TTFP (Mean)</th>
            <th>TTFP (Min)</th>
            <th>Throughput (FPS)</th>
            <th>Avg Frame Latency</th>
            <th>Peak RAM</th>
          </tr>
        </thead>
        <tbody>
"""

        for target in targets:
            ttfp_d = all_results.get(target, {}).get("e2e", {}).get("large_image_load", {})
            nav_d = all_results.get(target, {}).get("e2e", {}).get("folder_navigation", {})

            ttfp_m = ttfp_d.get("ttfp_ms", {}).get("mean", 0.0)
            ttfp_min = ttfp_d.get("ttfp_ms", {}).get("min", 0.0)
            fps_m = nav_d.get("fps", {}).get("mean", 0.0)
            frame_m = nav_d.get("avg_frame_ms", {}).get("mean", 0.0)
            ram_m = ttfp_d.get("peak_working_set_mb", {}).get("mean", 0.0)

            html_content += f"""
          <tr>
            <td><strong><code>{target}</code></strong></td>
            <td>{ttfp_m:.2f} ms</td>
            <td>{ttfp_min:.2f} ms</td>
            <td>{fps_m:.1f} FPS</td>
            <td>{frame_m:.2f} ms</td>
            <td>{ram_m:.1f} MB</td>
          </tr>"""

        html_content += f"""
        </tbody>
      </table>
    </div>
  </div>

  <script>
    const targets = {target_labels_json};
    const ttfpMeans = {json.dumps(ttfp_means)};
    const fpsMeans = {json.dumps(fps_means)};

    new Chart(document.getElementById('chartTtfp'), {{
      type: 'bar',
      data: {{
        labels: targets,
        datasets: [{{
          label: 'Time to First Paint (ms)',
          data: ttfpMeans,
          backgroundColor: 'rgba(88, 166, 255, 0.6)',
          borderColor: '#58a6ff',
          borderWidth: 1
        }}]
      }},
      options: {{
        responsive: true,
        maintainAspectRatio: false,
        scales: {{
          y: {{ beginAtZero: true, grid: {{ color: '#21262d' }} }},
          x: {{ grid: {{ display: false }} }}
        }},
        plugins: {{ legend: {{ display: false }} }}
      }}
    }});

    new Chart(document.getElementById('chartFps'), {{
      type: 'bar',
      data: {{
        labels: targets,
        datasets: [{{
          label: 'Navigation Throughput (FPS)',
          data: fpsMeans,
          backgroundColor: 'rgba(63, 185, 80, 0.6)',
          borderColor: '#3fb950',
          borderWidth: 1
        }}]
      }},
      options: {{
        responsive: true,
        maintainAspectRatio: false,
        scales: {{
          y: {{ beginAtZero: true, grid: {{ color: '#21262d' }} }},
          x: {{ grid: {{ display: false }} }}
        }},
        plugins: {{ legend: {{ display: false }} }}
      }}
    }});
  </script>
</body>
</html>
"""

        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(html_content, encoding="utf-8")

        print(f"[+] Exported interactive HTML report to {out_path}")
        return out_path
