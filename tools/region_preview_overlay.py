"""Single-region calibration overlay helper.

Run from the project root:
    python tools/region_preview_overlay.py

The control window is excluded from Windows screenshots when supported.
The fullscreen overlay is intentionally capturable so it can be used to
produce calibration-guide images.
"""

from __future__ import annotations

import ctypes
import json
import sys
import tkinter as tk
from pathlib import Path
from tkinter import ttk


ROOT = Path(__file__).resolve().parents[1]
CONFIG_FILE = ROOT / "config" / "config.json"

WDA_EXCLUDEFROMCAPTURE = 0x00000011
WDA_MONITOR = 0x00000001
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_LAYERED = 0x00080000
WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080


REGION_NAMES = {
    "largemap_region": "大地图",
    "minimap_region": "小地图",
    "largemap_1km_px": "1km比例",
    "weapon1_number_region": "武器1编号",
    "weapon2_number_region": "武器2编号",
    "elevation_region": "垂直测高",
    "weapon1_name_region": "武器1名称",
    "weapon2_name_region": "武器2名称",
    "weapon_region": "武器图标",
    "weapon1_scope_region": "武器1倍镜",
    "weapon2_scope_region": "武器2倍镜",
    "stance_region": "姿势区域",
    "weapon1_muzzle_region": "武器1枪口",
    "weapon2_muzzle_region": "武器2枪口",
    "": "四倍镜内边",
    "weapon1_grip_region": "武器1握把",
    "weapon2_grip_region": "武器2握把",
    "scope_top_edge_6x_region": "六倍镜内边",
    "weapon1_stock_region": "武器1枪托",
    "weapon2_stock_region": "武器2枪托",
    "scope_top_edge_8x_region": "八倍镜内边",
    "mortar_mount_region": "迫击炮上炮",
    "compass_region": "顶部方向",
    "crosshair_region": "准星区域",
}


ORDER = [
    "largemap_region",
    "minimap_region",
    "largemap_1km_px",
    "weapon1_number_region",
    "weapon2_number_region",
    "elevation_region",
    "weapon1_name_region",
    "weapon2_name_region",
    "weapon_region",
    "weapon1_scope_region",
    "weapon2_scope_region",
    "stance_region",
    "weapon1_muzzle_region",
    "weapon2_muzzle_region",
    "scope_top_edge_4x_region",
    "weapon1_grip_region",
    "weapon2_grip_region",
    "scope_top_edge_6x_region",
    "weapon1_stock_region",
    "weapon2_stock_region",
    "scope_top_edge_8x_region",
    "mortar_mount_region",
    "compass_region",
    "crosshair_region",
]


def bgr_to_tk(bgr: tuple[int, int, int]) -> str:
    b, g, r = bgr
    return f"#{r:02X}{g:02X}{b:02X}"


def region_color(key: str) -> str:
    if "weapon" in key:
        if "number" in key:
            return bgr_to_tk((255, 107, 53))
        if "name" in key:
            return bgr_to_tk((255, 159, 28))
        if any(part in key for part in ("scope", "grip", "muzzle", "stock")):
            return bgr_to_tk((247, 37, 133))
        return bgr_to_tk((114, 9, 183))
    if "mini" in key:
        return bgr_to_tk((0, 180, 216))
    if "large" in key:
        return bgr_to_tk((0, 119, 182))
    if "stance" in key:
        return bgr_to_tk((46, 196, 182))
    if "elevation" in key:
        return bgr_to_tk((131, 56, 236))
    if "crosshair" in key or "scope_top_edge" in key:
        return bgr_to_tk((255, 0, 110))
    return "#FFFFFF"


def load_regions() -> dict[str, dict[str, int]]:
    with CONFIG_FILE.open("r", encoding="utf-8") as f:
        data = json.load(f)
    raw = data.get("real_regions") or data.get("detection_regions") or {}
    regions: dict[str, dict[str, int]] = {}
    for key, value in raw.items():
        try:
            left = int(round(float(value["left"])))
            top = int(round(float(value["top"])))
            width = int(round(float(value["width"])))
            height = int(round(float(value["height"])))
        except (KeyError, TypeError, ValueError):
            continue
        if width > 0 and height > 0:
            regions[key] = {"left": left, "top": top, "width": width, "height": height}
    return regions


def apply_capture_exclusion(window: tk.Tk | tk.Toplevel) -> None:
    if sys.platform != "win32":
        return
    try:
        hwnd = ctypes.windll.user32.GetParent(window.winfo_id()) or window.winfo_id()
        if not ctypes.windll.user32.SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE):
            ctypes.windll.user32.SetWindowDisplayAffinity(hwnd, WDA_MONITOR)
    except Exception:
        pass


def make_clickthrough(window: tk.Toplevel) -> None:
    if sys.platform != "win32":
        return
    try:
        hwnd = ctypes.windll.user32.GetParent(window.winfo_id()) or window.winfo_id()
        style = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        style |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
        ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style)
    except Exception:
        pass


