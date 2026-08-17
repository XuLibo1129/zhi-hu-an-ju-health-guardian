from __future__ import annotations

import csv
import math
import queue
import re
import socket
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports
import tkinter as tk
from tkinter import filedialog, font as tkfont, messagebox, ttk


CHANNEL_NAMES = ["信号强度", "平均幅度", "子载波8", "子载波24", "子载波48", "子载波80"]
CHANNEL_COLORS = ["#61afef", "#98c379", "#e5c07b", "#c678dd", "#56b6c2", "#e06c75"]
DEFAULT_MAX_POINTS = 800
CSI_BRACKET_RE = re.compile(r"\[([^\]]+)\]")


@dataclass
class Sample:
    t: float
    values: list[float]
    raw: str


def parse_number_list(text: str) -> list[float] | None:
    parts = [part.strip() for part in text.split(",")]
    if len(parts) < 2:
        return None

    values: list[float] = []
    for part in parts:
        if not part:
            return None
        try:
            values.append(float(part))
        except ValueError:
            return None

    return values


def amp2_from_pair(values: list[int], pair_index: int) -> float:
    idx = pair_index * 2
    if idx + 1 >= len(values):
        return 0.0
    imag = values[idx]
    real = values[idx + 1]
    return float(real * real + imag * imag)


def mean_amp2(values: list[int]) -> float:
    total = 0
    count = 0
    for idx in range(0, len(values) - 1, 2):
        imag = values[idx]
        real = values[idx + 1]
        if imag == 0 and real == 0:
            continue
        total += real * real + imag * imag
        count += 1
    return float(total / count) if count else 0.0


def parse_csi_data_line(line: str) -> list[float] | None:
    match = CSI_BRACKET_RE.search(line)
    if not match:
        return None

    prefix = line[: match.start()].rstrip(",")
    fields = [part.strip() for part in prefix.split(",")]
    if len(fields) < 4 or fields[0] != "CSI_DATA":
        return None

    try:
        rssi = float(fields[3])
        raw_values = [int(part.strip()) for part in match.group(1).split(",") if part.strip()]
    except ValueError:
        return None

    return [
        rssi,
        mean_amp2(raw_values),
        amp2_from_pair(raw_values, 8),
        amp2_from_pair(raw_values, 24),
        amp2_from_pair(raw_values, 48),
        amp2_from_pair(raw_values, 80),
    ]


def parse_serial_line(line: str) -> list[float] | None:
    stripped = line.strip()
    if not stripped:
        return None

    if stripped.startswith("CSI_DATA,"):
        return parse_csi_data_line(stripped)

    values = parse_number_list(stripped)
    if values is None:
        return None

    if len(values) >= len(CHANNEL_NAMES):
        return values[: len(CHANNEL_NAMES)]

    padded = values[:]
    while len(padded) < len(CHANNEL_NAMES):
        padded.append(0.0)
    return padded


class SerialReader(threading.Thread):
    def __init__(self, port: str, baud: int, out_queue: queue.Queue[Sample | str]):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.out_queue = out_queue
        self.stop_event = threading.Event()
        self.serial_obj: serial.Serial | None = None
        self.started_at = time.perf_counter()

    def run(self) -> None:
        try:
            self.serial_obj = serial.Serial(self.port, self.baud, timeout=0.1)
        except Exception as exc:
            self.out_queue.put(f"错误：打开 {self.port} 失败：{exc}")
            return

        self.out_queue.put(f"已连接：{self.port} @ {self.baud}")
        while not self.stop_event.is_set():
            try:
                raw = self.serial_obj.readline()
            except Exception as exc:
                self.out_queue.put(f"错误：串口读取失败：{exc}")
                break

            if not raw:
                continue

            line = raw.decode("utf-8", errors="ignore").strip()
            values = parse_serial_line(line)
            if values is not None:
                self.out_queue.put(Sample(time.perf_counter(), values, line))

        try:
            self.serial_obj.close()
        except Exception:
            pass
        self.out_queue.put("已断开连接")

    def stop(self) -> None:
        self.stop_event.set()


