#!/usr/bin/env python3
"""Realtime CSI plotting tool for the current ESPproject.

This script is intentionally self-contained and avoids external plotting
dependencies like matplotlib so it can run in this workspace with the
available Python environment.
"""

from __future__ import annotations

import argparse
import queue
import statistics
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
import sys
import tkinter as tk
from tkinter import messagebox, ttk

import serial

from raw_csi_reader import normalize_line, open_optional_text, parse_packet, summarize_packet


PLOT_BG = "#fbfaf6"
PANEL_BG = "#fffdf8"
GRID_COLOR = "#ddd5c7"
TEXT_COLOR = "#312c25"
WAVE_COLOR = "#b24a2c"
TREND_COLOR = "#1c7c75"
RSSI_COLOR = "#355cbe"
ACCENT_COLOR = "#8d6a2d"
BAD_COLOR = "#a12a2a"
GOOD_COLOR = "#25633b"


@dataclass
class PacketView:
    seq: int
    mac: str
    rssi: int
    channel: int
    amplitude_mean: float
    amplitude_std: float
    phase_std: float
    timestamp: int
    subcarrier_count: int
    amplitudes: list[float]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read CSI_DATA lines from serial and display realtime charts."
    )
    parser.add_argument("-p", "--port", required=True, help="Serial port, for example COM8")
    parser.add_argument(
        "-b",
        "--baud",
        type=int,
        default=115200,
        help="Serial baud rate, default: 115200",
    )
    parser.add_argument(
        "--history",
        type=int,
        default=180,
        help="Number of packets kept for trend charts, default: 180",
    )
    parser.add_argument(
        "--refresh-ms",
        type=int,
        default=120,
        help="UI refresh interval in milliseconds, default: 120",
    )
    parser.add_argument(
        "--log",
        dest="log_path",
        default="",
        help="Optional path to save malformed or non-CSI serial lines.",
    )
    return parser.parse_args()


class SerialReader(threading.Thread):
    def __init__(self, *, port: str, baud: int, event_queue: queue.Queue, log_path: str) -> None:
        super().__init__(daemon=True)
        self._port = port
        self._baud = baud
        self._event_queue = event_queue
        self._stop_event = threading.Event()
        self._log_path = log_path

    def stop(self) -> None:
        self._stop_event.set()

    def run(self) -> None:
        log_fd = open_optional_text(self._log_path)
        try:
            with serial.Serial(
                port=self._port,
                baudrate=self._baud,
                bytesize=8,
                parity="N",
                stopbits=1,
                timeout=1,
            ) as ser:
                self._event_queue.put(
                    {
                        "kind": "status",
                        "message": f"已连接串口 {self._port} @ {self._baud}",
                        "level": "info",
                    }
                )

                valid_count = 0
                parse_errors = 0
                ignored_lines = 0
                last_heartbeat = 0.0

                while not self._stop_event.is_set():
                    line = normalize_line(ser.readline())
                    if not line:
                        now = time.monotonic()
                        if now - last_heartbeat > 1.5:
                            last_heartbeat = now
                            self._event_queue.put(
                                {
                                    "kind": "heartbeat",
                                    "parse_errors": parse_errors,
                                    "ignored_lines": ignored_lines,
                                }
                            )
                        continue

                    try:
                        packet = parse_packet(line)
                    except Exception as exc:  # noqa: BLE001
                        parse_errors += 1
                        if log_fd is not None:
                            log_fd.write(f"PARSE_ERROR: {exc} | {line}\n")
                            log_fd.flush()
                        self._event_queue.put(
                            {
                                "kind": "parse_error",
                                "message": str(exc),
                                "parse_errors": parse_errors,
                            }
                        )
                        continue

                    if packet is None:
                        ignored_lines += 1
                        if log_fd is not None:
                            log_fd.write(line + "\n")
                            log_fd.flush()
                        continue

                    valid_count += 1
                    summary = summarize_packet(packet=packet, seq=valid_count, include_samples=False)
                    view = PacketView(
                        seq=valid_count,
                        mac=str(summary["mac"]),
                        rssi=int(summary["rssi"]),
                        channel=int(summary["channel"]),
                        amplitude_mean=float(summary["amplitude_mean"]),
                        amplitude_std=float(summary["amplitude_std"]),
                        phase_std=float(summary["phase_std"]),
                        timestamp=int(summary["timestamp"]),
                        subcarrier_count=int(summary["subcarrier_count"]),
                        amplitudes=packet.amplitudes,
                    )
                    self._event_queue.put(
                        {
                            "kind": "packet",
                            "packet": view,
                            "parse_errors": parse_errors,
                            "ignored_lines": ignored_lines,
                        }
                    )
        except Exception as exc:  # noqa: BLE001
            self._event_queue.put({"kind": "fatal", "message": str(exc)})
        finally:
            if log_fd is not None:
                log_fd.close()


