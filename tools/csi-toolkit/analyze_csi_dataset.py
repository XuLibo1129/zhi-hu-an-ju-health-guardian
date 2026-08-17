from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATA_DIR = SCRIPT_DIR / "data"
CHANNELS = ["信号强度", "平均幅度", "子载波8", "子载波24", "子载波48", "子载波80"]
TIME_COL = "时间_s"
LABEL_COL = "标签"
LABEL_ALIASES = {
    "其他": "弯腰捡东西",
}


def read_csv(path: Path) -> pd.DataFrame:
    for encoding in ("utf-8-sig", "utf-8", "gbk"):
        try:
            return pd.read_csv(path, encoding=encoding)
        except UnicodeDecodeError:
            continue
    return pd.read_csv(path)


def normalize_time(df: pd.DataFrame) -> pd.DataFrame:
    if TIME_COL not in df:
        return df

    df = df.copy()
    df[TIME_COL] = pd.to_numeric(df[TIME_COL], errors="coerce")
    df = df.dropna(subset=[TIME_COL])
    if not df.empty:
        df[TIME_COL] = df[TIME_COL] - df[TIME_COL].min()
    return df


def normalize_label(label: object, fallback: str) -> str:
    text = str(label).strip() if label is not None else ""
    if text and text != "nan":
        return LABEL_ALIASES.get(text, text)
    return LABEL_ALIASES.get(fallback, fallback)


def safe_filename(text: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "_-" else "_" for ch in text)


def summarize_file(path: Path, df: pd.DataFrame) -> dict[str, object]:
    label = normalize_label(df[LABEL_COL].mode().iloc[0] if LABEL_COL in df and not df.empty else None, path.stem)
    duration = float(df[TIME_COL].max() - df[TIME_COL].min()) if TIME_COL in df and len(df) >= 2 else 0.0
    hz = float(len(df) / duration) if duration > 0 else 0.0

    row: dict[str, object] = {
        "文件": path.name,
        "标签": label,
        "行数": len(df),
        "时长_s": round(duration, 3),
        "采样率_Hz": round(hz, 2),
    }
    for col in CHANNELS:
        if col not in df:
            continue
        series = pd.to_numeric(df[col], errors="coerce").dropna()
        if series.empty:
            continue
        row[f"{col}_均值"] = round(float(series.mean()), 3)
        row[f"{col}_最小"] = round(float(series.min()), 3)
        row[f"{col}_最大"] = round(float(series.max()), 3)
        row[f"{col}_标准差"] = round(float(series.std()), 3)
    return row


def plot_preview(path: Path, df: pd.DataFrame, out_dir: Path) -> Path | None:
    cols = [col for col in CHANNELS if col in df]
    if TIME_COL not in df or not cols:
        return None

    label = normalize_label(df[LABEL_COL].mode().iloc[0] if LABEL_COL in df and not df.empty else None, path.stem)
    t = pd.to_numeric(df[TIME_COL], errors="coerce")

    fig, axes = plt.subplots(len(cols), 1, figsize=(12, 2.1 * len(cols)), sharex=True)
    if len(cols) == 1:
        axes = [axes]

    for ax, col in zip(axes, cols):
        y = pd.to_numeric(df[col], errors="coerce")
        ax.plot(t, y, linewidth=0.8)
        ax.set_ylabel(col)
        ax.grid(True, alpha=0.25)

    axes[-1].set_xlabel("时间 / s")
    fig.suptitle(f"{label} - {path.name}", fontsize=14)
    fig.tight_layout()

    out_path = out_dir / f"{safe_filename(path.stem)}_preview.png"
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return out_path


def plot_overview_by_label(files: list[tuple[Path, pd.DataFrame]], out_dir: Path) -> Path | None:
    if not files:
        return None

    fig, ax = plt.subplots(figsize=(12, 6))
    grouped: dict[str, list[pd.DataFrame]] = {}
    for path, df in files:
        if TIME_COL not in df or "平均幅度" not in df or df.empty:
            continue
        label = normalize_label(df[LABEL_COL].mode().iloc[0] if LABEL_COL in df else None, path.stem)
        grouped.setdefault(label, []).append(df)

    plotted = 0
    for label, frames in grouped.items():
        merged = pd.concat(frames, ignore_index=True)
        if merged.empty:
            continue
        # Use packet index instead of absolute time so repeated short trials can be compared together.
        y = pd.to_numeric(merged["平均幅度"], errors="coerce").dropna().reset_index(drop=True)
        if y.empty:
            continue
        window = max(5, min(50, len(y) // 20))
        smoothed = y.rolling(window=window, min_periods=1).mean()
        ax.plot(smoothed.index, smoothed, linewidth=1.1, label=f"{label} ({len(frames)}段)")
        plotted += 1

    if not plotted:
        plt.close(fig)
        return None

    ax.set_title("各类动作平均幅度对比")
    ax.set_xlabel("样本点")
    ax.set_ylabel("平均幅度")
    ax.grid(True, alpha=0.25)
    ax.legend()
    fig.tight_layout()

    out_path = out_dir / "overview_mean_amp_by_label.png"
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return out_path


def make_label_summary(summary_df: pd.DataFrame) -> pd.DataFrame:
    if summary_df.empty or LABEL_COL not in summary_df:
        return pd.DataFrame()

    grouped = summary_df.groupby(LABEL_COL, dropna=False)
    rows: list[dict[str, object]] = []
    for label, group in grouped:
        rows.append({
            "标签": label,
            "文件数": int(len(group)),
            "总行数": int(group["行数"].sum()),
            "总时长_s": round(float(group["时长_s"].sum()), 3),
            "平均采样率_Hz": round(float(group["采样率_Hz"].mean()), 2),
        })
    return pd.DataFrame(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze ESP32 CSI viewer CSV files.")
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA_DIR, help="CSI CSV data directory")
    parser.add_argument("--out", type=Path, default=None, help="analysis output directory")
    args = parser.parse_args()

    data_dir = args.data.expanduser().resolve()
    out_dir = (args.out.expanduser().resolve() if args.out else data_dir / "analysis")
    out_dir.mkdir(exist_ok=True)

    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "Arial Unicode MS", "DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False

    files: list[tuple[Path, pd.DataFrame]] = []
    summaries: list[dict[str, object]] = []

    csv_paths = sorted(path for path in data_dir.glob("*.csv") if path.is_file())
    if not csv_paths:
        print(f"没有找到 CSV 文件: {data_dir}")
        return

    for path in csv_paths:
        df = normalize_time(read_csv(path))
        files.append((path, df))
        summaries.append(summarize_file(path, df))
        preview = plot_preview(path, df, out_dir)
        if preview:
            print(f"预览图: {preview}")

    if summaries:
        summary_df = pd.DataFrame(summaries)
        summary_path = out_dir / "summary.csv"
        summary_df.to_csv(summary_path, index=False, encoding="utf-8-sig")
        print(f"逐文件汇总: {summary_path}")

        label_summary = make_label_summary(summary_df)
        if not label_summary.empty:
            label_summary_path = out_dir / "label_summary.csv"
            label_summary.to_csv(label_summary_path, index=False, encoding="utf-8-sig")
            print(f"按标签汇总: {label_summary_path}")

    overview = plot_overview_by_label(files, out_dir)
    if overview:
        print(f"标签总览图: {overview}")

    print(f"完成，处理 {len(files)} 个 CSV 文件。数据目录: {data_dir}")


if __name__ == "__main__":
    main()
