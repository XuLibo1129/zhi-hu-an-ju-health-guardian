from __future__ import annotations

import queue
import socket
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, font as tkfont, messagebox, ttk
import tkinter as tk

import joblib
import pandas as pd
import serial
from serial.tools import list_ports

from csi_realtime_infer import (
    RealtimeInfer,
    find_latest_model,
    parse_serial_line,
    post_json,
    replay_samples,
)
from train_csi_model import CHANNELS


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODELS_DIR = SCRIPT_DIR / "models"
DEFAULT_DATA_DIR = SCRIPT_DIR / "data"


class InferenceWorker(threading.Thread):
    def __init__(
        self,
        *,
        mode: str,
        port: str,
        baud: int,
        udp_port: int,
        replay_path: Path | None,
        model_path: Path,
        threshold: float,
        smooth: int,
        confirm_windows: int,
        warmup_windows: int,
        post_url: str,
        out_queue: queue.Queue[tuple[str, object]],
        stop_event: threading.Event,
    ) -> None:
        super().__init__(daemon=True)
        self.mode = mode
        self.port = port
        self.baud = baud
        self.udp_port = udp_port
        self.replay_path = replay_path
        self.model_path = model_path
        self.threshold = threshold
        self.smooth = smooth
        self.confirm_windows = confirm_windows
        self.warmup_windows = warmup_windows
        self.post_url = post_url.strip()
        self.out_queue = out_queue
        self.stop_event = stop_event
        self.last_post_error_at = 0.0

    def run(self) -> None:
        try:
            bundle = joblib.load(self.model_path)
            infer = RealtimeInfer(
                bundle=bundle,
                threshold=self.threshold,
                smooth=self.smooth,
                confirm_windows=self.confirm_windows,
                warmup_windows=self.warmup_windows,
            )
            self.out_queue.put(("info", f"模型已加载：{self.model_path.name}"))
            self.out_queue.put(("model_info", {
                "window_s": infer.window_s,
                "step_s": infer.step_s,
                "threshold": infer.threshold,
            }))

            if self.mode == "replay":
                if not self.replay_path:
                    raise ValueError("未选择回放 CSV")
                self.out_queue.put(("info", f"开始回放：{self.replay_path.name}"))
                for values in replay_samples(self.replay_path):
                    if self.stop_event.is_set():
                        break
                    self.handle_values(infer, values)
                    time.sleep(1.0 / max(infer.sample_rate, 1.0))
            elif self.mode == "udp":
                self.out_queue.put(("info", f"开始UDP接收：0.0.0.0:{self.udp_port}"))
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    sock.bind(("0.0.0.0", self.udp_port))
                    sock.settimeout(0.2)
                    while not self.stop_event.is_set():
                        try:
                            packet, _addr = sock.recvfrom(2048)
                        except socket.timeout:
                            continue
                        line_text = packet.decode("utf-8", errors="ignore")
                        for line in line_text.splitlines():
                            values = parse_serial_line(line)
                            if values is None:
                                continue
                            self.handle_values(infer, values)
            else:
                self.out_queue.put(("info", f"打开串口：{self.port} @ {self.baud}"))
                with serial.Serial(self.port, self.baud, timeout=0.2) as ser:
                    while not self.stop_event.is_set():
                        raw = ser.readline()
                        if not raw:
                            continue
                        line = raw.decode("utf-8", errors="ignore").strip()
                        values = parse_serial_line(line)
                        if values is None:
                            continue
                        self.handle_values(infer, values)
        except Exception as exc:
            self.out_queue.put(("error", str(exc)))
        finally:
            self.out_queue.put(("done", None))

    def handle_values(self, infer: RealtimeInfer, values: list[float]) -> None:
        result = infer.add_sample(values)
        self.out_queue.put(("sample", values))
        if result is not None:
            self.out_queue.put(("result", result))
            if self.post_url:
                payload = {
                    "sensor": "esp32_s3_wifi_csi",
                    "node_id": "wifi_csi_01",
                    "time": datetime.now().isoformat(timespec="seconds"),
                    "fall_probability": result["fall_probability"],
                    "smooth_probability": result["smooth_probability"],
                    "threshold": result["threshold"],
                    "effective_threshold": result.get("effective_threshold", result["threshold"]),
                    "motion_score": result.get("motion_score", 0.0),
                    "motion_threshold": result.get("motion_threshold", 0.0),
                    "motion_ok": result.get("motion_ok", True),
                    "alarm": result["alarm"],
                    "state": result["state"],
                    "samples": result["samples"],
                }
                try:
                    post_json(self.post_url, payload)
                    self.out_queue.put(("post", "ok"))
                except Exception as exc:
                    now = time.monotonic()
                    if now - self.last_post_error_at > 3.0:
                        self.out_queue.put(("post_error", str(exc)))
                        self.last_post_error_at = now