class UdpReader(threading.Thread):
    def __init__(self, port: int, out_queue: queue.Queue[Sample | str]):
        super().__init__(daemon=True)
        self.port = port
        self.out_queue = out_queue
        self.stop_event = threading.Event()
        self.sock: socket.socket | None = None

    def run(self) -> None:
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.bind(("0.0.0.0", self.port))
            self.sock.settimeout(0.1)
        except Exception as exc:
            self.out_queue.put(f"错误：UDP {self.port} 打开失败：{exc}")
            return

        self.out_queue.put(f"已连接：UDP 0.0.0.0:{self.port}")
        while not self.stop_event.is_set():
            try:
                packet, _addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except Exception as exc:
                self.out_queue.put(f"错误：UDP读取失败：{exc}")
                break

            line_text = packet.decode("utf-8", errors="ignore")
            for line in line_text.splitlines():
                values = parse_serial_line(line)
                if values is not None:
                    self.out_queue.put(Sample(time.perf_counter(), values, line))

        try:
            self.sock.close()
        except Exception:
            pass
        self.out_queue.put("已断开连接")

    def stop(self) -> None:
        self.stop_event.set()


class PlotCanvas(tk.Canvas):
    def __init__(self, parent: tk.Misc, **kwargs):
        super().__init__(parent, **kwargs)
        self.configure(bg="#101418", highlightthickness=0)
        self.samples: deque[Sample] = deque(maxlen=DEFAULT_MAX_POINTS)
        self.enabled = [tk.BooleanVar(value=True) for _ in CHANNEL_NAMES]
        self.auto_scale = tk.BooleanVar(value=True)
        self.manual_min = tk.DoubleVar(value=0.0)
        self.manual_max = tk.DoubleVar(value=8000.0)
        self.show_points = tk.IntVar(value=DEFAULT_MAX_POINTS)

    def set_max_points(self, count: int) -> None:
        count = max(50, min(5000, count))
        old = list(self.samples)[-count:]
        self.samples = deque(old, maxlen=count)

    def add_sample(self, sample: Sample) -> None:
        self.samples.append(sample)

    def clear_samples(self) -> None:
        self.samples.clear()
        self.redraw()

    def redraw(self) -> None:
        self.delete("all")
        width = max(self.winfo_width(), 200)
        height = max(self.winfo_height(), 200)
        margin_left = 94
        margin_right = 14
        margin_top = 16
        margin_bottom = 18
        plot_width = max(width - margin_left - margin_right, 10)
        plot_height = max(height - margin_top - margin_bottom, 10)

        enabled_indices = [i for i, enabled in enumerate(self.enabled) if enabled.get()]
        if not enabled_indices:
            self.create_text(width / 2, height / 2, fill="#a0a8b0", text="未选择通道")
            return

        lane_height = plot_height / len(enabled_indices)
        visible_samples = list(self.samples)

        self.create_rectangle(0, 0, width, height, fill="#101418", outline="")
        for grid_i in range(0, 6):
            x = margin_left + plot_width * grid_i / 5
            self.create_line(x, margin_top, x, margin_top + plot_height, fill="#202832")

        for lane_no, ch in enumerate(enabled_indices):
            lane_top = margin_top + lane_no * lane_height
            lane_bottom = lane_top + lane_height
            mid_y = (lane_top + lane_bottom) / 2
            self.create_line(margin_left, lane_bottom, margin_left + plot_width, lane_bottom, fill="#26313d")
            self.create_text(8, mid_y, anchor="w", fill=CHANNEL_COLORS[ch], text=CHANNEL_NAMES[ch])

            vals = [sample.values[ch] for sample in visible_samples if ch < len(sample.values)]
            if not vals:
                continue

            if self.auto_scale.get():
                vmin = min(vals)
                vmax = max(vals)
                if math.isclose(vmin, vmax):
                    pad = max(abs(vmax) * 0.1, 1.0)
                    vmin -= pad
                    vmax += pad
                else:
                    pad = (vmax - vmin) * 0.12
                    vmin -= pad
                    vmax += pad
            else:
                vmin = self.manual_min.get()
                vmax = self.manual_max.get()
                if vmax <= vmin:
                    vmax = vmin + 1.0

            self.create_text(margin_left - 8, lane_top + 10, anchor="e", fill="#7d8790", text=f"{vmax:.0f}")
            self.create_text(margin_left - 8, lane_bottom - 10, anchor="e", fill="#7d8790", text=f"{vmin:.0f}")

            if len(visible_samples) == 1:
                x = margin_left
                y = self.map_y(vals[-1], vmin, vmax, lane_top, lane_bottom)
                self.create_oval(x - 2, y - 2, x + 2, y + 2, fill=CHANNEL_COLORS[ch], outline="")
                continue

            points: list[float] = []
            denom = max(len(visible_samples) - 1, 1)
            for idx, sample in enumerate(visible_samples):
                val = sample.values[ch]
                x = margin_left + plot_width * idx / denom
                y = self.map_y(val, vmin, vmax, lane_top + 4, lane_bottom - 4)
                points.extend([x, y])
            if len(points) >= 4:
                self.create_line(*points, fill=CHANNEL_COLORS[ch], width=1.5)

            latest = vals[-1]
            self.create_text(width - 8, mid_y, anchor="e", fill=CHANNEL_COLORS[ch], text=f"{latest:.0f}")

    @staticmethod
    def map_y(value: float, vmin: float, vmax: float, lane_top: float, lane_bottom: float) -> float:
        ratio = (value - vmin) / (vmax - vmin)
        ratio = max(0.0, min(1.0, ratio))
        return lane_bottom - ratio * (lane_bottom - lane_top)


class CsiViewerApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("ESP32 CSI 无线感知波形工具")
        self.root.geometry("1180x760")
        self.root.minsize(900, 560)

        self.queue: queue.Queue[Sample | str] = queue.Queue()
        self.reader: SerialReader | UdpReader | None = None
        self.record_file = None
        self.record_writer: csv.writer | None = None
        self.record_started_at = 0.0
        self.record_duration_s = 8.0
        self.record_stop_job: str | None = None
        self.current_label = tk.StringVar(value="空房")
        self.status_text = tk.StringVar(value="就绪")
        self.sample_count = 0
        self.last_rate_time = time.perf_counter()
        self.last_rate_count = 0
        self.hz_text = tk.StringVar(value="0 Hz")
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.udp_port_var = tk.StringVar(value="34567")
        self.duration_var = tk.IntVar(value=8)

        self.build_ui()
        self.refresh_ports()
        self.root.after(25, self.poll_queue)
        self.root.after(60, self.redraw_plot)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def build_ui(self) -> None:
        self.root.configure(bg="#171b20")
        self.configure_fonts()

        top = ttk.Frame(self.root, padding=8)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="串口").pack(side=tk.LEFT)
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=18, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(6, 8))
        ttk.Button(top, text="刷新", command=self.refresh_ports).pack(side=tk.LEFT, padx=(0, 12))

        ttk.Label(top, text="波特率").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.baud_var, width=10).pack(side=tk.LEFT, padx=(6, 8))
        self.connect_button = ttk.Button(top, text="连接", command=self.toggle_connection)
        self.connect_button.pack(side=tk.LEFT, padx=(0, 12))

        ttk.Label(top, text="UDP").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.udp_port_var, width=7).pack(side=tk.LEFT, padx=(6, 8))
        self.udp_button = ttk.Button(top, text="UDP连接", command=self.toggle_udp_connection)
        self.udp_button.pack(side=tk.LEFT, padx=(0, 12))

        ttk.Label(top, text="显示点数").pack(side=tk.LEFT)
        self.points_spin = ttk.Spinbox(top, from_=50, to=5000, increment=50, width=8, command=self.apply_points)
        self.points_spin.set(str(DEFAULT_MAX_POINTS))
        self.points_spin.pack(side=tk.LEFT, padx=(6, 12))

        ttk.Label(top, text="采集时长(s)").pack(side=tk.LEFT)
        self.duration_spin = ttk.Spinbox(top, from_=1, to=120, increment=1, width=6,
                                         textvariable=self.duration_var)
        self.duration_spin.pack(side=tk.LEFT, padx=(6, 8))

        self.record_button = ttk.Button(top, text="开始记录", command=self.toggle_recording)
        self.record_button.pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(top, text="清空", command=self.clear_plot).pack(side=tk.LEFT)

        ttk.Label(top, textvariable=self.hz_text).pack(side=tk.RIGHT, padx=(10, 0))

        main = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

        left = ttk.Frame(main, padding=8)
        main.add(left, weight=0)

        self.plot = PlotCanvas(main)
        main.add(self.plot, weight=1)

        ttk.Label(left, text="显示通道").pack(anchor="w")
        for idx, name in enumerate(CHANNEL_NAMES):
            cb = ttk.Checkbutton(left, text=name, variable=self.plot.enabled[idx], command=self.plot.redraw)
            cb.pack(anchor="w", pady=2)

        ttk.Separator(left).pack(fill=tk.X, pady=10)
        ttk.Checkbutton(left, text="自动缩放", variable=self.plot.auto_scale, command=self.plot.redraw).pack(anchor="w")

        scale_frame = ttk.Frame(left)
        scale_frame.pack(fill=tk.X, pady=(6, 0))
        ttk.Label(scale_frame, text="纵轴最小").grid(row=0, column=0, sticky="w")
        ttk.Entry(scale_frame, textvariable=self.plot.manual_min, width=10).grid(row=0, column=1, padx=(6, 0))
        ttk.Label(scale_frame, text="纵轴最大").grid(row=1, column=0, sticky="w", pady=(4, 0))
        ttk.Entry(scale_frame, textvariable=self.plot.manual_max, width=10).grid(row=1, column=1, padx=(6, 0), pady=(4, 0))

        ttk.Separator(left).pack(fill=tk.X, pady=10)
        ttk.Label(left, text="当前动作标签").pack(anchor="w")
        labels = ["空房", "走路", "坐下", "站立", "跌倒", "弯腰捡东西", "其他"]
        for label in labels:
            ttk.Radiobutton(left, text=label, value=label, variable=self.current_label).pack(anchor="w", pady=1)

        ttk.Separator(left).pack(fill=tk.X, pady=10)
        ttk.Label(left, text="使用提示").pack(anchor="w")
        hint = "选择 A 板串口。\n固件 VOFA 模式输出：\nRSSI、平均幅度、子载波8/24/48/80"
        ttk.Label(left, text=hint, justify=tk.LEFT).pack(anchor="w")

        bottom = ttk.Frame(self.root, padding=(8, 0, 8, 8))
        bottom.pack(side=tk.BOTTOM, fill=tk.X)
        ttk.Label(bottom, textvariable=self.status_text).pack(side=tk.LEFT)

    def configure_fonts(self) -> None:
        font_family = "Microsoft YaHei UI"
        for font_name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont"):
            try:
                tkfont.nametofont(font_name).configure(family=font_family, size=10)
            except tk.TclError:
                pass
        style = ttk.Style(self.root)
        style.configure(".", font=(font_family, 10))

    def refresh_ports(self) -> None:
        ports = list(list_ports.comports())
        items = [f"{port.device} - {port.description}" for port in ports]
        self.port_combo["values"] = items
        if items and not self.port_var.get():
            self.port_var.set(items[0])
        if not items:
            self.status_text.set("未发现串口")

    def selected_port(self) -> str:
        text = self.port_var.get().strip()
        return text.split(" - ", 1)[0] if text else ""

    def toggle_connection(self) -> None:
        if self.reader:
            self.disconnect()
        else:
            self.connect()

    def toggle_udp_connection(self) -> None:
        if self.reader:
            self.disconnect()
        else:
            self.connect_udp()

    def connect(self) -> None:
        port = self.selected_port()
        if not port:
            messagebox.showwarning("未选择串口", "请先选择 A 板对应的串口。")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("波特率无效", "波特率必须是数字。")
            return

        self.reader = SerialReader(port, baud, self.queue)
        self.reader.start()
        self.connect_button.configure(text="断开")
        self.udp_button.configure(state="disabled")
        self.status_text.set(f"正在连接 {port}...")

    def connect_udp(self) -> None:
        try:
            udp_port = int(self.udp_port_var.get())
        except ValueError:
            messagebox.showwarning("UDP端口无效", "UDP端口必须是数字。")
            return
        if not (1 <= udp_port <= 65535):
            messagebox.showwarning("UDP端口无效", "UDP端口范围必须是 1-65535。")
            return

        self.reader = UdpReader(udp_port, self.queue)
        self.reader.start()
        self.connect_button.configure(state="disabled")
        self.udp_button.configure(text="断开")
        self.status_text.set(f"正在监听 UDP {udp_port}...")

    def disconnect(self) -> None:
        if self.reader:
            self.reader.stop()
            self.reader = None
        self.connect_button.configure(text="连接", state="normal")
        self.udp_button.configure(text="UDP连接", state="normal")

    def apply_points(self) -> None:
        try:
            count = int(self.points_spin.get())
        except ValueError:
            return
        self.plot.set_max_points(count)
        self.plot.redraw()

    def clear_plot(self) -> None:
        self.plot.clear_samples()
        self.sample_count = 0
        self.last_rate_count = 0
        self.status_text.set("波形已清空")

    def toggle_recording(self) -> None:
        if self.record_writer:
            self.stop_recording()
        else:
            self.start_recording()

    def start_recording(self) -> None:
        try:
            duration_s = int(self.duration_var.get())
        except (tk.TclError, ValueError):
            messagebox.showwarning("采集时长无效", "采集时长必须是 1 到 120 秒之间的整数。")
            return
        if duration_s < 1 or duration_s > 120:
            messagebox.showwarning("采集时长无效", "采集时长必须是 1 到 120 秒之间的整数。")
            return

        data_dir = Path(__file__).with_name("data")
        data_dir.mkdir(exist_ok=True)
        default_name = f"csi_{datetime.now().strftime('%Y%m%d_%H%M%S')}_{self.current_label.get()}.csv"
        path = filedialog.asksaveasfilename(
            title="保存 CSI 数据 CSV",
            initialdir=str(data_dir),
            initialfile=default_name,
            defaultextension=".csv",
            filetypes=[("CSV 文件", "*.csv"), ("所有文件", "*.*")],
        )
        if not path:
            return

        self.record_file = open(path, "w", newline="", encoding="utf-8-sig")
        self.record_writer = csv.writer(self.record_file)
        self.record_writer.writerow(["时间_s", "标签", *CHANNEL_NAMES, "原始行"])
        self.record_started_at = time.perf_counter()
        self.record_duration_s = float(duration_s)
        self.record_stop_job = self.root.after(duration_s * 1000, self.stop_recording)
        self.record_button.configure(text="停止记录")
        self.duration_spin.configure(state="disabled")
        self.status_text.set(f"正在记录 {duration_s} 秒：{path}")

    def stop_recording(self) -> None:
        if self.record_stop_job:
            try:
                self.root.after_cancel(self.record_stop_job)
            except tk.TclError:
                pass
            self.record_stop_job = None
        if self.record_file:
            self.record_file.close()
        self.record_file = None
        self.record_writer = None
        self.record_button.configure(text="开始记录")
        self.duration_spin.configure(state="normal")
        self.status_text.set("记录已停止")

    def poll_queue(self) -> None:
        drained = 0
        while drained < 500:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break

            if isinstance(item, Sample):
                self.plot.add_sample(item)
                self.sample_count += 1
                if self.record_writer:
                    elapsed = item.t - self.record_started_at
                    if 0.0 <= elapsed <= self.record_duration_s:
                        self.record_writer.writerow([f"{elapsed:.6f}", self.current_label.get(), *item.values, item.raw])
            else:
                self.status_text.set(item)
            drained += 1

        now = time.perf_counter()
        if now - self.last_rate_time >= 1.0:
            hz = (self.sample_count - self.last_rate_count) / (now - self.last_rate_time)
            self.hz_text.set(f"{hz:.1f} Hz  样本={self.sample_count}")
            self.last_rate_time = now
            self.last_rate_count = self.sample_count

        self.root.after(25, self.poll_queue)

    def redraw_plot(self) -> None:
        self.plot.redraw()
        self.root.after(60, self.redraw_plot)

    def on_close(self) -> None:
        self.disconnect()
        self.stop_recording()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    app = CsiViewerApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
