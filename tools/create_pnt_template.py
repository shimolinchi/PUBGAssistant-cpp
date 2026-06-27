import json
import sys
from pathlib import Path
from typing import Dict, List, Tuple

import cv2
import numpy as np
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = ROOT / "config" / "config.json"
OUTPUT_DIR = ROOT / "assets" / "templates" / "pnt" / "minimap"


ColorRule = Tuple[np.ndarray, np.ndarray]


def load_config() -> dict:
    with CONFIG_PATH.open("r", encoding="utf-8") as f:
        return json.load(f)


def load_modes(config: dict) -> Dict[str, dict]:
    modes = config.get("pnt_color_modes")
    if isinstance(modes, dict) and modes:
        return modes
    colors = config.get("pnt_colors")
    if isinstance(colors, dict) and colors:
        return {"current": colors}
    raise RuntimeError("config.json 中没有找到 pnt_color_modes 或 pnt_colors")


def rules_for_mode(mode_config: dict) -> List[ColorRule]:
    rules: List[ColorRule] = []
    for name in ("Yellow", "Orange", "Blue", "Green"):
        item = mode_config.get(name)
        if not isinstance(item, dict):
            continue
        lower = item.get("lower")
        upper = item.get("upper")
        if not isinstance(lower, list) or not isinstance(upper, list) or len(lower) < 3 or len(upper) < 3:
            continue
        rules.append((
            np.array(lower[:3], dtype=np.uint8),
            np.array(upper[:3], dtype=np.uint8),
        ))
    if not rules:
        raise RuntimeError("该色盲模式下没有可用的标点颜色阈值")
    return rules


def next_output_path() -> Path:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    used = set()
    for file in OUTPUT_DIR.glob("*.png"):
        try:
            used.add(int(file.stem))
        except ValueError:
            pass
    index = 0
    while index in used:
        index += 1
    return OUTPUT_DIR / f"{index}.png"


def build_mask(bgr: np.ndarray, rules: List[ColorRule]) -> np.ndarray:
    hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)
    mask = np.zeros(hsv.shape[:2], dtype=np.uint8)
    for lower, upper in rules:
        mask = cv2.bitwise_or(mask, cv2.inRange(hsv, lower, upper))
    return mask


def trim_to_mask(rgba: np.ndarray, mask: np.ndarray, padding: int = 1) -> np.ndarray:
    points = cv2.findNonZero(mask)
    if points is None:
        raise RuntimeError("没有筛选到任何标点颜色像素，请检查色盲模式或截图是否正确")
    x, y, w, h = cv2.boundingRect(points)
    x1 = max(0, x - padding)
    y1 = max(0, y - padding)
    x2 = min(rgba.shape[1], x + w + padding)
    y2 = min(rgba.shape[0], y + h + padding)
    return rgba[y1:y2, x1:x2]


def create_template(input_path: Path, mode_config: dict) -> Tuple[Path, int, Tuple[int, int]]:
    image = cv2.imdecode(np.fromfile(str(input_path), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if image is None or image.size == 0:
        raise RuntimeError(f"无法读取图片：{input_path}")

    if image.ndim == 2:
        bgr = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    elif image.shape[2] == 4:
        bgr = image[:, :, :3]
    else:
        bgr = image[:, :, :3]

    mask = build_mask(bgr, rules_for_mode(mode_config))
    rgba = np.zeros((bgr.shape[0], bgr.shape[1], 4), dtype=np.uint8)
    rgba[mask > 0] = (0, 0, 0, 255)
    rgba = trim_to_mask(rgba, mask)

    out_path = next_output_path()
    ok, encoded = cv2.imencode(".png", rgba)
    if not ok:
        raise RuntimeError("PNG 编码失败")
    encoded.tofile(str(out_path))
    return out_path, int(cv2.countNonZero(rgba[:, :, 3])), (rgba.shape[1], rgba.shape[0])


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("标点模板制作")
        self.geometry("430x210")
        self.resizable(False, False)

        self.config_data = load_config()
        self.modes = load_modes(self.config_data)
        default_mode = self.config_data.get("pnt_color_mode", "normal")
        if default_mode not in self.modes:
            default_mode = next(iter(self.modes))

        self.mode_var = tk.StringVar(value=default_mode)
        self.file_var = tk.StringVar(value="")

        frame = ttk.Frame(self, padding=18)
        frame.pack(fill="both", expand=True)

        ttk.Label(frame, text="色盲模式").grid(row=0, column=0, sticky="w")
        mode_box = ttk.Combobox(frame, textvariable=self.mode_var, values=list(self.modes.keys()), state="readonly", width=22)
        mode_box.grid(row=0, column=1, sticky="ew", padx=(12, 0))

        ttk.Label(frame, text="截图 PNG").grid(row=1, column=0, sticky="w", pady=(14, 0))
        ttk.Entry(frame, textvariable=self.file_var, state="readonly").grid(row=1, column=1, sticky="ew", padx=(12, 8), pady=(14, 0))
        ttk.Button(frame, text="选择", command=self.choose_file).grid(row=1, column=2, pady=(14, 0))

        hint = (
            "脚本会筛选配置中的标点色块，输出黑色形状 + 透明背景，\n"
            "并自动保存到 assets/templates/pnt/minimap。"
        )
        ttk.Label(frame, text=hint, foreground="#555").grid(row=2, column=0, columnspan=3, sticky="w", pady=(16, 0))

        ttk.Button(frame, text="生成模板", command=self.generate).grid(row=3, column=0, columnspan=3, pady=(18, 0))
        frame.columnconfigure(1, weight=1)

    def choose_file(self) -> None:
        path = filedialog.askopenfilename(
            title="选择标点截图 PNG",
            filetypes=[("PNG 图片", "*.png"), ("所有图片", "*.png;*.jpg;*.jpeg;*.bmp"), ("所有文件", "*.*")]
        )
        if path:
            self.file_var.set(path)

    def generate(self) -> None:
        if not self.file_var.get():
            messagebox.showwarning("缺少图片", "请先选择一张标点截图。")
            return
        try:
            out_path, pixels, size = create_template(Path(self.file_var.get()), self.modes[self.mode_var.get()])
        except Exception as exc:
            messagebox.showerror("生成失败", str(exc))
            return
        messagebox.showinfo(
            "生成成功",
            f"已保存：\n{out_path}\n\n模板尺寸：{size[0]} x {size[1]}\n有效像素：{pixels}"
        )


def main() -> int:
    try:
        app = App()
        app.mainloop()
        return 0
    except Exception as exc:
        print(f"生成工具启动失败：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
