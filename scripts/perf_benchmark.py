#!/usr/bin/env python3
"""Orchestrate a performance benchmark run for the HFT system."""

import argparse
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, TextIO

PROJECT_ROOT = Path(__file__).resolve().parents[1]
LOG_PATTERN = re.compile(r"\d{2}:\d{2}:\d{2}\.\d{9} RDTSC ([A-Za-z0-9_]+) (\d+)")

CLIENT_CONFIGS = [
    {
        "label": "client1_maker",
        "args": [
            "./build-perf-test/trading_main",
            "1",
            "MAKER",
            "100",
            "0.6",
            "150",
            "300",
            "-100",
            "60",
            "0.6",
            "150",
            "300",
            "-100",
            "150",
            "0.5",
            "250",
            "600",
            "-100",
            "200",
            "0.4",
            "500",
            "3000",
            "-100",
            "1000",
            "0.9",
            "5000",
            "4000",
            "-100",
            "300",
            "0.8",
            "1500",
            "3000",
            "-100",
            "50",
            "0.7",
            "150",
            "300",
            "-100",
            "100",
            "0.3",
            "250",
            "300",
            "-100",
        ],
        "delay": 5.0,
    },
    {
        "label": "client2_maker",
        "args": [
            "./build-perf-test/trading_main",
            "2",
            "MAKER",
            "2100",
            "0.4",
            "2150",
            "2300",
            "-1100",
            "260",
            "0.8",
            "2150",
            "2300",
            "-1100",
            "2150",
            "0.2",
            "2250",
            "2600",
            "-1100",
            "2200",
            "0.6",
            "2500",
            "23000",
            "-1100",
            "210",
            "0.6",
            "2500",
            "24000",
            "-1100",
            "2300",
            "0.5",
            "21500",
            "23000",
            "-1100",
            "250",
            "0.8",
            "2150",
            "2300",
            "-1100",
            "2100",
            "0.3",
            "2250",
            "2300",
            "-1100",
        ],
        "delay": 5.0,
    },
    {
        "label": "client3_taker",
        "args": [
            "./build-perf-test/trading_main",
            "3",
            "TAKER",
            "300",
            "0.8",
            "350",
            "300",
            "-300",
            "60",
            "0.7",
            "350",
            "300",
            "-300",
            "350",
            "0.5",
            "250",
            "600",
            "-300",
            "200",
            "0.6",
            "500",
            "3000",
            "-300",
            "3000",
            "0.5",
            "5000",
            "4000",
            "-300",
            "300",
            "0.7",
            "3500",
            "3000",
            "-300",
            "50",
            "0.3",
            "350",
            "300",
            "-300",
            "300",
            "0.8",
            "350",
            "300",
            "-300",
        ],
        "delay": 5.0,
    },
    {
        "label": "client4_taker",
        "args": [
            "./build-perf-test/trading_main",
            "4",
            "TAKER",
            "4100",
            "0.8",
            "4150",
            "4300",
            "-1100",
            "460",
            "0.9",
            "4150",
            "4300",
            "-1100",
            "4150",
            "0.4",
            "4450",
            "4600",
            "-1100",
            "4400",
            "0.4",
            "4500",
            "43000",
            "-1100",
            "410",
            "0.6",
            "4500",
            "44000",
            "-1100",
            "4300",
            "0.6",
            "41500",
            "43000",
            "-1100",
            "450",
            "0.6",
            "4150",
            "4300",
            "-1100",
            "4100",
            "0.9",
            "4450",
            "4300",
            "-1100",
        ],
        "delay": 5.0,
    },
    {
        "label": "client5_random",
        "args": ["./build-perf-test/trading_main", "5", "RANDOM"],
        "delay": 5.0,
    },
]


@dataclass
class ProcessHandle:
    label: str
    role: str
    popen: subprocess.Popen
    stdout_path: Path
    stdout_file: TextIO

    def is_running(self) -> bool:
        return self.popen.poll() is None

    def close_log(self) -> None:
        try:
            self.stdout_file.close()
        except Exception:
            pass


