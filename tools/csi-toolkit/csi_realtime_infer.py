from __future__ import annotations

import argparse
import csv
import json
import re
import socket
import sys
import time
import urllib.request
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Iterable

import joblib
import numpy as np
import pandas as pd
import serial
from serial.tools import list_ports

from train_csi_model import CHANNELS, extract_window_feature_dict


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODELS_DIR = SCRIPT_DIR / "models"
CSI_BRACKET_RE = re.compile(r"\[([^\]]+)\]")


def parse_number_list(text: str) -> list[float] | None:
    parts = [part.strip() for part in text.split(",")]
    if len(parts) < len(CHANNELS):
        return None

    values: list[float] = []
    for part in parts[:len(CHANNELS)]:
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

    prefix = line[:match.start()].rstrip(",")
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
    return parse_number_list(stripped)


def find_latest_model(models_dir: Path) -> Path:
    candidates = sorted(
        models_dir.glob("csi_binary_*/csi_model.joblib"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError(f"No model found under {models_dir}. Run train_csi_model.py first.")
    return candidates[0]


def choose_port(port: str | None) -> str:
    if port:
        return port

    ports = list(list_ports.comports())
    if len(ports) == 1:
        return ports[0].device

    print("请用 --port 指定 A 板串口。当前可用串口：")
    for item in ports:
        print(f"  {item.device}: {item.description}")
    raise SystemExit(2)


def replay_samples(path: Path) -> Iterable[list[float]]:
    df = pd.read_csv(path, encoding="utf-8-sig")
    missing = [channel for channel in CHANNELS if channel not in df]
    if missing:
        raise ValueError(f"{path} missing columns: {missing}")

    for _, row in df.iterrows():
        try:
            yield [float(row[channel]) for channel in CHANNELS]
        except (TypeError, ValueError):
            continue


def serial_samples(port: str, baud: int) -> Iterable[list[float]]:
    with serial.Serial(port, baud, timeout=0.2) as ser:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="ignore").strip()
            values = parse_serial_line(line)
            if values is not None:
                yield values


def udp_samples(port: int, host: str = "0.0.0.0") -> Iterable[list[float]]:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host, port))
        while True:
            packet, _addr = sock.recvfrom(2048)
            line_text = packet.decode("utf-8", errors="ignore")
            for line in line_text.splitlines():
                values = parse_serial_line(line)
                if values is not None:
                    yield values


def post_json(url: str, payload: dict[str, object]) -> None:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=0.5) as response:
        response.read()


