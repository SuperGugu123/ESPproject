#!/usr/bin/env python3
"""Read CSI data and emit simple occupancy/motion status.

This script builds on raw CSI parsing and adds a lightweight heuristic status
layer. It is not a trained model and it does not estimate absolute distance.
For best results, keep the monitored area empty during the calibration window
so the script can learn an "empty room" baseline.
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import sys
import time
from collections import deque
from dataclasses import asdict, dataclass
from pathlib import Path

import serial

from raw_csi_reader import normalize_line, open_optional_text, parse_packet, summarize_packet


CSV_COLUMNS = [
    "seq",
    "stage",
    "status",
    "mac",
    "rssi",
    "channel",
    "amplitude_mean",
    "amplitude_std",
    "phase_std",
    "calibration_mode",
    "calibration_progress",
    "remaining_packets",
    "remaining_seconds",
    "window_progress",
    "presence_score",
    "motion_score",
    "amp_mean_delta",
    "amp_std_delta",
    "phase_std_delta",
    "rssi_delta",
    "window_amp_mean",
    "window_amp_std",
    "window_phase_std",
    "window_rssi",
    "baseline_amplitude_mean",
    "baseline_amplitude_std",
    "baseline_phase_std",
    "baseline_rssi",
]


@dataclass
class Baseline:
    amplitude_mean: float
    amplitude_std: float
    phase_std: float
    rssi: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read CSI from serial and emit occupancy state JSON."
    )
    parser.add_argument("-p", "--port", required=True, help="Serial port, for example COM12")
    parser.add_argument(
        "-b",
        "--baud",
        type=int,
        default=115200,
        help="Serial baud rate, default: 115200",
    )
    parser.add_argument(
        "-n",
        "--packets",
        type=int,
        default=0,
        help="Stop after N valid CSI packets. 0 means run forever.",
    )
    parser.add_argument(
        "--log",
        dest="log_path",
        default="",
        help="Optional path to save malformed or non-CSI serial lines.",
    )
    parser.add_argument(
        "--csv",
        dest="csv_path",
        default="",
        help="Optional path to save emitted status rows as CSV.",
    )
    parser.add_argument(
        "--start-delay",
        type=float,
        default=0.0,
        help="Wait this many seconds before starting calibration, default: 0.",
    )
    parser.add_argument(
        "--calibration",
        type=int,
        default=60,
        help="Calibration packet count when --calibration-seconds is not used, default: 60.",
    )
    parser.add_argument(
        "--calibration-seconds",
        type=float,
        default=0.0,
        help="Calibration duration in seconds. If > 0, this overrides --calibration.",
    )
    parser.add_argument(
        "--window",
        type=int,
        default=20,
        help="Sliding window size for status estimation, default: 20.",
    )
    parser.add_argument(
        "--baseline-in",
        default="",
        help="Optional JSON baseline file to load and skip calibration.",
    )
    parser.add_argument(
        "--baseline-out",
        default="",
        help="Optional JSON baseline file to save after calibration.",
    )
    return parser.parse_args()


def compute_baseline(calibration_summaries: list[dict[str, object]]) -> Baseline:
    return Baseline(
        amplitude_mean=statistics.fmean(
            float(item["amplitude_mean"]) for item in calibration_summaries
        ),
        amplitude_std=statistics.fmean(
            float(item["amplitude_std"]) for item in calibration_summaries
        ),
        phase_std=statistics.fmean(float(item["phase_std"]) for item in calibration_summaries),
        rssi=statistics.fmean(float(item["rssi"]) for item in calibration_summaries),
    )


def classify_status(
    baseline: Baseline, history: deque[dict[str, object]]
) -> tuple[str, dict[str, float]]:
    amp_means = [float(item["amplitude_mean"]) for item in history]
    amp_stds = [float(item["amplitude_std"]) for item in history]
    phase_stds = [float(item["phase_std"]) for item in history]
    rssis = [float(item["rssi"]) for item in history]

    win_amp_mean = statistics.fmean(amp_means)
    win_amp_std = statistics.fmean(amp_stds)
    win_phase_std = statistics.fmean(phase_stds)
    win_rssi = statistics.fmean(rssis)

    amp_mean_delta = abs(win_amp_mean - baseline.amplitude_mean)
    amp_std_delta = abs(win_amp_std - baseline.amplitude_std)
    phase_std_delta = abs(win_phase_std - baseline.phase_std)
    rssi_delta = abs(win_rssi - baseline.rssi)

    amplitude_motion = statistics.pstdev(amp_means) if len(amp_means) > 1 else 0.0
    phase_motion = statistics.pstdev(phase_stds) if len(phase_stds) > 1 else 0.0
    rssi_motion = statistics.pstdev(rssis) if len(rssis) > 1 else 0.0

    presence_score = (
        amp_mean_delta + 0.8 * amp_std_delta + 1.2 * phase_std_delta + 0.25 * rssi_delta
    )
    motion_score = 1.6 * amplitude_motion + 2.4 * phase_motion + 0.4 * rssi_motion

    if motion_score >= 1.2 and presence_score >= 2.0:
        status = "有人移动"
    elif presence_score >= 2.0:
        status = "有人静止"
    else:
        status = "无人"

    features = {
        "presence_score": round(presence_score, 4),
        "motion_score": round(motion_score, 4),
        "amp_mean_delta": round(amp_mean_delta, 4),
        "amp_std_delta": round(amp_std_delta, 4),
        "phase_std_delta": round(phase_std_delta, 4),
        "rssi_delta": round(rssi_delta, 4),
        "window_amp_mean": round(win_amp_mean, 4),
        "window_amp_std": round(win_amp_std, 4),
        "window_phase_std": round(win_phase_std, 4),
        "window_rssi": round(win_rssi, 4),
    }
    return status, features


def print_json(payload: dict[str, object]) -> None:
    print(json.dumps(payload, ensure_ascii=False), flush=True)


def write_log_line(log_fd, line: str) -> None:
    if log_fd is None:
        return
    log_fd.write(line + "\n")
    log_fd.flush()


def round_seconds(value: float) -> float:
    return round(max(0.0, value), 3)


def baseline_to_dict(baseline: Baseline) -> dict[str, float]:
    return {
        "amplitude_mean": round(baseline.amplitude_mean, 4),
        "amplitude_std": round(baseline.amplitude_std, 4),
        "phase_std": round(baseline.phase_std, 4),
        "rssi": round(baseline.rssi, 4),
    }


def save_baseline(path_str: str, baseline: Baseline) -> None:
    if not path_str:
        return
    path = Path(path_str)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(baseline_to_dict(baseline), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def load_baseline(path_str: str) -> Baseline:
    data = json.loads(Path(path_str).read_text(encoding="utf-8"))
    return Baseline(
        amplitude_mean=float(data["amplitude_mean"]),
        amplitude_std=float(data["amplitude_std"]),
        phase_std=float(data["phase_std"]),
        rssi=float(data["rssi"]),
    )


def csv_row_from_payload(payload: dict[str, object], baseline: Baseline | None) -> dict[str, object]:
    row = {column: "" for column in CSV_COLUMNS}
    for column in ("seq", "stage", "status", "mac", "rssi", "channel"):
        if column in payload:
            row[column] = payload[column]

    for column in ("amplitude_mean", "amplitude_std", "phase_std"):
        if column in payload:
            row[column] = payload[column]

    for column in (
        "calibration_mode",
        "calibration_progress",
        "remaining_packets",
        "remaining_seconds",
        "window_progress",
    ):
        if column in payload:
            row[column] = payload[column]

    features = payload.get("features")
    if isinstance(features, dict):
        for key in (
            "presence_score",
            "motion_score",
            "amp_mean_delta",
            "amp_std_delta",
            "phase_std_delta",
            "rssi_delta",
            "window_amp_mean",
            "window_amp_std",
            "window_phase_std",
            "window_rssi",
        ):
            if key in features:
                row[key] = features[key]

    if baseline is not None:
        row["baseline_amplitude_mean"] = round(baseline.amplitude_mean, 4)
        row["baseline_amplitude_std"] = round(baseline.amplitude_std, 4)
        row["baseline_phase_std"] = round(baseline.phase_std, 4)
        row["baseline_rssi"] = round(baseline.rssi, 4)

    return row


def emit_payload(
    payload: dict[str, object],
    csv_writer: csv.DictWriter | None,
    csv_fd,
    baseline: Baseline | None,
) -> None:
    print_json(payload)
    if csv_writer is None or csv_fd is None:
        return
    csv_writer.writerow(csv_row_from_payload(payload, baseline))
    csv_fd.flush()


def main() -> int:
    args = parse_args()

    log_fd = open_optional_text(args.log_path)
    csv_fd = open_optional_text(args.csv_path)
    csv_writer = None
    if csv_fd is not None:
        csv_writer = csv.DictWriter(csv_fd, fieldnames=CSV_COLUMNS)
        csv_writer.writeheader()
        csv_fd.flush()

    calibration_summaries: list[dict[str, object]] = []
    history: deque[dict[str, object]] = deque(maxlen=max(3, args.window))
    baseline: Baseline | None = load_baseline(args.baseline_in) if args.baseline_in else None
    valid_count = 0

    delay_end_time = time.monotonic() + max(0.0, args.start_delay)
    calibration_start_time: float | None = None
    calibration_mode = "seconds" if args.calibration_seconds > 0 else "packets"
    calibration_target_seconds = max(0.0, args.calibration_seconds)

    try:
        if baseline is not None:
            emit_payload(
                {
                    "seq": 0,
                    "stage": "ready",
                    "status": "已加载基线",
                    "baseline": baseline_to_dict(baseline),
                    "baseline_file": args.baseline_in,
                },
                csv_writer,
                csv_fd,
                baseline,
            )

        with serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=8,
            parity="N",
            stopbits=1,
            timeout=1,
        ) as ser:
            while True:
                line = normalize_line(ser.readline())
                if not line:
                    continue

                try:
                    packet = parse_packet(line)
                except Exception as exc:  # noqa: BLE001
                    write_log_line(log_fd, f"PARSE_ERROR: {exc} | {line}")
                    continue

                if packet is None:
                    write_log_line(log_fd, line)
                    continue

                valid_count += 1
                summary = summarize_packet(packet=packet, seq=valid_count, include_samples=False)
                now = time.monotonic()

                if baseline is None:
                    if now < delay_end_time:
                        payload = {
                            "seq": valid_count,
                            "stage": "start_delay",
                            "status": "等待校准开始",
                            "remaining_seconds": round_seconds(delay_end_time - now),
                            "rssi": summary["rssi"],
                            "amplitude_mean": summary["amplitude_mean"],
                            "amplitude_std": summary["amplitude_std"],
                            "phase_std": summary["phase_std"],
                        }
                        emit_payload(payload, csv_writer, csv_fd, baseline)
                        if args.packets > 0 and valid_count >= args.packets:
                            break
                        continue

                    if calibration_start_time is None:
                        calibration_start_time = now

                    calibration_summaries.append(summary)

                    if calibration_mode == "seconds":
                        elapsed = now - calibration_start_time
                        remaining_seconds = round_seconds(calibration_target_seconds - elapsed)
                        progress = (
                            min(1.0, elapsed / calibration_target_seconds)
                            if calibration_target_seconds > 0
                            else 1.0
                        )
                        payload = {
                            "seq": valid_count,
                            "stage": "calibrating",
                            "status": "无人校准中",
                            "calibration_mode": calibration_mode,
                            "calibration_progress": f"{round(progress * 100, 1)}%",
                            "remaining_seconds": remaining_seconds,
                            "rssi": summary["rssi"],
                            "amplitude_mean": summary["amplitude_mean"],
                            "amplitude_std": summary["amplitude_std"],
                            "phase_std": summary["phase_std"],
                        }
                        emit_payload(payload, csv_writer, csv_fd, baseline)
                        calibration_done = elapsed >= calibration_target_seconds
                    else:
                        remaining_packets = max(0, args.calibration - len(calibration_summaries))
                        payload = {
                            "seq": valid_count,
                            "stage": "calibrating",
                            "status": "无人校准中",
                            "calibration_mode": calibration_mode,
                            "calibration_progress": f"{len(calibration_summaries)}/{args.calibration}",
                            "remaining_packets": remaining_packets,
                            "rssi": summary["rssi"],
                            "amplitude_mean": summary["amplitude_mean"],
                            "amplitude_std": summary["amplitude_std"],
                            "phase_std": summary["phase_std"],
                        }
                        emit_payload(payload, csv_writer, csv_fd, baseline)
                        calibration_done = len(calibration_summaries) >= args.calibration

                    if calibration_done:
                        baseline = compute_baseline(calibration_summaries)
                        save_baseline(args.baseline_out, baseline)
                        emit_payload(
                            {
                                "seq": valid_count,
                                "stage": "ready",
                                "status": "校准完成",
                                "baseline": baseline_to_dict(baseline),
                                "baseline_file": args.baseline_out or "",
                            },
                            csv_writer,
                            csv_fd,
                            baseline,
                        )

                    if args.packets > 0 and valid_count >= args.packets:
                        break
                    continue

                history.append(summary)
                if len(history) < history.maxlen:
                    payload = {
                        "seq": valid_count,
                        "stage": "buffering",
                        "status": "监测缓冲中",
                        "window_progress": f"{len(history)}/{history.maxlen}",
                        "rssi": summary["rssi"],
                        "amplitude_mean": summary["amplitude_mean"],
                        "amplitude_std": summary["amplitude_std"],
                        "phase_std": summary["phase_std"],
                    }
                else:
                    status, features = classify_status(baseline, history)
                    payload = {
                        "seq": valid_count,
                        "stage": "monitoring",
                        "status": status,
                        "mac": summary["mac"],
                        "rssi": summary["rssi"],
                        "channel": summary["channel"],
                        "features": features,
                    }

                emit_payload(payload, csv_writer, csv_fd, baseline)

                if args.packets > 0 and valid_count >= args.packets:
                    break
    finally:
        if log_fd is not None:
            log_fd.close()
        if csv_fd is not None:
            csv_fd.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