@dataclass
class ProcSample:
    proc_ticks: int
    total_ticks: int
    rss_kb: int


class ProcessMonitor:
    """Sample CPU and RSS usage for a running process."""

    def __init__(self, handle: ProcessHandle, samples_dir: Path, sample_interval: float):
        self.handle = handle
        self.samples_dir = samples_dir
        self.sample_interval = sample_interval
        self.page_size_bytes = os.sysconf("SC_PAGE_SIZE")
        self.ticks_per_second = os.sysconf("SC_CLK_TCK")
        self.num_cpus = os.cpu_count() or 1
        self.samples_path = samples_dir / f"resource_samples_{handle.label}.csv"
        self.samples_path.write_text("timestamp_iso,elapsed_s,cpu_percent,rss_kb\n")
        self._cpu_samples: List[float] = []
        self._rss_samples: List[float] = []
        self._prev_proc_ticks: Optional[int] = None
        self._prev_total_ticks: Optional[int] = None
        self._primed = False

    def _read_proc_stats(self) -> Optional[ProcSample]:
        pid = self.handle.popen.pid
        stat_path = Path(f"/proc/{pid}/stat")
        if not stat_path.exists():
            return None
        try:
            with stat_path.open() as stat_file:
                parts = stat_file.read().strip().split()
        except FileNotFoundError:
            return None
        if len(parts) < 24:
            return None
        utime = int(parts[13])
        stime = int(parts[14])
        rss_pages = int(parts[23])
        rss_kb = rss_pages * self.page_size_bytes // 1024

        try:
            with open("/proc/stat", "r", encoding="utf-8") as f_stat:
                cpu_line = f_stat.readline()
        except FileNotFoundError:
            return None
        cpu_parts = cpu_line.strip().split()[1:]
        total_ticks = sum(int(val) for val in cpu_parts)

        return ProcSample(proc_ticks=utime + stime, total_ticks=total_ticks, rss_kb=rss_kb)

    def prime(self) -> None:
        stats = self._read_proc_stats()
        if stats is None:
            return
        self._prev_proc_ticks = stats.proc_ticks
        self._prev_total_ticks = stats.total_ticks
        self._primed = True

    def sample(self, measurement_start: float) -> None:
        if not self.handle.is_running():
            return
        stats = self._read_proc_stats()
        if stats is None:
            return
        if not self._primed:
            self._prev_proc_ticks = stats.proc_ticks
            self._prev_total_ticks = stats.total_ticks
            self._primed = True
            return
        assert self._prev_proc_ticks is not None and self._prev_total_ticks is not None
        delta_proc = stats.proc_ticks - self._prev_proc_ticks
        delta_total = stats.total_ticks - self._prev_total_ticks
        if delta_total <= 0:
            return
        cpu_percent = (delta_proc / delta_total) * 100.0 * self.num_cpus
        elapsed = time.time() - measurement_start
        timestamp_iso = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())
        rss_kb = stats.rss_kb
        with self.samples_path.open("a", encoding="utf-8") as samples_file:
            samples_file.write(f"{timestamp_iso},{elapsed:.3f},{cpu_percent:.2f},{rss_kb}\n")
        self._cpu_samples.append(cpu_percent)
        self._rss_samples.append(rss_kb)
        self._prev_proc_ticks = stats.proc_ticks
        self._prev_total_ticks = stats.total_ticks

    def summary(self) -> Dict[str, float]:
        if self._cpu_samples:
            avg_cpu = sum(self._cpu_samples) / len(self._cpu_samples)
            max_cpu = max(self._cpu_samples)
        else:
            avg_cpu = 0.0
            max_cpu = 0.0
        if self._rss_samples:
            avg_rss = sum(self._rss_samples) / len(self._rss_samples)
            max_rss = max(self._rss_samples)
        else:
            avg_rss = 0.0
            max_rss = 0.0
        return {
            "avg_cpu": avg_cpu,
            "max_cpu": max_cpu,
            "avg_rss": avg_rss,
            "max_rss": max_rss,
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Performance benchmark harness for the HFT system")
    parser.add_argument("--duration", type=float, default=120.0, help="Measurement duration in seconds (after warmup)")
    parser.add_argument("--warmup", type=float, default=10.0, help="Warmup time before measurements start")
    parser.add_argument("--sample-interval", type=float, default=0.5, help="Sampling interval for resource usage")
    parser.add_argument("--results-dir", type=Path, default=None, help="Directory to store benchmark artifacts")
    parser.add_argument("--skip-build", action="store_true", help="Skip the CMake/ninja build step")
    return parser.parse_args()


def run_build() -> None:
    missing_tools = [tool for tool in ("cmake", "ninja") if shutil.which(tool) is None]
    if missing_tools:
        install_hint = "sudo apt-get install cmake ninja-build"
        raise RuntimeError(
            "缺少构建工具: {}。请安装相关工具（例如 `{}`），或在已有二进制的情况下使用 --skip-build.".format(
                ", ".join(missing_tools), install_hint
            )
        )
    print("[+] Building binaries (Release + Perf)")
    subprocess.run(["bash", "scripts/build.sh"], cwd=PROJECT_ROOT, check=True)


def launch_process(cmd: List[str], label: str, role: str, stdout_dir: Path) -> ProcessHandle:
    stdout_path = stdout_dir / f"{label}.stdout.log"
    stdout_file = stdout_path.open("w", encoding="utf-8")
    popen = subprocess.Popen(cmd, cwd=PROJECT_ROOT, stdout=stdout_file, stderr=subprocess.STDOUT)
    return ProcessHandle(label=label, role=role, popen=popen, stdout_path=stdout_path, stdout_file=stdout_file)


def ensure_process_running(handle: ProcessHandle) -> None:
    time.sleep(1.0)
    if handle.popen.poll() is not None:
        raise RuntimeError(f"Process {handle.label} exited prematurely, check {handle.stdout_path}")


def graceful_shutdown(processes: List[ProcessHandle], timeout: float = 10.0) -> None:
    for handle in processes:
        if handle.is_running():
            try:
                handle.popen.send_signal(signal.SIGINT)
            except ProcessLookupError:
                continue
    deadline = time.time() + timeout
    while time.time() < deadline:
        if all(not handle.is_running() for handle in processes):
            break
        time.sleep(0.5)
    for handle in processes:
        if handle.is_running():
            print(f"[!] Force killing {handle.label}")
            handle.popen.kill()
    for handle in processes:
        handle.close_log()


def collect_new_logs(before: List[Path], logs_dir: Path) -> List[Path]:
    logs_after = set(PROJECT_ROOT.glob("*.log"))
    new_logs = [path for path in logs_after if path not in before]
    logs_dir.mkdir(parents=True, exist_ok=True)
    moved_paths = []
    for path in new_logs:
        destination = logs_dir / path.name
        shutil.move(str(path), destination)
        moved_paths.append(destination)
    return moved_paths


def read_cpu_mhz() -> Optional[float]:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        with cpuinfo.open() as f:
            for line in f:
                if line.startswith("cpu MHz"):
                    try:
                        return float(line.split(":", 1)[1].strip())
                    except ValueError:
                        continue
    try:
        result = subprocess.run(["lscpu"], capture_output=True, text=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    for line in result.stdout.splitlines():
        if "CPU MHz" in line:
            try:
                return float(line.split(":", 1)[1].strip())
            except ValueError:
                continue
    return None


def percentile(values: Sequence[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    rank = pct / 100.0 * (len(values) - 1)
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return values[int(rank)]
    weight = rank - lower
    return values[lower] * (1 - weight) + values[upper] * weight


def analyze_rdtsc_logs(log_paths: List[Path], cpu_mhz: Optional[float]) -> List[Dict[str, float]]:
    metrics: Dict[str, List[int]] = defaultdict(list)
    for path in log_paths:
        with path.open(encoding="utf-8", errors="ignore") as logfile:
            for line in logfile:
                match = LOG_PATTERN.search(line)
                if match:
                    tag = match.group(1)
                    cycles = int(match.group(2))
                    metrics[tag].append(cycles)
    results = []
    for tag, cycles_list in metrics.items():
        cycles_list.sort()
        count = len(cycles_list)
        float_cycles = [float(value) for value in cycles_list]
        avg_cycles = sum(float_cycles) / count
        p50_cycles = percentile(float_cycles, 50.0)
        p99_cycles = percentile(float_cycles, 99.0)
        max_cycles = float_cycles[-1]
        if cpu_mhz:
            factor = 1000.0 / cpu_mhz
            avg_ns = avg_cycles * factor
            p50_ns = p50_cycles * factor
            p99_ns = p99_cycles * factor
            max_ns = max_cycles * factor
        else:
            avg_ns = p50_ns = p99_ns = max_ns = 0.0
        results.append(
            {
                "tag": tag,
                "count": count,
                "avg_cycles": avg_cycles,
                "p50_cycles": p50_cycles,
                "p99_cycles": p99_cycles,
                "max_cycles": max_cycles,
                "avg_ns": avg_ns,
                "p50_ns": p50_ns,
                "p99_ns": p99_ns,
                "max_ns": max_ns,
            }
        )
    results.sort(key=lambda entry: entry["tag"])
    return results


def write_rdtsc_report(results: List[Dict[str, float]], output_path: Path, cpu_mhz: Optional[float]) -> None:
    header = (
        "tag,count,avg_cycles,p50_cycles,p99_cycles,max_cycles,avg_ns,p50_ns,p99_ns,max_ns"
    )
    with output_path.open("w", encoding="utf-8") as report:
        report.write(header + "\n")
        for entry in results:
            report.write(
                f"{entry['tag']},{entry['count']},{entry['avg_cycles']:.2f},{entry['p50_cycles']:.2f},"
                f"{entry['p99_cycles']:.2f},{entry['max_cycles']:.2f},{entry['avg_ns']:.2f},"
                f"{entry['p50_ns']:.2f},{entry['p99_ns']:.2f},{entry['max_ns']:.2f}\n"
            )


def compute_throughput(log_paths: List[Path], duration: float) -> Dict[str, float]:
    totals: Dict[str, int] = defaultdict(int)
    patterns = {
        "matching_engine_requests": re.compile(r"Processing .*MEClientRequest"),
        "order_server_responses": re.compile(r"Processing cid:"),
        "market_data_updates": re.compile(r"Sending seq:"),
        "order_gateway_sends": re.compile(r"Sending cid:"),
    }
    for path in log_paths:
        filename = path.name
        with path.open(encoding="utf-8", errors="ignore") as logfile:
            for line in logfile:
                for key, pattern in patterns.items():
                    if pattern.search(line):
                        totals[key] += 1
        if "market_data_consumer" in filename:
            with path.open(encoding="utf-8", errors="ignore") as logfile:
                for line in logfile:
                    if "Received socket" in line:
                        totals["md_consumer_receives"] += 1
    throughput = {key: (value / duration if duration > 0 else 0.0) for key, value in totals.items()}
    throughput.update({f"total_{key}": value for key, value in totals.items()})
    return throughput


def write_summary(
    results_dir: Path,
    duration: float,
    warmup: float,
    cpu_mhz: Optional[float],
    monitors: List[ProcessMonitor],
    throughput: Dict[str, float],
    rdtsc_results: List[Dict[str, float]],
) -> None:
    summary_path = results_dir / "summary.txt"
    lines = []
    lines.append(f"Measurement duration: {duration:.1f}s (warmup {warmup:.1f}s)")
    if cpu_mhz:
        lines.append(f"CPU frequency reference: {cpu_mhz:.2f} MHz")
    else:
        lines.append("CPU frequency reference: unavailable (RDTSC metrics remain in cycles)")
    lines.append("")
    lines.append("Process resource usage:")
    lines.append("label,role,avg_cpu_pct,max_cpu_pct,avg_rss_kb,max_rss_kb")
    for monitor in monitors:
        stats = monitor.summary()
        lines.append(
            f"{monitor.handle.label},{monitor.handle.role},{stats['avg_cpu']:.2f},"
            f"{stats['max_cpu']:.2f},{stats['avg_rss']:.1f},{stats['max_rss']:.1f}"
        )
    lines.append("")
    lines.append("Throughput metrics:")
    for key in sorted(throughput):
        lines.append(f"{key}: {throughput[key]:.2f}")
    lines.append("")
    lines.append("Latency metrics (ns unless CPU MHz unavailable):")
    lines.append("tag,count,avg_ns,p50_ns,p99_ns,max_ns")
    top_tags = sorted(rdtsc_results, key=lambda entry: entry["count"], reverse=True)[:15]
    for entry in top_tags:
        lines.append(
            f"{entry['tag']},{entry['count']},{entry['avg_ns']:.2f},{entry['p50_ns']:.2f},"
            f"{entry['p99_ns']:.2f},{entry['max_ns']:.2f}"
        )
    summary_path.write_text("\n".join(lines), encoding="utf-8")
    print(summary_path.read_text())


def main() -> None:
    args = parse_args()
    results_dir = args.results_dir
    if results_dir is None:
        timestamp = time.strftime("%Y%m%d-%H%M%S")
        results_dir = PROJECT_ROOT / "perf-results" / timestamp
    results_dir = results_dir.resolve()
    stdout_dir = results_dir / "stdout"
    samples_dir = results_dir / "samples"
    logs_dir = results_dir / "logs"
    stdout_dir.mkdir(parents=True, exist_ok=True)
    samples_dir.mkdir(parents=True, exist_ok=True)

    if not args.skip_build:
        try:
            run_build()
        except RuntimeError as exc:
            print(f"[!] {exc}", file=sys.stderr)
            sys.exit(1)

    existing_logs = list(PROJECT_ROOT.glob("*.log"))

    print("[+] Launching exchange")
    exchange_handle = launch_process(["./build-perf-test/exchange_main"], "exchange", "exchange", stdout_dir)
    ensure_process_running(exchange_handle)

    processes = [exchange_handle]

    monitors: List[ProcessMonitor] = []
    monitors.append(ProcessMonitor(exchange_handle, samples_dir, args.sample_interval))

    for config in CLIENT_CONFIGS:
        print(f"[+] Launching {config['label']}")
        handle = launch_process(config["args"], config["label"], "client", stdout_dir)
        ensure_process_running(handle)
        processes.append(handle)
        monitors.append(ProcessMonitor(handle, samples_dir, args.sample_interval))
        time.sleep(config.get("delay", 0.0))

    print(f"[+] Warmup for {args.warmup:.1f}s")
    for monitor in monitors:
        monitor.prime()
    time.sleep(args.warmup)

    measurement_start = time.time()
    measurement_end = measurement_start + args.duration
    print(f"[+] Collecting metrics for {args.duration:.1f}s")
    while time.time() < measurement_end:
        for monitor in monitors:
            monitor.sample(measurement_start)
        time.sleep(args.sample_interval)
    print("[+] Measurement window complete, initiating shutdown")

    graceful_shutdown(processes)

    moved_logs = collect_new_logs(existing_logs, logs_dir)

    cpu_mhz = read_cpu_mhz()
    rdtsc_results = analyze_rdtsc_logs(moved_logs, cpu_mhz)
    throughput = compute_throughput(moved_logs, args.duration)

    rdtsc_report_path = results_dir / "rdtsc_metrics.csv"
    write_rdtsc_report(rdtsc_results, rdtsc_report_path, cpu_mhz)
    write_summary(results_dir, args.duration, args.warmup, cpu_mhz, monitors, throughput, rdtsc_results)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(1)
