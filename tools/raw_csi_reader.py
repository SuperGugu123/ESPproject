#!/usr/bin/env python3
"""Read raw CSI data from an ESP serial port and emit structured summaries.

This is a headless alternative to esp-csi's GUI helpers. It focuses on
programmatic integration:
1. Read `CSI_DATA,...` lines from a serial port.
2. Validate and parse CSI payloads.
3. Optionally save the original rows to CSV.
4. Print one JSON summary per valid packet to stdout.

The script does not try to estimate distance. It exposes raw/summarized CSI
features that a higher-level program can consume for custom logic.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from dataclasses import dataclass
from io import StringIO
from pathlib import Path

import serial


STANDARD_COLUMNS = [
    "type",
    "id",
    "mac",
    "rssi",
    "rate",
    "sig_mode",
    "mcs",
    "bandwidth",
    "smoothing",
    "not_sounding",
    "aggregation",
    "stbc",
    "fec_coding",
    "sgi",
    "noise_floor",
    "ampdu_cnt",
    "channel",
    "secondary_channel",
    "local_timestamp",
    "ant",
    "sig_len",
    "rx_state",
    "len",
    "first_word",
    "data",
]

C5C6_COLUMNS = [
    "type",
    "id",
    "mac",
    "rssi",
    "rate",
    "noise_floor",
    "fft_gain",
    "agc_gain",
    "channel",
    "local_timestamp",
    "sig_len",
    "rx_state",
    "len",
    "first_word",
    "data",
]


@dataclass
class ParsedPacket:
    row: dict[str, str]
    raw_samples: list[int]
    amplitudes: list[float]
    phases: list[float]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read raw ESP CSI data from a serial port and emit JSON summaries."
    )
    parser.add_argument("-p", "--port", required=True, help="Serial port, for example COM5")
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
        "--csv",
        dest="csv_path",
        default="",
        help="Optional path to save original valid CSI rows as CSV.",
    )
    parser.add_argument(
        "--log",
        dest="log_path",
        default="",
        help="Optional path to save non-CSI or malformed serial lines.",
    )
    parser.add_argument(
        "--include-samples",
        action="store_true",
        help="Include full raw CSI integer payload in each JSON line.",
    )
    return parser.parse_args()


def open_optional_text(path_str: str):
    if not path_str:
        return None
    path = Path(path_str)
    path.parent.mkdir(parents=True, exist_ok=True)
    return path.open("w", encoding="utf-8", newline="")


def normalize_line(raw: bytes) -> str:
    return raw.decode("utf-8", errors="ignore").strip()


def parse_csv_line(line: str) -> list[str]:
    return next(csv.reader(StringIO(line)))


def choose_columns(row_len: int) -> list[str] | None:
    if row_len == len(STANDARD_COLUMNS):
        return STANDARD_COLUMNS
    if row_len == len(C5C6_COLUMNS):
        return C5C6_COLUMNS
    return None


def parse_packet(line: str) -> ParsedPacket | None:
    marker = line.find("CSI_DATA")
    if marker < 0:
        return None

    row_values = parse_csv_line(line[marker:])
    columns = choose_columns(len(row_values))
    if columns is None:
        raise ValueError(f"unexpected column count: {len(row_values)}")

    row = dict(zip(columns, row_values))
    raw_samples = json.loads(row["data"])
    expected_len = int(row["len"])
    if expected_len != len(raw_samples):
        raise ValueError(
            f"payload length mismatch: expected {expected_len}, got {len(raw_samples)}"
        )
    if len(raw_samples) % 2 != 0:
        raise ValueError("CSI payload length must be even")

    amplitudes: list[float] = []
    phases: list[float] = []
    for i in range(0, len(raw_samples), 2):
        imag = raw_samples[i]
        real = raw_samples[i + 1]
        amplitudes.append(math.hypot(real, imag))
        phases.append(math.atan2(imag, real))

    return ParsedPacket(row=row, raw_samples=raw_samples, amplitudes=amplitudes, phases=phases)


def safe_int(row: dict[str, str], key: str, default: int = 0) -> int:
    value = row.get(key)
    if value in (None, ""):
        return default
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def summarize_packet(packet: ParsedPacket, seq: int, include_samples: bool) -> dict[str, object]:
    amplitudes = packet.amplitudes
    phases = packet.phases

    summary = {
        "seq": seq,
        "packet_id": safe_int(packet.row, "id"),
        "mac": packet.row.get("mac", ""),
        "rssi": safe_int(packet.row, "rssi"),
        "channel": safe_int(packet.row, "channel"),
        "secondary_channel": safe_int(packet.row, "secondary_channel", -1),
        "noise_floor": safe_int(packet.row, "noise_floor"),
        "timestamp": safe_int(packet.row, "local_timestamp"),
        "sample_count": len(packet.raw_samples),
        "subcarrier_count": len(amplitudes),
        "amplitude_mean": round(statistics.fmean(amplitudes), 4),
        "amplitude_std": round(statistics.pstdev(amplitudes), 4) if len(amplitudes) > 1 else 0.0,
        "amplitude_max": round(max(amplitudes), 4),
        "amplitude_min": round(min(amplitudes), 4),
        "phase_std": round(statistics.pstdev(phases), 4) if len(phases) > 1 else 0.0,
        "first_amplitudes": [round(v, 4) for v in amplitudes[:8]],
        "first_phases": [round(v, 4) for v in phases[:8]],
    }

    if "agc_gain" in packet.row:
        summary["agc_gain"] = safe_int(packet.row, "agc_gain")
    if "fft_gain" in packet.row:
        summary["fft_gain"] = safe_int(packet.row, "fft_gain")
    if include_samples:
        summary["raw_samples"] = packet.raw_samples

    return summary


def write_log_line(log_fd, line: str) -> None:
    if log_fd is None:
        return
    log_fd.write(line + "\n")
    log_fd.flush()


def print_json(obj: dict[str, object]) -> None:
    print(json.dumps(obj, ensure_ascii=False), flush=True)


def main() -> int:
    args = parse_args()

    csv_fd = open_optional_text(args.csv_path)
    log_fd = open_optional_text(args.log_path)
    csv_writer = None
    csv_columns: list[str] | None = None

    try:
        with serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=8,
            parity="N",
            stopbits=1,
            timeout=1,
        ) as ser:
            valid_count = 0

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

                if csv_fd is not None:
                    current_columns = list(packet.row.keys())
                    if csv_writer is None:
                        csv_writer = csv.writer(csv_fd)
                        csv_columns = current_columns
                        csv_writer.writerow(csv_columns)
                    if csv_columns != current_columns:
                        write_log_line(log_fd, f"CSV_SCHEMA_CHANGED: {current_columns}")
                    else:
                        csv_writer.writerow([packet.row[col] for col in csv_columns])
                        csv_fd.flush()

                summary = summarize_packet(
                    packet=packet,
                    seq=valid_count,
                    include_samples=args.include_samples,
                )
                print_json(summary)

                if args.packets > 0 and valid_count >= args.packets:
                    break
    finally:
        if csv_fd is not None:
            csv_fd.close()
        if log_fd is not None:
            log_fd.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
