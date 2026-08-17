from __future__ import annotations

from pathlib import Path


def convert_csv(path: Path) -> bool:
    data = path.read_bytes()
    if data.startswith(b"\xef\xbb\xbf"):
        return False

    text = data.decode("utf-8")
    path.write_text(text, encoding="utf-8-sig", newline="")
    return True


def main() -> None:
    data_dir = Path(__file__).with_name("data")
    if not data_dir.exists():
        print("data 文件夹不存在。")
        return

    changed = 0
    for path in data_dir.glob("*.csv"):
        try:
            if convert_csv(path):
                changed += 1
                print(f"已转换: {path.name}")
            else:
                print(f"已是 UTF-8 BOM: {path.name}")
        except UnicodeDecodeError:
            print(f"跳过，无法按 UTF-8 解码: {path.name}")

    print(f"完成，转换 {changed} 个文件。")


if __name__ == "__main__":
    main()