class CSIPlotApp:
    def __init__(self, root: tk.Tk, args: argparse.Namespace) -> None:
        self.root = root
        self.args = args
        self.root.title(f"CSI Realtime Plot - {args.port}")
        self.root.geometry("1380x880")
        self.root.configure(bg=PLOT_BG)
        self.root.minsize(1080, 720)

        self.event_queue: queue.Queue = queue.Queue()
        self.reader = SerialReader(
            port=args.port,
            baud=args.baud,
            event_queue=self.event_queue,
            log_path=args.log_path,
        )

        self.latest_packet: PacketView | None = None
        self.amplitude_history: deque[float] = deque(maxlen=max(20, args.history))
        self.amplitude_std_history: deque[float] = deque(maxlen=max(20, args.history))
        self.phase_std_history: deque[float] = deque(maxlen=max(20, args.history))
        self.rssi_history: deque[float] = deque(maxlen=max(20, args.history))

        self.packet_count = 0
        self.parse_errors = 0
        self.ignored_lines = 0
        self.last_packet_time = 0.0
        self.last_status_message = "等待串口数据..."
        self.last_error_message = ""
        self.is_paused = False

        self.status_text = tk.StringVar(value=self.last_status_message)
        self.packet_text = tk.StringVar(value="有效包: 0")
        self.detail_text = tk.StringVar(value="RSSI: -- | 通道: -- | 子载波: --")
        self.metrics_text = tk.StringVar(value="振幅均值: -- | 振幅标准差: -- | 相位标准差: --")
        self.health_text = tk.StringVar(value="解析错误: 0 | 忽略行: 0")

        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.bind("<space>", self.toggle_pause)

    def _build_ui(self) -> None:
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        style.configure("Card.TFrame", background=PANEL_BG)
        style.configure("Info.TLabel", background=PANEL_BG, foreground=TEXT_COLOR, font=("Microsoft YaHei UI", 11))
        style.configure("Title.TLabel", background=PANEL_BG, foreground=TEXT_COLOR, font=("Microsoft YaHei UI", 15, "bold"))
        style.configure("Header.TLabel", background=PLOT_BG, foreground=TEXT_COLOR, font=("Microsoft YaHei UI", 18, "bold"))

        header = ttk.Frame(self.root, style="Card.TFrame", padding=(18, 14))
        header.pack(fill="x", padx=14, pady=(14, 8))

        left = ttk.Frame(header, style="Card.TFrame")
        left.pack(side="left", fill="both", expand=True)
        ttk.Label(left, text="ESP CSI 实时图表", style="Header.TLabel").pack(anchor="w")
        ttk.Label(
            left,
            text=f"串口 {self.args.port} | 波特率 {self.args.baud} | 空格键暂停/恢复",
            style="Info.TLabel",
        ).pack(anchor="w", pady=(4, 0))

        right = ttk.Frame(header, style="Card.TFrame")
        right.pack(side="right", anchor="e")
        self.pause_button = tk.Button(
            right,
            text="暂停",
            command=self.toggle_pause,
            bg=ACCENT_COLOR,
            fg="white",
            activebackground="#6d5323",
            activeforeground="white",
            relief="flat",
            padx=16,
            pady=8,
            font=("Microsoft YaHei UI", 10, "bold"),
        )
        self.pause_button.pack(side="right")

        info = ttk.Frame(self.root, style="Card.TFrame", padding=(18, 14))
        info.pack(fill="x", padx=14, pady=(0, 8))
        ttk.Label(info, textvariable=self.status_text, style="Title.TLabel").pack(anchor="w")
        ttk.Label(info, textvariable=self.packet_text, style="Info.TLabel").pack(anchor="w", pady=(6, 0))
        ttk.Label(info, textvariable=self.detail_text, style="Info.TLabel").pack(anchor="w", pady=(4, 0))
        ttk.Label(info, textvariable=self.metrics_text, style="Info.TLabel").pack(anchor="w", pady=(4, 0))
        ttk.Label(info, textvariable=self.health_text, style="Info.TLabel").pack(anchor="w", pady=(4, 0))

        body = ttk.Frame(self.root, style="Card.TFrame")
        body.pack(fill="both", expand=True, padx=14, pady=(0, 14))
        body.columnconfigure(0, weight=2)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(0, weight=1)
        body.rowconfigure(1, weight=1)

        self.wave_canvas = tk.Canvas(body, bg=PANEL_BG, highlightthickness=0)
        self.wave_canvas.grid(row=0, column=0, sticky="nsew", padx=(0, 8), pady=(0, 8))

        self.amp_canvas = tk.Canvas(body, bg=PANEL_BG, highlightthickness=0)
        self.amp_canvas.grid(row=1, column=0, sticky="nsew", padx=(0, 8))

        self.rssi_canvas = tk.Canvas(body, bg=PANEL_BG, highlightthickness=0)
        self.rssi_canvas.grid(row=0, column=1, rowspan=2, sticky="nsew")

    def start(self) -> None:
        self.reader.start()
        self.root.after(self.args.refresh_ms, self.refresh)

    def on_close(self) -> None:
        self.reader.stop()
        self.root.after(100, self.root.destroy)

    def toggle_pause(self, _event=None) -> None:
        self.is_paused = not self.is_paused
        self.pause_button.configure(text="恢复" if self.is_paused else "暂停")
        if self.is_paused:
            self.status_text.set("图表已暂停，串口线程仍在接收数据")
        else:
            self.status_text.set(self.last_status_message)

    def refresh(self) -> None:
        drained = False
        while True:
            try:
                event = self.event_queue.get_nowait()
            except queue.Empty:
                break
            drained = True
            self.handle_event(event)

        self.draw_all()

        if not self.is_paused and not drained and self.last_packet_time:
            age = time.monotonic() - self.last_packet_time
            if age > 2.5:
                self.status_text.set(f"{self.last_status_message} | 最近 {age:.1f}s 没有新包")

        self.root.after(self.args.refresh_ms, self.refresh)

    def handle_event(self, event: dict[str, object]) -> None:
        kind = event["kind"]
        if kind == "fatal":
            message = str(event["message"])
            self.status_text.set(f"串口打开失败: {message}")
            self.health_text.set(
                f"解析错误: {self.parse_errors} | 忽略行: {self.ignored_lines} | 状态: 打开失败"
            )
            messagebox.showerror("CSI Plot Tool", f"无法打开串口 {self.args.port}\n\n{message}")
            return

        if kind == "status":
            self.last_status_message = str(event["message"])
            if not self.is_paused:
                self.status_text.set(self.last_status_message)
            return

        if kind == "heartbeat":
            self.parse_errors = int(event["parse_errors"])
            self.ignored_lines = int(event["ignored_lines"])
            self.update_health_text()
            return

        if kind == "parse_error":
            self.parse_errors = int(event["parse_errors"])
            self.last_error_message = str(event["message"])
            self.update_health_text()
            return

        if kind != "packet":
            return

        self.parse_errors = int(event["parse_errors"])
        self.ignored_lines = int(event["ignored_lines"])
        packet = event["packet"]
        if not isinstance(packet, PacketView):
            return

        self.latest_packet = packet
        self.packet_count = packet.seq
        self.last_packet_time = time.monotonic()

        self.amplitude_history.append(packet.amplitude_mean)
        self.amplitude_std_history.append(packet.amplitude_std)
        self.phase_std_history.append(packet.phase_std)
        self.rssi_history.append(packet.rssi)

        self.last_status_message = f"正在接收 CSI 数据 | MAC {packet.mac}"
        if not self.is_paused:
            self.status_text.set(self.last_status_message)

        self.packet_text.set(f"有效包: {packet.seq}")
        self.detail_text.set(
            f"RSSI: {packet.rssi} dBm | 通道: {packet.channel} | 子载波: {packet.subcarrier_count} | 时间戳: {packet.timestamp}"
        )
        self.metrics_text.set(
            f"振幅均值: {packet.amplitude_mean:.3f} | 振幅标准差: {packet.amplitude_std:.3f} | 相位标准差: {packet.phase_std:.3f}"
        )
        self.update_health_text()

    def update_health_text(self) -> None:
        extra = f" | 最近错误: {self.last_error_message}" if self.last_error_message else ""
        self.health_text.set(
            f"解析错误: {self.parse_errors} | 忽略行: {self.ignored_lines}{extra}"
        )

    def draw_all(self) -> None:
        self.draw_wave_canvas()
        self.draw_amp_canvas()
        self.draw_rssi_canvas()

    def draw_wave_canvas(self) -> None:
        canvas = self.wave_canvas
        values = self.latest_packet.amplitudes if self.latest_packet else []
        subtitle = (
            f"最新一包的子载波振幅 | seq {self.latest_packet.seq}"
            if self.latest_packet is not None
            else "等待有效 CSI 包..."
        )
        self.draw_single_series_plot(
            canvas=canvas,
            title="CSI 振幅波形",
            subtitle=subtitle,
            values=values,
            color=WAVE_COLOR,
            fill=True,
        )

    def draw_amp_canvas(self) -> None:
        amp_values = list(self.amplitude_history)
        amp_std_values = list(self.amplitude_std_history)
        subtitle = "历史趋势: 振幅均值与振幅标准差"
        self.draw_multi_series_plot(
            canvas=self.amp_canvas,
            title="振幅趋势",
            subtitle=subtitle,
            series=[
                ("振幅均值", amp_values, TREND_COLOR),
                ("振幅标准差", amp_std_values, ACCENT_COLOR),
            ],
        )

    def draw_rssi_canvas(self) -> None:
        rssi_values = list(self.rssi_history)
        phase_values = list(self.phase_std_history)
        subtitle = "历史趋势: RSSI 与相位标准差"
        self.draw_multi_series_plot(
            canvas=self.rssi_canvas,
            title="链路与抖动趋势",
            subtitle=subtitle,
            series=[
                ("RSSI", rssi_values, RSSI_COLOR),
                ("相位标准差", phase_values, GOOD_COLOR),
            ],
        )

    def draw_single_series_plot(
        self,
        *,
        canvas: tk.Canvas,
        title: str,
        subtitle: str,
        values: list[float],
        color: str,
        fill: bool,
    ) -> None:
        canvas.delete("all")
        width = max(canvas.winfo_width(), 100)
        height = max(canvas.winfo_height(), 100)
        self.draw_panel_background(canvas, width, height, title, subtitle)

        if not values:
            canvas.create_text(
                width / 2,
                height / 2,
                text="暂无数据",
                fill=TEXT_COLOR,
                font=("Microsoft YaHei UI", 16, "bold"),
            )
            return

        left, top, right, bottom = 56, 66, width - 20, height - 32
        self.draw_grid(canvas, left, top, right, bottom)
        points, y_min, y_max = self.make_points(values, left, top, right, bottom)

        if fill and len(points) >= 4:
            fill_points = [left, bottom] + points + [right, bottom]
            canvas.create_polygon(fill_points, fill="#f0d7cf", outline="")
        canvas.create_line(*points, fill=color, width=2.5, smooth=False)
        self.draw_y_labels(canvas, left, top, bottom, y_min, y_max)
        self.draw_series_stats(canvas, right, top - 18, values, color)

    def draw_multi_series_plot(
        self,
        *,
        canvas: tk.Canvas,
        title: str,
        subtitle: str,
        series: list[tuple[str, list[float], str]],
    ) -> None:
        canvas.delete("all")
        width = max(canvas.winfo_width(), 100)
        height = max(canvas.winfo_height(), 100)
        self.draw_panel_background(canvas, width, height, title, subtitle)

        non_empty = [values for _name, values, _color in series if values]
        if not non_empty:
            canvas.create_text(
                width / 2,
                height / 2,
                text="暂无数据",
                fill=TEXT_COLOR,
                font=("Microsoft YaHei UI", 16, "bold"),
            )
            return

        left, top, right, bottom = 56, 78, width - 20, height - 36
        self.draw_grid(canvas, left, top, right, bottom)

        merged = [item for values in non_empty for item in values]
        y_min = min(merged)
        y_max = max(merged)
        if y_min == y_max:
            span = abs(y_min) * 0.1 or 1.0
            y_min -= span
            y_max += span
        else:
            pad = (y_max - y_min) * 0.12
            y_min -= pad
            y_max += pad

        for index, (name, values, color) in enumerate(series):
            if not values:
                continue
            points = self.make_points_with_range(values, left, top, right, bottom, y_min, y_max)
            if len(points) >= 4:
                canvas.create_line(*points, fill=color, width=2.2, smooth=False)

            legend_x = left + index * 160
            canvas.create_line(legend_x, 46, legend_x + 18, 46, fill=color, width=3)
            canvas.create_text(
                legend_x + 26,
                46,
                text=name,
                anchor="w",
                fill=TEXT_COLOR,
                font=("Microsoft YaHei UI", 10),
            )

        self.draw_y_labels(canvas, left, top, bottom, y_min, y_max)

    def draw_panel_background(self, canvas: tk.Canvas, width: int, height: int, title: str, subtitle: str) -> None:
        canvas.create_rectangle(0, 0, width, height, fill=PANEL_BG, outline="")
        canvas.create_text(
            18,
            18,
            text=title,
            anchor="nw",
            fill=TEXT_COLOR,
            font=("Microsoft YaHei UI", 16, "bold"),
        )
        canvas.create_text(
            18,
            42,
            text=subtitle,
            anchor="nw",
            fill="#6e675e",
            font=("Microsoft YaHei UI", 10),
        )

    def draw_grid(self, canvas: tk.Canvas, left: int, top: int, right: int, bottom: int) -> None:
        canvas.create_rectangle(left, top, right, bottom, outline=GRID_COLOR, width=1)
        steps = 5
        for i in range(1, steps):
            y = top + (bottom - top) * i / steps
            canvas.create_line(left, y, right, y, fill=GRID_COLOR, dash=(2, 4))
        for i in range(1, steps):
            x = left + (right - left) * i / steps
            canvas.create_line(x, top, x, bottom, fill=GRID_COLOR, dash=(2, 4))

    def draw_y_labels(
        self,
        canvas: tk.Canvas,
        left: int,
        top: int,
        bottom: int,
        y_min: float,
        y_max: float,
    ) -> None:
        for i in range(6):
            y = bottom - (bottom - top) * i / 5
            value = y_min + (y_max - y_min) * i / 5
            canvas.create_text(
                left - 8,
                y,
                text=f"{value:.2f}",
                anchor="e",
                fill="#6e675e",
                font=("Consolas", 9),
            )

    def draw_series_stats(self, canvas: tk.Canvas, x: int, y: int, values: list[float], color: str) -> None:
        if not values:
            return
        mean_value = statistics.fmean(values)
        max_value = max(values)
        min_value = min(values)
        canvas.create_text(
            x,
            y,
            text=f"mean {mean_value:.2f}  min {min_value:.2f}  max {max_value:.2f}",
            anchor="ne",
            fill=color,
            font=("Consolas", 10, "bold"),
        )

    def make_points(
        self,
        values: list[float],
        left: int,
        top: int,
        right: int,
        bottom: int,
    ) -> tuple[list[float], float, float]:
        y_min = min(values)
        y_max = max(values)
        if y_min == y_max:
            span = abs(y_min) * 0.1 or 1.0
            y_min -= span
            y_max += span
        else:
            pad = (y_max - y_min) * 0.1
            y_min -= pad
            y_max += pad
        points = self.make_points_with_range(values, left, top, right, bottom, y_min, y_max)
        return points, y_min, y_max

    def make_points_with_range(
        self,
        values: list[float],
        left: int,
        top: int,
        right: int,
        bottom: int,
        y_min: float,
        y_max: float,
    ) -> list[float]:
        if len(values) == 1:
            x = (left + right) / 2
            y = self.scale_value(values[0], y_min, y_max, top, bottom)
            return [x - 1, y, x + 1, y]

        points: list[float] = []
        span = len(values) - 1
        for index, value in enumerate(values):
            x = left + (right - left) * index / span
            y = self.scale_value(value, y_min, y_max, top, bottom)
            points.extend([x, y])
        return points

    @staticmethod
    def scale_value(value: float, y_min: float, y_max: float, top: int, bottom: int) -> float:
        if y_max == y_min:
            return (top + bottom) / 2
        ratio = (value - y_min) / (y_max - y_min)
        return bottom - ratio * (bottom - top)


def main() -> int:
    args = parse_args()

    if args.log_path:
        path = Path(args.log_path)
        path.parent.mkdir(parents=True, exist_ok=True)

    root = tk.Tk()
    app = CSIPlotApp(root, args)
    app.start()
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