class RegionPreviewApp:
    def __init__(self) -> None:
        self.root = tk.Tk()
        self.root.title("区域框预览控制")
        self.root.geometry("360x620+80+80")
        self.root.minsize(330, 500)
        self.root.attributes("-topmost", True)

        self.overlay = tk.Toplevel(self.root)
        self.overlay.overrideredirect(True)
        self.overlay.attributes("-topmost", True)
        self.overlay.attributes("-transparentcolor", "black")
        self.overlay.configure(bg="black")
        sw = self.overlay.winfo_screenwidth()
        sh = self.overlay.winfo_screenheight()
        self.overlay.geometry(f"{sw}x{sh}+0+0")
        self.canvas = tk.Canvas(self.overlay, width=sw, height=sh, bg="black", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.overlay.update_idletasks()
        make_clickthrough(self.overlay)

        self.regions: dict[str, dict[str, int]] = {}
        self.current_key: str | None = None
        self.status_var = tk.StringVar(value="")
        self.controls = ttk.Frame(self.root)
        self.controls.pack(fill="both", expand=True)
        self.button_frame: ttk.Frame | None = None
        self.load_and_build()

        self.root.after(300, lambda: apply_capture_exclusion(self.root))
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def load_and_build(self) -> None:
        self.regions = load_regions()
        for child in self.controls.winfo_children():
            child.destroy()

        ttk.Label(self.controls, text="选择单个区域框", font=("Microsoft YaHei", 11, "bold")).pack(pady=(10, 4))
        ttk.Label(
            self.controls,
            text="控制窗口会尽量不被截图捕获；屏幕上的区域框可被截图。",
            wraplength=320,
            justify="center",
        ).pack(padx=12, pady=(0, 8))

        top = ttk.Frame(self.controls)
        top.pack(fill="x", padx=12, pady=(0, 8))
        ttk.Button(top, text="隐藏区域框", command=self.clear_overlay).pack(side="left", expand=True, fill="x", padx=(0, 4))
        ttk.Button(top, text="重新读取配置", command=self.load_and_build).pack(side="left", expand=True, fill="x", padx=(4, 0))

        container = ttk.Frame(self.controls)
        container.pack(fill="both", expand=True, padx=12, pady=(0, 8))
        list_canvas = tk.Canvas(container, highlightthickness=0)
        scrollbar = ttk.Scrollbar(container, orient="vertical", command=list_canvas.yview)
        self.button_frame = ttk.Frame(list_canvas)
        self.button_frame.bind(
            "<Configure>",
            lambda _event: list_canvas.configure(scrollregion=list_canvas.bbox("all")),
        )
        list_canvas.create_window((0, 0), window=self.button_frame, anchor="nw")
        list_canvas.configure(yscrollcommand=scrollbar.set)
        list_canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        ordered = [key for key in ORDER if key in self.regions]
        ordered.extend(sorted(key for key in self.regions if key not in set(ORDER)))
        for idx, key in enumerate(ordered):
            name = REGION_NAMES.get(key, key)
            btn = ttk.Button(self.button_frame, text=name, command=lambda k=key: self.show_region(k))
            btn.grid(row=idx // 2, column=idx % 2, padx=4, pady=4, sticky="ew")
        if self.button_frame:
            self.button_frame.columnconfigure(0, weight=1)
            self.button_frame.columnconfigure(1, weight=1)

        ttk.Label(self.controls, textvariable=self.status_var, wraplength=320).pack(fill="x", padx=12, pady=(0, 8))
        ttk.Button(self.controls, text="退出", command=self.close).pack(fill="x", padx=12, pady=(0, 12))
        self.status_var.set(f"已读取 {len(self.regions)} 个区域。")
        self.clear_overlay()

    def show_region(self, key: str) -> None:
        rect = self.regions.get(key)
        if not rect:
            return
        self.current_key = key
        self.canvas.delete("all")
        color = region_color(key)
        left = rect["left"]
        top = rect["top"]
        right = left + rect["width"]
        bottom = top + rect["height"]
        self.canvas.create_rectangle(left, top, right, bottom, outline=color, width=1)
        self.canvas.create_text(
            left + 5,
            max(5, top - 16),
            text=REGION_NAMES.get(key, key),
            fill=color,
            anchor="nw",
            font=("Microsoft YaHei", 13, "bold"),
        )
        self.status_var.set(
            f"当前显示：{REGION_NAMES.get(key, key)}  "
            f"x={left}, y={top}, w={rect['width']}, h={rect['height']}"
        )

    def clear_overlay(self) -> None:
        self.current_key = None
        self.canvas.delete("all")
        if self.regions:
            self.status_var.set("当前未显示区域框。")

    def close(self) -> None:
        try:
            self.overlay.destroy()
        finally:
            self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


if __name__ == "__main__":
    RegionPreviewApp().run()
