from __future__ import annotations

import argparse
import json
from collections import Counter
from datetime import datetime
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import ExtraTreesClassifier, RandomForestClassifier
from sklearn.metrics import classification_report, confusion_matrix, precision_recall_fscore_support
from sklearn.model_selection import StratifiedGroupKFold


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATA_DIR = SCRIPT_DIR / "data"
DEFAULT_OUT_DIR = SCRIPT_DIR / "models"

TIME_COL = "\u65f6\u95f4_s"
LABEL_COL = "\u6807\u7b7e"
CHANNELS = [
    "\u4fe1\u53f7\u5f3a\u5ea6",
    "\u5e73\u5747\u5e45\u5ea6",
    "\u5b50\u8f7d\u6ce28",
    "\u5b50\u8f7d\u6ce224",
    "\u5b50\u8f7d\u6ce248",
    "\u5b50\u8f7d\u6ce280",
]

LABEL_ALIASES = {
    "\u5176\u4ed6": "\u5f2f\u8170\u6361\u4e1c\u897f",
}
FALL_LABEL = "\u8dcc\u5012"
FEATURE_EXTRACTOR_VERSION = 2

OLD_CHANNEL_STATS = [
    "mean",
    "std",
    "min",
    "max",
    "range",
    "q25",
    "q75",
    "iqr",
    "rms",
    "energy",
    "diff_abs_mean",
    "diff_abs_max",
    "diff_std",
    "delta",
    "slope",
]

NEW_CHANNEL_STATS = [
    "median",
    "mad",
    "rel_std",
    "rel_range",
    "rel_iqr",
    "abs_slope",
    "first_half_mean",
    "second_half_mean",
    "half_delta",
    "diff_q90",
    "diff_energy",
    "diff_peak_to_mean",
    "fft_low_ratio",
    "fft_high_ratio",
    "fft_peak_bin",
    "peak_index_norm",
    "zero_cross_rate",
]

GLOBAL_FEATURES = [
    "all_mean",
    "all_std",
    "all_range",
    "all_diff_abs_mean",
    "all_diff_abs_max",
    "all_duplicate_ratio",
    "all_max_repeat_run",
    "amp_mean",
    "amp_std",
    "amp_range",
    "amp_diff_abs_mean",
    "amp_diff_abs_max",
    "amp_channel_corr_mean",
    "amp_channel_corr_max",
    "amp_channel_corr_min",
    "amp_channel_mean_std",
    "amp_channel_range_mean",
    "rssi_amp_corr_abs_mean",
    "sub8_to_mean_amp",
    "sub24_to_mean_amp",
    "sub48_to_mean_amp",
    "sub80_to_mean_amp",
    "sub80_to_sub24",
]


def read_csv(path: Path) -> pd.DataFrame:
    for encoding in ("utf-8-sig", "utf-8", "gbk"):
        try:
            return pd.read_csv(path, encoding=encoding)
        except UnicodeDecodeError:
            continue
    return pd.read_csv(path)


def normalize_label(label: object, fallback: str) -> str:
    text = str(label).strip() if label is not None else ""
    if not text or text == "nan":
        text = fallback
    return LABEL_ALIASES.get(text, text)


def label_from_frame(path: Path, df: pd.DataFrame) -> str:
    if LABEL_COL in df and not df.empty:
        mode = df[LABEL_COL].dropna().mode()
        if not mode.empty:
            return normalize_label(mode.iloc[0], path.stem)
    return normalize_label(path.stem, path.stem)


def detect_sample_rate(df: pd.DataFrame, fallback: float) -> float:
    if TIME_COL not in df:
        return fallback
    t = pd.to_numeric(df[TIME_COL], errors="coerce").dropna().to_numpy()
    if t.size < 3:
        return fallback
    duration = float(np.nanmax(t) - np.nanmin(t))
    if duration <= 0:
        return fallback
    return float(t.size / duration)


def feature_names() -> list[str]:
    names: list[str] = []
    for channel in CHANNELS:
        for stat in OLD_CHANNEL_STATS + NEW_CHANNEL_STATS:
            names.append(f"{channel}_{stat}")
    names.extend(GLOBAL_FEATURES)
    return names


