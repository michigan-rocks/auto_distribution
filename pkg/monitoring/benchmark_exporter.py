#!/usr/bin/env python3
"""Expose retained JSON benchmark reports in Prometheus text format."""

from __future__ import annotations

import json
import math
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


RESULTS_DIR = Path(os.environ.get("BENCHMARK_RESULTS_DIR", "/results"))
PORT = int(os.environ.get("METRICS_PORT", "9200"))
REFRESH_SECONDS = 1.0


def escape_label(value: Any) -> str:
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def numeric(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def format_number(value: float) -> str:
    return format(value, ".17g")


def report_labels(report: dict[str, Any]) -> str:
    scheduler = report.get("scheduler")
    if not isinstance(scheduler, dict):
        scheduler = {}

    experiment_id = str(report.get("experiment_id", ""))
    date_match = re.search(r"-(\d{8})-", experiment_id)
    run_date = date_match.group(1) if date_match else "unknown"
    workload = str(report.get("workload", "")).replace("\\", "/").rsplit("/", 1)[-1]
    labels = {
        "experiment_id": experiment_id,
        "run_date": run_date,
        "mode": report.get("mode", ""),
        "workload": workload,
        "status": report.get("status", ""),
        "alpha": scheduler.get("ewma_alpha", ""),
        "window": scheduler.get("max_in_flight_total", ""),
    }
    return ",".join(f'{key}="{escape_label(value)}"' for key, value in labels.items())


def samples_for_report(report: dict[str, Any]) -> list[str]:
    labels = report_labels(report)
    samples: list[str] = []
    metrics = {
        "benchmark_elapsed_seconds": report.get("elapsed_seconds"),
        "benchmark_tasks_per_second": report.get("tasks_per_second"),
        "benchmark_completed_tasks": report.get("completed"),
    }
    for name, raw_value in metrics.items():
        value = numeric(raw_value)
        if value is not None:
            samples.append(f"{name}{{{labels}}} {format_number(value)}")
    return samples


class ReportCache:
    def __init__(self) -> None:
        self._entries: dict[Path, tuple[tuple[int, int], list[str]]] = {}
        self._rendered = ""
        self._last_refresh = 0.0
        self._lock = threading.Lock()

    def render(self) -> str:
        with self._lock:
            now = time.monotonic()
            if now - self._last_refresh >= REFRESH_SECONDS:
                self._refresh()
                self._last_refresh = now
            return self._rendered

    def _refresh(self) -> None:
        current_paths = set(RESULTS_DIR.glob("*.json")) if RESULTS_DIR.exists() else set()
        for removed in set(self._entries) - current_paths:
            del self._entries[removed]

        for path in current_paths:
            try:
                stat = path.stat()
                signature = (stat.st_mtime_ns, stat.st_size)
                cached = self._entries.get(path)
                if cached is not None and cached[0] == signature:
                    continue
                with path.open("r", encoding="utf-8") as source:
                    report = json.load(source)
                if isinstance(report, dict):
                    self._entries[path] = (signature, samples_for_report(report))
            except (OSError, UnicodeError, json.JSONDecodeError):
                # A report can be observed while it is being replaced. Retry it
                # on the next scrape instead of making the metrics endpoint fail.
                continue

        lines = [
            "# HELP benchmark_elapsed_seconds Total measured benchmark duration.",
            "# TYPE benchmark_elapsed_seconds gauge",
            "# HELP benchmark_tasks_per_second Completed benchmark tasks per second.",
            "# TYPE benchmark_tasks_per_second gauge",
            "# HELP benchmark_completed_tasks Number of tasks completed by a benchmark.",
            "# TYPE benchmark_completed_tasks gauge",
        ]
        for path in sorted(self._entries):
            lines.extend(self._entries[path][1])
        self._rendered = "\n".join(lines) + "\n"


CACHE = ReportCache()


class MetricsHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path == "/-/healthy":
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
        elif self.path == "/metrics":
            body = CACHE.render().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        else:
            body = b"not found\n"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        return


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", PORT), MetricsHandler)
    print(f"Serving benchmark metrics from {RESULTS_DIR} on :{PORT}", flush=True)
    server.serve_forever()