class RealtimeInfer:
    def __init__(
        self,
        bundle: dict[str, object],
        threshold: float,
        smooth: int,
        confirm_windows: int,
        warmup_windows: int,
        adaptive_margin: float = 0.15,
        motion_gate: bool = True,
        motion_multiplier: float = 2.0,
        motion_min: float = 0.01,
        motion_hold_windows: int = 8,
    ) -> None:
        self.model = bundle["model"]
        self.feature_names = list(bundle["feature_names"])
        self.window_s = float(bundle["window_s"])
        self.step_s = float(bundle["step_s"])
        self.sample_rate = float(bundle["sample_rate"])
        self.window_rows = max(10, int(round(self.window_s * self.sample_rate)))
        self.step_rows = max(1, int(round(self.step_s * self.sample_rate)))
        self.threshold = threshold
        self.buffer: deque[list[float]] = deque(maxlen=self.window_rows)
        self.prob_history: deque[float] = deque(maxlen=max(1, smooth))
        self.confirm_windows = max(1, confirm_windows)
        self.warmup_windows = max(0, warmup_windows)
        self.adaptive_margin = max(0.0, adaptive_margin)
        self.motion_gate = motion_gate
        self.motion_multiplier = max(1.0, motion_multiplier)
        self.motion_min = max(0.0, motion_min)
        self.motion_hold_windows = max(1, motion_hold_windows)
        self.calibration_probs: list[float] = []
        self.calibration_motion: list[float] = []
        self.motion_hold_count = 0
        self.over_threshold_count = 0
        self.window_count = 0
        self.total_samples = 0
        self.last_infer_sample = 0

    def motion_score(self, window: np.ndarray) -> float:
        if window.shape[0] < 2 or window.shape[1] < 2:
            return 0.0
        # Use CSI amplitude channels only. RSSI is useful for ML, but too
        # coarse for a direct motion gate.
        amp = np.abs(window[:, 1:].astype(float))
        prev = np.maximum(amp[:-1], 1.0)
        diff = np.abs(np.diff(amp, axis=0)) / prev
        return float(np.median(diff) + 0.5 * np.percentile(diff, 75))

    def add_sample(self, values: list[float]) -> dict[str, object] | None:
        self.total_samples += 1
        self.buffer.append(values)

        if len(self.buffer) < self.window_rows:
            return None
        if self.total_samples - self.last_infer_sample < self.step_rows:
            return None
        self.last_infer_sample = self.total_samples

        window = np.asarray(list(self.buffer), dtype=float)
        feature_map = extract_window_feature_dict(window)
        features = np.asarray(
            [[feature_map.get(name, 0.0) for name in self.feature_names]],
            dtype=float,
        )
        prob = self.model.predict_proba(features)[0]
        fall_col = list(self.model.classes_).index("fall")
        fall_probability = float(prob[fall_col])
        motion_score = self.motion_score(window)

        self.prob_history.append(fall_probability)
        smooth_probability = float(np.mean(self.prob_history))
        self.window_count += 1

        calibrating = self.window_count <= self.warmup_windows
        if calibrating:
            self.calibration_probs.append(smooth_probability)
            self.calibration_motion.append(motion_score)

        baseline_probability = float(np.percentile(self.calibration_probs, 90)) if self.calibration_probs else 0.0
        baseline_motion = float(np.percentile(self.calibration_motion, 90)) if self.calibration_motion else 0.0
        effective_threshold = min(0.95, max(self.threshold, baseline_probability + self.adaptive_margin))
        motion_threshold = max(self.motion_min, baseline_motion * self.motion_multiplier)

        if motion_score >= motion_threshold:
            self.motion_hold_count = self.motion_hold_windows
        elif self.motion_hold_count > 0:
            self.motion_hold_count -= 1
        motion_ok = (not self.motion_gate) or self.motion_hold_count > 0

        if calibrating:
            self.over_threshold_count = 0
        elif smooth_probability >= effective_threshold and motion_ok:
            self.over_threshold_count += 1
        else:
            self.over_threshold_count = 0

        alarm = self.over_threshold_count >= self.confirm_windows
        if calibrating:
            state = "校准中"
        elif alarm:
            state = "疑似跌倒"
        elif self.over_threshold_count > 0:
            state = "预警中"
        else:
            state = "正常"
        return {
            "samples": self.total_samples,
            "fall_probability": round(fall_probability, 4),
            "smooth_probability": round(smooth_probability, 4),
            "threshold": self.threshold,
            "effective_threshold": round(effective_threshold, 4),
            "baseline_probability": round(baseline_probability, 4),
            "motion_score": round(motion_score, 5),
            "motion_threshold": round(motion_threshold, 5),
            "motion_ok": motion_ok,
            "over_threshold_count": self.over_threshold_count,
            "confirm_windows": self.confirm_windows,
            "alarm": alarm,
            "state": state,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description="Realtime ESP32 CSI fall inference.")
    parser.add_argument("--port", help="serial port, e.g. COM8")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate")
    parser.add_argument("--model", type=Path, help="path to csi_model.joblib")
    parser.add_argument("--models-dir", type=Path, default=DEFAULT_MODELS_DIR, help="models directory")
    parser.add_argument("--threshold", type=float, default=0.50, help="fall probability threshold")
    parser.add_argument("--smooth", type=int, default=2, help="moving average window count for probabilities")
    parser.add_argument("--confirm-windows", type=int, default=4, help="consecutive smoothed windows required for alarm")
    parser.add_argument("--warmup-windows", type=int, default=0, help="initial inference windows used for environment calibration")
    parser.add_argument("--adaptive-margin", type=float, default=0.15, help="extra probability margin above the calibrated baseline")
    parser.add_argument("--no-motion-gate", action="store_true", help="disable CSI motion gate")
    parser.add_argument("--motion-multiplier", type=float, default=2.0, help="motion threshold multiplier above calibrated baseline")
    parser.add_argument("--motion-min", type=float, default=0.01, help="minimum dimensionless motion score required")
    parser.add_argument("--motion-hold-windows", type=int, default=8, help="windows kept motion-valid after a burst")
    parser.add_argument("--replay", type=Path, help="replay an existing CSV instead of reading serial")
    parser.add_argument("--udp-port", type=int, help="receive six-channel CSI features from UDP instead of serial")
    parser.add_argument("--udp-host", default="0.0.0.0", help="UDP bind host")
    parser.add_argument("--log", type=Path, help="optional CSV log path for inference results")
    parser.add_argument("--post-url", help="optional HTTP endpoint to post inference JSON")
    parser.add_argument("--alarm-only", action="store_true", help="only print alarm windows")
    parser.add_argument("--print-normal", action="store_true", help="deprecated: normal windows are printed by default")
    args = parser.parse_args()

    model_path = args.model.expanduser().resolve() if args.model else find_latest_model(args.models_dir.expanduser().resolve())
    bundle = joblib.load(model_path)
    threshold = float(args.threshold)

    infer = RealtimeInfer(
        bundle=bundle,
        threshold=threshold,
        smooth=args.smooth,
        confirm_windows=args.confirm_windows,
        warmup_windows=args.warmup_windows,
        adaptive_margin=args.adaptive_margin,
        motion_gate=not args.no_motion_gate,
        motion_multiplier=args.motion_multiplier,
        motion_min=args.motion_min,
        motion_hold_windows=args.motion_hold_windows,
    )

    log_file = None
    writer: csv.writer | None = None
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        log_file = args.log.open("w", newline="", encoding="utf-8-sig")
        writer = csv.writer(log_file)
        writer.writerow([
            "time",
            "samples",
            "fall_probability",
            "smooth_probability",
            "threshold",
            "effective_threshold",
            "baseline_probability",
            "motion_score",
            "motion_threshold",
            "motion_ok",
            "state",
        ])

    try:
        print(f"model: {model_path}")
        print(f"window={infer.window_s:.2f}s step={infer.step_s:.2f}s threshold={threshold:.2f}")
        if args.replay:
            source = replay_samples(args.replay.expanduser().resolve())
            print(f"replay: {args.replay}")
        elif args.udp_port:
            source = udp_samples(args.udp_port, args.udp_host)
            print(f"udp: {args.udp_host}:{args.udp_port}")
        else:
            port = choose_port(args.port)
            source = serial_samples(port, args.baud)
            print(f"serial: {port} @ {args.baud}")

        for values in source:
            result = infer.add_sample(values)
            if result is None:
                continue

            now = datetime.now().strftime("%H:%M:%S")
            if result["alarm"] or not args.alarm_only or args.print_normal:
                print(
                    f"{now} samples={result['samples']} "
                    f"p_fall={result['fall_probability']:.3f} "
                    f"smooth={result['smooth_probability']:.3f} "
                    f"th={result.get('effective_threshold', result['threshold']):.3f} "
                    f"motion={result.get('motion_score', 0.0):.4f}/{result.get('motion_threshold', 0.0):.4f} "
                    f"state={result['state']}"
                )

            if writer:
                writer.writerow([
                    now,
                    result["samples"],
                    result["fall_probability"],
                    result["smooth_probability"],
                    result["threshold"],
                    result.get("effective_threshold", result["threshold"]),
                    result.get("baseline_probability", 0.0),
                    result.get("motion_score", 0.0),
                    result.get("motion_threshold", 0.0),
                    result.get("motion_ok", True),
                    result["state"],
                ])
                log_file.flush()

            if args.post_url:
                payload = {
                    "sensor": "esp32_s3_wifi_csi",
                    "time": now,
                    **result,
                }
                try:
                    post_json(args.post_url, payload)
                except Exception as exc:
                    print(f"post failed: {exc}", file=sys.stderr)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if log_file:
            log_file.close()


if __name__ == "__main__":
    main()