def safe_div(numerator: float, denominator: float, eps: float = 1e-6) -> float:
    if abs(denominator) < eps:
        return 0.0
    return float(numerator / denominator)


def max_repeat_run(row_repeat: np.ndarray) -> int:
    best = 0
    current = 0
    for repeated in row_repeat:
        if repeated:
            current += 1
            best = max(best, current)
        else:
            current = 0
    return best


def spectral_features(series: np.ndarray) -> tuple[float, float, float]:
    if series.size < 4:
        return 0.0, 0.0, 0.0

    centered = series.astype(float) - float(np.mean(series))
    if np.allclose(centered, 0.0):
        return 0.0, 0.0, 0.0

    power = np.abs(np.fft.rfft(centered)) ** 2
    if power.size <= 1:
        return 0.0, 0.0, 0.0

    power = power[1:]
    total = float(np.sum(power))
    if total <= 1e-9:
        return 0.0, 0.0, 0.0

    split = max(1, int(round(power.size * 0.35)))
    low_ratio = float(np.sum(power[:split]) / total)
    high_ratio = float(np.sum(power[split:]) / total) if split < power.size else 0.0
    peak_bin = float((int(np.argmax(power)) + 1) / max(series.size, 1))
    return low_ratio, high_ratio, peak_bin


def extract_window_feature_dict(window: np.ndarray) -> dict[str, float]:
    feats: dict[str, float] = {}
    window = window.astype(float)

    for idx in range(window.shape[1]):
        channel = CHANNELS[idx]
        series = window[:, idx]
        diff = np.diff(series)
        abs_diff = np.abs(diff)
        q25, median, q75 = np.percentile(series, [25, 50, 75])
        mean = float(np.mean(series))
        std = float(np.std(series))
        min_value = float(np.min(series))
        max_value = float(np.max(series))
        value_range = max_value - min_value
        iqr = float(q75 - q25)
        rms = float(np.sqrt(np.mean(series * series)))
        energy = float(np.mean(series * series))
        mad = float(np.median(np.abs(series - median)))
        denom = max(series.size - 1, 1)
        delta = float(series[-1] - series[0])
        slope = float(delta / denom)
        first_half = series[: max(1, series.size // 2)]
        second_half = series[series.size // 2 :]
        first_half_mean = float(np.mean(first_half))
        second_half_mean = float(np.mean(second_half)) if second_half.size else first_half_mean
        fft_low_ratio, fft_high_ratio, fft_peak_bin = spectral_features(series)
        centered = series - median
        zero_cross_rate = float(np.mean(centered[:-1] * centered[1:] < 0)) if series.size > 1 else 0.0
        diff_abs_mean = float(np.mean(abs_diff)) if abs_diff.size else 0.0
        diff_abs_max = float(np.max(abs_diff)) if abs_diff.size else 0.0

        values = {
            "mean": mean,
            "std": std,
            "min": min_value,
            "max": max_value,
            "range": value_range,
            "q25": float(q25),
            "q75": float(q75),
            "iqr": iqr,
            "rms": rms,
            "energy": energy,
            "diff_abs_mean": diff_abs_mean,
            "diff_abs_max": diff_abs_max,
            "diff_std": float(np.std(diff)) if diff.size else 0.0,
            "delta": delta,
            "slope": slope,
            "median": float(median),
            "mad": mad,
            "rel_std": safe_div(std, abs(mean)),
            "rel_range": safe_div(value_range, abs(mean)),
            "rel_iqr": safe_div(iqr, abs(median)),
            "abs_slope": abs(slope),
            "first_half_mean": first_half_mean,
            "second_half_mean": second_half_mean,
            "half_delta": second_half_mean - first_half_mean,
            "diff_q90": float(np.percentile(abs_diff, 90)) if abs_diff.size else 0.0,
            "diff_energy": float(np.mean(diff * diff)) if diff.size else 0.0,
            "diff_peak_to_mean": safe_div(diff_abs_max, diff_abs_mean),
            "fft_low_ratio": fft_low_ratio,
            "fft_high_ratio": fft_high_ratio,
            "fft_peak_bin": fft_peak_bin,
            "peak_index_norm": float(int(np.argmax(np.abs(centered))) / max(series.size - 1, 1)),
            "zero_cross_rate": zero_cross_rate,
        }
        for stat, value in values.items():
            feats[f"{channel}_{stat}"] = float(value)

    flat = window
    flat_diff = np.diff(flat, axis=0)
    row_repeat = np.sum(np.abs(flat_diff), axis=1) == 0 if flat_diff.size else np.array([], dtype=bool)
    amp = window[:, 1:] if window.shape[1] > 1 else window
    amp_diff = np.diff(amp, axis=0)

    feats.update({
        "all_mean": float(np.mean(flat)),
        "all_std": float(np.std(flat)),
        "all_range": float(np.max(flat) - np.min(flat)),
        "all_diff_abs_mean": float(np.mean(np.abs(flat_diff))) if flat_diff.size else 0.0,
        "all_diff_abs_max": float(np.max(np.abs(flat_diff))) if flat_diff.size else 0.0,
        "all_duplicate_ratio": float(np.mean(row_repeat)) if row_repeat.size else 0.0,
        "all_max_repeat_run": float(max_repeat_run(row_repeat)),
        "amp_mean": float(np.mean(amp)),
        "amp_std": float(np.std(amp)),
        "amp_range": float(np.max(amp) - np.min(amp)),
        "amp_diff_abs_mean": float(np.mean(np.abs(amp_diff))) if amp_diff.size else 0.0,
        "amp_diff_abs_max": float(np.max(np.abs(amp_diff))) if amp_diff.size else 0.0,
    })

    if amp.shape[1] >= 2 and amp.shape[0] >= 3:
        corr = np.corrcoef(amp, rowvar=False)
        corr = np.nan_to_num(corr, nan=0.0, posinf=0.0, neginf=0.0)
        upper = corr[np.triu_indices_from(corr, k=1)]
        feats["amp_channel_corr_mean"] = float(np.mean(upper)) if upper.size else 0.0
        feats["amp_channel_corr_max"] = float(np.max(upper)) if upper.size else 0.0
        feats["amp_channel_corr_min"] = float(np.min(upper)) if upper.size else 0.0
    else:
        feats["amp_channel_corr_mean"] = 0.0
        feats["amp_channel_corr_max"] = 0.0
        feats["amp_channel_corr_min"] = 0.0

    feats["amp_channel_mean_std"] = float(np.std(np.mean(amp, axis=0))) if amp.ndim == 2 else 0.0
    feats["amp_channel_range_mean"] = float(np.mean(np.max(amp, axis=0) - np.min(amp, axis=0))) if amp.ndim == 2 else 0.0

    if window.shape[1] >= 2 and window.shape[0] >= 3:
        rssi = window[:, 0]
        rssi_corrs: list[float] = []
        for idx in range(1, window.shape[1]):
            if np.std(rssi) <= 1e-9 or np.std(window[:, idx]) <= 1e-9:
                rssi_corrs.append(0.0)
            else:
                rssi_corrs.append(float(abs(np.corrcoef(rssi, window[:, idx])[0, 1])))
        feats["rssi_amp_corr_abs_mean"] = float(np.mean(rssi_corrs)) if rssi_corrs else 0.0
    else:
        feats["rssi_amp_corr_abs_mean"] = 0.0

    mean_amp = feats.get(f"{CHANNELS[1]}_mean", 0.0) if len(CHANNELS) > 1 else 0.0
    sub8 = feats.get(f"{CHANNELS[2]}_mean", 0.0) if len(CHANNELS) > 2 else 0.0
    sub24 = feats.get(f"{CHANNELS[3]}_mean", 0.0) if len(CHANNELS) > 3 else 0.0
    sub48 = feats.get(f"{CHANNELS[4]}_mean", 0.0) if len(CHANNELS) > 4 else 0.0
    sub80 = feats.get(f"{CHANNELS[5]}_mean", 0.0) if len(CHANNELS) > 5 else 0.0
    feats["sub8_to_mean_amp"] = safe_div(sub8, mean_amp)
    feats["sub24_to_mean_amp"] = safe_div(sub24, mean_amp)
    feats["sub48_to_mean_amp"] = safe_div(sub48, mean_amp)
    feats["sub80_to_mean_amp"] = safe_div(sub80, mean_amp)
    feats["sub80_to_sub24"] = safe_div(sub80, sub24)
    return feats


def extract_window_features(window: np.ndarray) -> list[float]:
    feature_map = extract_window_feature_dict(window)
    return [feature_map.get(name, 0.0) for name in feature_names()]


def load_windows(
    data_dir: Path,
    mode: str,
    sample_rate: float,
    window_s: float,
    step_s: float,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    csv_paths = sorted(path for path in data_dir.glob("*.csv") if path.is_file())
    if not csv_paths:
        raise FileNotFoundError(f"No CSV files found in {data_dir}")

    rows: list[dict[str, object]] = []
    file_rows: list[dict[str, object]] = []
    names = feature_names()

    for path in csv_paths:
        df = read_csv(path)
        missing = [col for col in CHANNELS if col not in df]
        if missing:
            continue

        label = label_from_frame(path, df)
        target = "fall" if label == FALL_LABEL else "non_fall" if mode == "binary" else label
        file_rate = detect_sample_rate(df, sample_rate)
        window_n = max(10, int(round(window_s * file_rate)))
        step_n = max(1, int(round(step_s * file_rate)))

        work = df[CHANNELS].apply(pd.to_numeric, errors="coerce").dropna()
        arr = work.to_numpy(dtype=float)
        if len(arr) < window_n:
            file_rows.append({
                "file": path.name,
                "label": label,
                "target": target,
                "rows": len(arr),
                "windows": 0,
                "sample_rate_hz": round(file_rate, 3),
            })
            continue

        file_windows = 0
        for start in range(0, len(arr) - window_n + 1, step_n):
            window = arr[start:start + window_n]
            feats = extract_window_features(window)
            row = dict(zip(names, feats, strict=True))
            row.update({
                "label": label,
                "target": target,
                "group": path.name,
                "start_index": start,
                "window_rows": window_n,
            })
            rows.append(row)
            file_windows += 1

        file_rows.append({
            "file": path.name,
            "label": label,
            "target": target,
            "rows": len(arr),
            "windows": file_windows,
            "sample_rate_hz": round(file_rate, 3),
        })

    if not rows:
        raise RuntimeError("No usable windows were extracted. Check CSV columns and window size.")
    return pd.DataFrame(rows), pd.DataFrame(file_rows)


def build_model(seed: int, n_estimators: int, estimator: str) -> RandomForestClassifier | ExtraTreesClassifier:
    if estimator == "extra_trees":
        return ExtraTreesClassifier(
            n_estimators=n_estimators,
            min_samples_leaf=1,
            max_features="sqrt",
            class_weight="balanced",
            n_jobs=-1,
            random_state=seed,
        )

    return RandomForestClassifier(
        n_estimators=n_estimators,
        min_samples_leaf=2,
        max_features="sqrt",
        class_weight="balanced_subsample",
        n_jobs=-1,
        random_state=seed,
    )


def balanced_train_indices(
    y: np.ndarray,
    rng: np.random.Generator,
    mode: str,
    nonfall_ratio: float,
) -> np.ndarray:
    indices_by_class = {label: np.flatnonzero(y == label) for label in np.unique(y)}
    if mode == "binary" and "fall" in indices_by_class and "non_fall" in indices_by_class:
        fall_idx = indices_by_class["fall"]
        nonfall_idx = indices_by_class["non_fall"]
        nonfall_limit = min(len(nonfall_idx), max(1, int(round(len(fall_idx) * nonfall_ratio))))
        keep = [
            rng.choice(fall_idx, size=len(fall_idx), replace=False),
            rng.choice(nonfall_idx, size=nonfall_limit, replace=False),
        ]
    else:
        min_count = min(len(idx) for idx in indices_by_class.values())
        keep = [rng.choice(idx, size=min_count, replace=False) for idx in indices_by_class.values()]

    selected = np.concatenate(keep)
    rng.shuffle(selected)
    return selected


def predict_with_threshold(
    model: RandomForestClassifier | ExtraTreesClassifier,
    x: np.ndarray,
    labels: list[str],
    threshold: float,
) -> np.ndarray:
    if labels != ["fall", "non_fall"]:
        return model.predict(x)
    prob = model.predict_proba(x)
    fall_col = list(model.classes_).index("fall")
    return np.where(prob[:, fall_col] >= threshold, "fall", "non_fall")


def threshold_rows(y_true: np.ndarray, prob_fall: np.ndarray) -> list[dict[str, float]]:
    rows: list[dict[str, float]] = []
    y_bin = (y_true == "fall").astype(int)
    for threshold in np.arange(0.20, 0.76, 0.05):
        y_pred = (prob_fall >= threshold).astype(int)
        precision, recall, f1, _ = precision_recall_fscore_support(
            y_bin,
            y_pred,
            average="binary",
            zero_division=0,
        )
        false_alarm = float(np.mean((y_pred == 1) & (y_bin == 0)))
        rows.append({
            "threshold": round(float(threshold), 2),
            "fall_precision": round(float(precision), 4),
            "fall_recall": round(float(recall), 4),
            "fall_f1": round(float(f1), 4),
            "window_false_alarm_rate": round(false_alarm, 4),
        })
    return rows


def run_group_cv(
    x: np.ndarray,
    y: np.ndarray,
    groups: np.ndarray,
    labels: list[str],
    args: argparse.Namespace,
) -> tuple[np.ndarray, dict[str, object]]:
    splitter = StratifiedGroupKFold(n_splits=args.cv, shuffle=True, random_state=args.seed)
    pred = np.empty(y.shape, dtype=object)
    prob_fall = np.full(y.shape, np.nan, dtype=float)
    fold_rows: list[dict[str, object]] = []

    for fold, (train_idx, test_idx) in enumerate(splitter.split(x, y, groups), start=1):
        rng = np.random.default_rng(args.seed + fold)
        balanced_idx = train_idx[balanced_train_indices(y[train_idx], rng, args.mode, args.nonfall_ratio)]
        model = build_model(args.seed + fold, args.n_estimators, args.estimator)
        model.fit(x[balanced_idx], y[balanced_idx])

        fold_pred = predict_with_threshold(model, x[test_idx], labels, args.threshold)
        pred[test_idx] = fold_pred

        if labels == ["fall", "non_fall"]:
            prob = model.predict_proba(x[test_idx])
            fall_col = list(model.classes_).index("fall")
            prob_fall[test_idx] = prob[:, fall_col]

        accuracy = float(np.mean(fold_pred == y[test_idx]))
        fold_rows.append({
            "fold": fold,
            "accuracy": round(accuracy, 4),
            "train_windows": int(len(train_idx)),
            "train_windows_balanced": int(len(balanced_idx)),
            "test_windows": int(len(test_idx)),
            "test_groups": int(len(set(groups[test_idx]))),
        })

    metrics: dict[str, object] = {
        "folds": fold_rows,
        "mean_accuracy": round(float(np.mean([row["accuracy"] for row in fold_rows])), 4),
        "labels": labels,
        "estimator": args.estimator,
    }
    if labels == ["fall", "non_fall"]:
        metrics["threshold_table"] = threshold_rows(y, prob_fall)
    return pred, metrics


def estimator_score(y: np.ndarray, pred: np.ndarray, labels: list[str]) -> float:
    accuracy = float(np.mean(pred == y))
    if labels != ["fall", "non_fall"]:
        return accuracy

    precision, recall, f1, _ = precision_recall_fscore_support(
        y,
        pred,
        labels=["fall"],
        average="macro",
        zero_division=0,
    )
    return float(0.35 * accuracy + 0.25 * precision + 0.25 * recall + 0.15 * f1)


def select_estimator(
    x: np.ndarray,
    y: np.ndarray,
    groups: np.ndarray,
    labels: list[str],
    args: argparse.Namespace,
) -> tuple[str, np.ndarray, dict[str, object]]:
    if args.estimator != "auto":
        return args.estimator, *run_group_cv(x, y, groups, labels, args)

    candidates: list[dict[str, object]] = []
    best_name = "extra_trees"
    best_pred: np.ndarray | None = None
    best_metrics: dict[str, object] | None = None
    best_score = -1.0

    for estimator in ("extra_trees", "rf"):
        trial_args = argparse.Namespace(**vars(args))
        trial_args.estimator = estimator
        pred, metrics = run_group_cv(x, y, groups, labels, trial_args)
        score = estimator_score(y, pred, labels)
        row = {
            "estimator": estimator,
            "score": round(score, 4),
            "mean_accuracy": metrics["mean_accuracy"],
        }
        if labels == ["fall", "non_fall"]:
            precision, recall, f1, _ = precision_recall_fscore_support(
                y,
                pred,
                labels=["fall"],
                average="macro",
                zero_division=0,
            )
            row.update({
                "fall_precision": round(float(precision), 4),
                "fall_recall": round(float(recall), 4),
                "fall_f1": round(float(f1), 4),
            })
        candidates.append(row)
        if score > best_score:
            best_name = estimator
            best_pred = pred
            best_metrics = metrics
            best_score = score

    if best_pred is None or best_metrics is None:
        raise RuntimeError("Estimator auto-selection failed.")

    best_metrics["estimator_candidates"] = candidates
    best_metrics["selected_estimator"] = best_name
    best_metrics["selection_score"] = round(best_score, 4)
    args.estimator = best_name
    return best_name, best_pred, best_metrics


def save_outputs(
    out_dir: Path,
    model: RandomForestClassifier | ExtraTreesClassifier,
    features: pd.DataFrame,
    file_summary: pd.DataFrame,
    y: np.ndarray,
    pred: np.ndarray,
    labels: list[str],
    cv_metrics: dict[str, object],
    args: argparse.Namespace,
) -> dict[str, str]:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = out_dir / f"csi_{args.mode}_{timestamp}"
    run_dir.mkdir(parents=True, exist_ok=True)

    feature_cols = feature_names()
    bundle = {
        "model": model,
        "feature_names": feature_cols,
        "feature_extractor_version": FEATURE_EXTRACTOR_VERSION,
        "estimator": args.estimator,
        "channels": CHANNELS,
        "labels": labels,
        "mode": args.mode,
        "window_s": args.window_s,
        "step_s": args.step_s,
        "sample_rate": args.sample_rate,
        "threshold": args.threshold if args.mode == "binary" else None,
        "label_aliases": LABEL_ALIASES,
    }

    model_path = run_dir / "csi_model.joblib"
    joblib.dump(bundle, model_path)

    features_path = run_dir / "window_features.csv"
    features.to_csv(features_path, index=False, encoding="utf-8-sig")

    file_summary_path = run_dir / "file_summary.csv"
    file_summary.to_csv(file_summary_path, index=False, encoding="utf-8-sig")

    report_text = classification_report(y, pred, labels=labels, zero_division=0, digits=4)
    report_path = run_dir / "classification_report.txt"
    report_path.write_text(report_text, encoding="utf-8")

    cm = pd.DataFrame(confusion_matrix(y, pred, labels=labels), index=labels, columns=labels)
    cm_path = run_dir / "confusion_matrix.csv"
    cm.to_csv(cm_path, encoding="utf-8-sig")

    importances = pd.DataFrame({
        "feature": feature_cols,
        "importance": model.feature_importances_,
    }).sort_values("importance", ascending=False)
    importance_path = run_dir / "feature_importance.csv"
    importances.to_csv(importance_path, index=False, encoding="utf-8-sig")

    metadata = {
        "created_at": timestamp,
        "data_dir": str(args.data),
        "mode": args.mode,
        "window_s": args.window_s,
        "step_s": args.step_s,
        "sample_rate": args.sample_rate,
        "threshold": args.threshold if args.mode == "binary" else None,
        "feature_extractor_version": FEATURE_EXTRACTOR_VERSION,
        "estimator": args.estimator,
        "windows": int(len(features)),
        "files": int(file_summary["file"].nunique()),
        "window_counts": dict(Counter(y)),
        "file_counts": file_summary.groupby("target")["file"].count().to_dict(),
        "cv": cv_metrics,
        "paths": {
            "model": str(model_path),
            "features": str(features_path),
            "file_summary": str(file_summary_path),
            "report": str(report_path),
            "confusion_matrix": str(cm_path),
            "feature_importance": str(importance_path),
        },
    }
    metrics_path = run_dir / "metrics.json"
    metrics_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")

    return {key: str(value) for key, value in metadata["paths"].items()} | {"run_dir": str(run_dir), "metrics": str(metrics_path)}


def main() -> None:
    parser = argparse.ArgumentParser(description="Train a lightweight ESP32 CSI fall-detection model.")
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA_DIR, help="CSI CSV data directory")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT_DIR, help="model output directory")
    parser.add_argument("--mode", choices=("binary", "multiclass"), default="binary", help="training target")
    parser.add_argument("--window-s", type=float, default=2.0, help="sliding window length in seconds")
    parser.add_argument("--step-s", type=float, default=0.5, help="sliding window step in seconds")
    parser.add_argument("--sample-rate", type=float, default=50.0, help="fallback sample rate in Hz")
    parser.add_argument("--threshold", type=float, default=0.50, help="fall probability threshold in binary mode")
    parser.add_argument("--nonfall-ratio", type=float, default=2.0, help="max non-fall windows per fall window during training")
    parser.add_argument("--cv", type=int, default=5, help="StratifiedGroupKFold splits")
    parser.add_argument("--n-estimators", type=int, default=500, help="tree count")
    parser.add_argument("--estimator", choices=("auto", "extra_trees", "rf"), default="auto", help="tree model to train")
    parser.add_argument("--seed", type=int, default=7, help="random seed")
    args = parser.parse_args()

    args.data = args.data.expanduser().resolve()
    args.out = args.out.expanduser().resolve()

    features, file_summary = load_windows(
        args.data,
        args.mode,
        args.sample_rate,
        args.window_s,
        args.step_s,
    )

    feature_cols = feature_names()
    x = features[feature_cols].to_numpy(dtype=float)
    y = features["target"].to_numpy(dtype=object)
    groups = features["group"].to_numpy(dtype=object)
    labels = ["fall", "non_fall"] if args.mode == "binary" else sorted(pd.unique(y).tolist())

    group_counts = pd.Series(groups).groupby(pd.Series(y)).nunique()
    args.cv = max(2, min(args.cv, int(group_counts.min())))

    selected_estimator, pred, cv_metrics = select_estimator(x, y, groups, labels, args)

    rng = np.random.default_rng(args.seed)
    balanced_idx = balanced_train_indices(y, rng, args.mode, args.nonfall_ratio)
    final_model = build_model(args.seed, args.n_estimators, selected_estimator)
    final_model.fit(x[balanced_idx], y[balanced_idx])

    paths = save_outputs(args.out, final_model, features, file_summary, y, pred, labels, cv_metrics, args)

    print("Training complete.")
    print(f"Mode: {args.mode}")
    print(f"Estimator: {selected_estimator}")
    print(f"Feature extractor: v{FEATURE_EXTRACTOR_VERSION} ({len(feature_cols)} features)")
    print(f"Files: {file_summary['file'].nunique()}  Windows: {len(features)}")
    print("Window counts:")
    for label, count in Counter(y).items():
        print(f"  {label}: {count}")
    print(f"CV mean accuracy: {cv_metrics['mean_accuracy']}")
    if "estimator_candidates" in cv_metrics:
        print("Estimator candidates:")
        print(pd.DataFrame(cv_metrics["estimator_candidates"]).to_string(index=False))
    print("Classification report:")
    print(classification_report(y, pred, labels=labels, zero_division=0, digits=4))
    if args.mode == "binary":
        table = pd.DataFrame(cv_metrics["threshold_table"])
        print("Threshold table:")
        print(table.to_string(index=False))
    print("Outputs:")
    for name, path in paths.items():
        print(f"  {name}: {path}")


if __name__ == "__main__":
    main()