class ProbabilityPlot(tk.Canvas):
    def __init__(self, parent: tk.Misc, **kwargs) -> None:
        super().__init__(parent, **kwargs)
        self.configure(bg="#101418", highlightthickness=0)
        self.values: deque[float] = deque(maxlen=160)
        self.threshold = 0.70

    def set_threshold(self, threshold: float) -> None:
        self.threshold = threshold
        self.redraw()

    def add_value(self, value: float) -> None:
        self.values.append(max(0.0, min(1.0, value)))
        self.redraw()

    def clear(self) -> None:
        self.values.clear()
        self.redraw()

    def redraw(self) -> None:
        self.delete("all")
        width = max(self.winfo_width(), 240)
        height = max(self.winfo_height(), 120)
        left = 42
        right = 10
        top = 12
        bottom = 24
        plot_w = width - left - right
        plot_h = height - top - bottom

        self.create_rectangle(0, 0, width, height, fill="#101418", outline="")
        for i in range(6):
            y = top + plot_h * i / 5
            self.create_line(left, y, left + plot_w, y, fill="#22303a")
        for i, label in enumerate(("1.0", "0.5", "0.0")):
            y = top + plot_h * i / 2
            self.create_text(left - 8, y, text=label, fill="#8b949e", anchor="e")

        threshold_y = top + plot_h * (1.0 - self.threshold)
        self.create_line(left, threshold_y, left + plot_w, threshold_y, fill="#e5c07b", dash=(4, 3))
        self.create_text(left + 4, threshold_y - 8, text=f"阈值 {self.threshold:.2f}", fill="#e5c07b", anchor="w")

        if len(self.values) < 2:
            return

        points: list[float] = []
        denom = max(len(self.values) - 1, 1)
        for idx, value in enumerate(self.values):
            x = left + plot_w * idx / denom
            y = top + plot_h * (1.0 - value)
            points.extend([x, y])
        self.create_line(*points, fill="#61afef", width=2)


class CsiRealtimeGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("ESP32 CSI 实时跌倒辅助检测")
        self.root.geometry("1080x720")
        self.root.minsize(920, 620)

        self.queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.stop_event = threading.Event()
        self.worker: InferenceWorker | None = None
        self.latest_values = [0.0 for _ in CHANNELS]
        self.sample_count = 0
        self.result_count = 0

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.udp_port_var = tk.StringVar(value="34567")
        self.model_var = tk.StringVar(value=self.default_model_text())
        self.threshold_var = tk.DoubleVar(value=0.50)
        self.smooth_var = tk.IntVar(value=2)
        self.confirm_var = tk.IntVar(value=4)
        self.warmup_var = tk.IntVar(value=0)
        self.replay_var = tk.StringVar()
        self.post_url_var = tk.StringVar(value="http://192.168.1.100:8090/api/sensor/csi")
        self.status_var = tk.StringVar(value="未连接")
        self.prob_var = tk.StringVar(value="0.000")
        self.smooth_prob_var = tk.StringVar(value="0.000")
        self.sample_var = tk.StringVar(value="样本 0 / 窗口 0")

        self.configure_fonts()
        self.build_ui()
        self.refresh_ports()
        self.root.after(50, self.poll_queue)
        self.root.after(350, self.start_udp)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def default_model_text(self) -> str:
        try:
            return str(find_latest_model(DEFAULT_MODELS_DIR))
        except Exception:
            return ""

    def configure_fonts(self) -> None:
        family = "Microsoft YaHei UI"
        for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont"):
            try:
                tkfont.nametofont(name).configure(family=family, size=10)
            except tk.TclError:
                pass
        style = ttk.Style(self.root)
        style.configure(".", font=(family, 10))
        style.configure("Title.TLabel", font=(family, 14, "bold"))

    def build_ui(self) -> None:
        self.root.configure(bg="#171b20")

        top = ttk.Frame(self.root, padding=10)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="串口").grid(row=0, column=0, sticky="w")
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=26, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="ew", padx=(6, 8))
        ttk.Button(top, text="刷新", command=self.refresh_ports).grid(row=0, column=2, padx=(0, 14))

        ttk.Label(top, text="波特率").grid(row=0, column=3, sticky="w")
        ttk.Entry(top, textvariable=self.baud_var, width=10).grid(row=0, column=4, padx=(6, 14))

        self.start_button = ttk.Button(top, text="开始实时检测", command=self.start_serial)
        self.start_button.grid(row=0, column=5, padx=(0, 8))
        self.stop_button = ttk.Button(top, text="停止", command=self.stop_worker, state="disabled")
        self.stop_button.grid(row=0, column=6)

        ttk.Label(top, text="UDP端口").grid(row=1, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(top, textvariable=self.udp_port_var, width=10).grid(row=1, column=1, sticky="w", padx=(6, 8), pady=(10, 0))
        self.udp_button = ttk.Button(top, text="开始UDP接收", command=self.start_udp)
        self.udp_button.grid(row=1, column=5, pady=(10, 0), padx=(0, 8))
        ttk.Label(top, text="A板发送到本机 IP:34567").grid(row=1, column=2, columnspan=3, sticky="w", pady=(10, 0))

        ttk.Label(top, text="模型").grid(row=2, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(top, textvariable=self.model_var).grid(row=2, column=1, columnspan=4, sticky="ew", padx=(6, 8), pady=(10, 0))
        ttk.Button(top, text="选择模型", command=self.choose_model).grid(row=2, column=5, pady=(10, 0), padx=(0, 8))
        ttk.Button(top, text="最新模型", command=self.use_latest_model).grid(row=2, column=6, pady=(10, 0))

        ttk.Label(top, text="回放CSV").grid(row=3, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(top, textvariable=self.replay_var).grid(row=3, column=1, columnspan=4, sticky="ew", padx=(6, 8), pady=(10, 0))
        ttk.Button(top, text="选择CSV", command=self.choose_replay).grid(row=3, column=5, pady=(10, 0), padx=(0, 8))
        ttk.Button(top, text="回放测试", command=self.start_replay).grid(row=3, column=6, pady=(10, 0))

        ttk.Label(top, text="主终端URL").grid(row=4, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(top, textvariable=self.post_url_var).grid(row=4, column=1, columnspan=4, sticky="ew", padx=(6, 8), pady=(10, 0))
        ttk.Button(top, text="测试发送", command=self.test_post).grid(row=4, column=5, pady=(10, 0), padx=(0, 8))
        ttk.Button(top, text="清空URL", command=lambda: self.post_url_var.set("")).grid(row=4, column=6, pady=(10, 0))

        top.columnconfigure(1, weight=1)

        settings = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        settings.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(settings, text="阈值").pack(side=tk.LEFT)
        threshold_scale = ttk.Scale(settings, from_=0.30, to=0.95, variable=self.threshold_var, command=self.on_threshold_change)
        threshold_scale.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(8, 8))
        self.threshold_label = ttk.Label(settings, text="0.50", width=5)
        self.threshold_label.pack(side=tk.LEFT, padx=(0, 18))

        ttk.Label(settings, text="平滑窗口").pack(side=tk.LEFT)
        ttk.Spinbox(settings, from_=1, to=10, textvariable=self.smooth_var, width=5).pack(side=tk.LEFT, padx=(6, 18))
        ttk.Label(settings, text="连续确认").pack(side=tk.LEFT)
        ttk.Spinbox(settings, from_=1, to=12, textvariable=self.confirm_var, width=5).pack(side=tk.LEFT, padx=(6, 18))
        ttk.Label(settings, text="校准窗口").pack(side=tk.LEFT)
        ttk.Spinbox(settings, from_=0, to=12, textvariable=self.warmup_var, width=5).pack(side=tk.LEFT, padx=(6, 18))
        ttk.Button(settings, text="稳定运行", command=self.apply_stable_preset).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(settings, text="灵敏测试", command=self.apply_sensitive_preset).pack(side=tk.LEFT)

        main = ttk.Frame(self.root, padding=(10, 0, 10, 10))
        main.pack(fill=tk.BOTH, expand=True)

        left = ttk.Frame(main)
        left.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 10))

        status_frame = ttk.Frame(left)
        status_frame.pack(fill=tk.X)
        self.state_label = tk.Label(
            status_frame,
            textvariable=self.status_var,
            font=("Microsoft YaHei UI", 42, "bold"),
            bg="#2f343f",
            fg="#d0d7de",
            height=2,
        )
        self.state_label.pack(fill=tk.X)

        prob_frame = ttk.Frame(left, padding=(0, 12, 0, 8))
        prob_frame.pack(fill=tk.X)
        ttk.Label(prob_frame, text="跌倒概率", style="Title.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(prob_frame, textvariable=self.prob_var, font=("Microsoft YaHei UI", 22, "bold")).grid(row=0, column=1, sticky="e")
        self.prob_bar = ttk.Progressbar(prob_frame, maximum=100)
        self.prob_bar.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(6, 10))
        ttk.Label(prob_frame, text="平滑概率").grid(row=2, column=0, sticky="w")
        ttk.Label(prob_frame, textvariable=self.smooth_prob_var).grid(row=2, column=1, sticky="e")
        self.smooth_bar = ttk.Progressbar(prob_frame, maximum=100)
        self.smooth_bar.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        prob_frame.columnconfigure(0, weight=1)

        self.plot = ProbabilityPlot(left, height=220)
        self.plot.pack(fill=tk.BOTH, expand=True, pady=(4, 0))

        right = ttk.Frame(main, width=330)
        right.pack(side=tk.RIGHT, fill=tk.BOTH)

        ttk.Label(right, text="实时数据", style="Title.TLabel").pack(anchor="w")
        self.channel_box = tk.Text(right, height=9, width=36, bg="#101418", fg="#d0d7de", insertbackground="#d0d7de")
        self.channel_box.pack(fill=tk.X, pady=(8, 10))
        self.channel_box.configure(state="disabled")

        ttk.Label(right, textvariable=self.sample_var).pack(anchor="w", pady=(0, 8))

        ttk.Label(right, text="运行日志", style="Title.TLabel").pack(anchor="w")
        self.log_box = tk.Text(right, height=14, width=36, bg="#101418", fg="#d0d7de", insertbackground="#d0d7de")
        self.log_box.pack(fill=tk.BOTH, expand=True, pady=(8, 0))
        self.log_box.configure(state="disabled")

        self.update_channel_box()
        self.on_threshold_change()

    def refresh_ports(self) -> None:
        ports = list(list_ports.comports())
        items = [f"{port.device} - {port.description}" for port in ports]
        self.port_combo["values"] = items
        if items and not self.port_var.get():
            self.port_var.set(items[0])
        if not items:
            self.log("未发现串口")

    def selected_port(self) -> str:
        text = self.port_var.get().strip()
        return text.split(" - ", 1)[0] if text else ""

    def choose_model(self) -> None:
        path = filedialog.askopenfilename(
            title="选择 CSI 模型",
            initialdir=str(DEFAULT_MODELS_DIR),
            filetypes=[("Joblib 模型", "*.joblib"), ("所有文件", "*.*")],
        )
        if path:
            self.model_var.set(path)

    def use_latest_model(self) -> None:
        try:
            self.model_var.set(str(find_latest_model(DEFAULT_MODELS_DIR)))
            self.log("已切换到最新模型")
        except Exception as exc:
            messagebox.showerror("未找到模型", str(exc))

    def choose_replay(self) -> None:
        path = filedialog.askopenfilename(
            title="选择回放 CSV",
            initialdir=str(DEFAULT_DATA_DIR),
            filetypes=[("CSV 文件", "*.csv"), ("所有文件", "*.*")],
        )
        if path:
            self.replay_var.set(path)

    def test_post(self) -> None:
        url = self.post_url_var.get().strip()
        if not url:
            messagebox.showwarning("未填写主终端URL", "请先填写主终端 HTTP 接收地址。")
            return
        payload = {
            "sensor": "esp32_s3_wifi_csi",
            "node_id": "wifi_csi_01",
            "time": datetime.now().isoformat(timespec="seconds"),
            "fall_probability": 0.0,
            "smooth_probability": 0.0,
            "threshold": float(self.threshold_var.get()),
            "alarm": False,
            "state": "测试",
            "samples": 0,
        }
        try:
            post_json(url, payload)
            self.log("主终端测试发送成功")
        except Exception as exc:
            self.log(f"主终端测试发送失败：{exc}")
            messagebox.showerror("发送失败", str(exc))

    def on_threshold_change(self, *_args: object) -> None:
        value = self.threshold_var.get()
        self.threshold_label.configure(text=f"{value:.2f}")
        self.plot.set_threshold(value)

    def apply_stable_preset(self) -> None:
        self.threshold_var.set(0.50)
        self.smooth_var.set(2)
        self.confirm_var.set(4)
        self.warmup_var.set(0)
        self.on_threshold_change()
        self.log("已切换：稳定运行（阈值0.50，平滑2，连续确认4，校准0）")

    def apply_sensitive_preset(self) -> None:
        self.threshold_var.set(0.45)
        self.smooth_var.set(1)
        self.confirm_var.set(2)
        self.warmup_var.set(0)
        self.on_threshold_change()
        self.log("已切换：灵敏测试（阈值0.45，平滑1，连续确认2，校准0）")

    def start_serial(self) -> None:
        port = self.selected_port()
        if not port:
            messagebox.showwarning("未选择串口", "请先选择 A 板串口。")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("波特率无效", "波特率必须是数字。")
            return
        self.start_worker(mode="serial", port=port, baud=baud, udp_port=0, replay_path=None)

    def start_udp(self) -> None:
        try:
            udp_port = int(self.udp_port_var.get())
        except ValueError:
            messagebox.showwarning("UDP端口无效", "UDP端口必须是数字。")
            return
        if not (1 <= udp_port <= 65535):
            messagebox.showwarning("UDP端口无效", "UDP端口范围必须是 1-65535。")
            return
        self.start_worker(mode="udp", port="", baud=0, udp_port=udp_port, replay_path=None)

    def start_replay(self) -> None:
        text = self.replay_var.get().strip()
        if not text:
            messagebox.showwarning("未选择 CSV", "请先选择一个 CSV 回放文件。")
            return
        self.start_worker(mode="replay", port="", baud=0, udp_port=0, replay_path=Path(text))

    def start_worker(self, *, mode: str, port: str, baud: int, udp_port: int, replay_path: Path | None) -> None:
        if self.worker:
            return
        model_text = self.model_var.get().strip()
        if not model_text:
            messagebox.showwarning("未选择模型", "请先训练模型或选择 csi_model.joblib。")
            return

        try:
            threshold = float(self.threshold_var.get())
            smooth = int(self.smooth_var.get())
            confirm = int(self.confirm_var.get())
            warmup = int(self.warmup_var.get())
        except (tk.TclError, ValueError):
            messagebox.showwarning("参数无效", "阈值、平滑窗口、连续确认和校准窗口必须是数字。")
            return

        self.stop_event.clear()
        self.reset_runtime()
        self.worker = InferenceWorker(
            mode=mode,
            port=port,
            baud=baud,
            udp_port=udp_port,
            replay_path=replay_path,
            model_path=Path(model_text),
            threshold=threshold,
            smooth=smooth,
            confirm_windows=confirm,
            warmup_windows=warmup,
            post_url=self.post_url_var.get(),
            out_queue=self.queue,
            stop_event=self.stop_event,
        )
        self.worker.start()
        self.set_running(True)
        self.log("开始检测")

    def stop_worker(self) -> None:
        self.stop_event.set()
        self.log("正在停止...")

    def reset_runtime(self) -> None:
        self.sample_count = 0
        self.result_count = 0
        self.latest_values = [0.0 for _ in CHANNELS]
        self.plot.clear()
        self.update_probability(0.0, 0.0)
        self.set_state("校准中")
        self.update_channel_box()

    def set_running(self, running: bool) -> None:
        state = "disabled" if running else "normal"
        self.start_button.configure(state=state)
        self.udp_button.configure(state=state)
        self.stop_button.configure(state="normal" if running else "disabled")

    def poll_queue(self) -> None:
        while True:
            try:
                kind, payload = self.queue.get_nowait()
            except queue.Empty:
                break
            if kind == "sample":
                self.sample_count += 1
                self.latest_values = list(payload)  # type: ignore[arg-type]
                self.update_channel_box()
            elif kind == "result":
                self.result_count += 1
                result = payload  # type: ignore[assignment]
                self.handle_result(result)  # type: ignore[arg-type]
            elif kind == "model_info":
                info = payload  # type: ignore[assignment]
                self.log(f"窗口 {info['window_s']:.2f}s，步长 {info['step_s']:.2f}s，阈值 {info['threshold']:.2f}")
            elif kind == "info":
                self.log(str(payload))
            elif kind == "post":
                pass
            elif kind == "post_error":
                self.log(f"主终端发送失败：{payload}")
            elif kind == "error":
                self.log(f"错误：{payload}")
                messagebox.showerror("实时检测错误", str(payload))
            elif kind == "done":
                self.worker = None
                self.set_running(False)
                self.log("检测已停止")
        self.root.after(50, self.poll_queue)

    def handle_result(self, result: dict[str, object]) -> None:
        fall_probability = float(result["fall_probability"])
        smooth_probability = float(result["smooth_probability"])
        state = str(result["state"])
        effective_threshold = float(result.get("effective_threshold", result["threshold"]))
        motion_score = float(result.get("motion_score", 0.0))
        motion_threshold = float(result.get("motion_threshold", 0.0))
        self.update_probability(fall_probability, smooth_probability)
        self.plot.add_value(smooth_probability)
        self.set_state(state)
        self.sample_var.set(
            f"样本 {result['samples']} / 窗口 {self.result_count} / "
            f"确认 {result.get('over_threshold_count', 0)}/{result.get('confirm_windows', self.confirm_var.get())} / "
            f"动态阈值 {effective_threshold:.2f}"
        )
        if state in ("预警中", "疑似跌倒"):
            self.log(
                f"{state}：p={fall_probability:.3f}, smooth={smooth_probability:.3f}, "
                f"motion={motion_score:.4f}/{motion_threshold:.4f}, th={effective_threshold:.2f}"
            )

    def update_probability(self, fall_probability: float, smooth_probability: float) -> None:
        self.prob_var.set(f"{fall_probability:.3f}")
        self.smooth_prob_var.set(f"{smooth_probability:.3f}")
        self.prob_bar["value"] = fall_probability * 100.0
        self.smooth_bar["value"] = smooth_probability * 100.0

    def set_state(self, state: str) -> None:
        self.status_var.set(state)
        if state == "疑似跌倒":
            self.state_label.configure(bg="#8b1d1d", fg="#ffffff")
        elif state == "预警中":
            self.state_label.configure(bg="#9a6700", fg="#ffffff")
        elif state == "校准中":
            self.state_label.configure(bg="#735c0f", fg="#ffffff")
        else:
            self.state_label.configure(bg="#1f6f3d", fg="#ffffff")

    def update_channel_box(self) -> None:
        lines = []
        for name, value in zip(CHANNELS, self.latest_values, strict=True):
            lines.append(f"{name:<8} {value:>10.1f}")
        self.channel_box.configure(state="normal")
        self.channel_box.delete("1.0", tk.END)
        self.channel_box.insert(tk.END, "\n".join(lines))
        self.channel_box.configure(state="disabled")

    def log(self, text: str) -> None:
        now = datetime.now().strftime("%H:%M:%S")
        self.log_box.configure(state="normal")
        self.log_box.insert(tk.END, f"[{now}] {text}\n")
        self.log_box.see(tk.END)
        self.log_box.configure(state="disabled")

    def on_close(self) -> None:
        self.stop_event.set()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    CsiRealtimeGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
