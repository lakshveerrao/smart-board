"""
SMART BOARD - LABEL RECORDER
============================
Record balance-board motion with a label (neutral, tilt left, touching right, circle...)
so we can teach the computer what "touching the ground" looks like.

HOW TO USE
  1. pip install bleak
  2. Turn on the smart board, then run:   python smart_board_labeler.py
     (no board nearby? test it with:      python smart_board_labeler.py --demo)
  3. Click what you are going to do (e.g. "Touch - Left").
  4. Press SPACE (or the big button). You get 3 seconds to get ready, then it records.
  5. Do the move. It stops by itself (or press SPACE again in "Manual" mode).
  6. BACKSPACE deletes the last take if you messed up.

Everything is saved to:  Desktop/smart_board_data/labels_<date>_<time>.csv
"""

import asyncio
import csv
import math
import queue
import random
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
import tkinter as tk

DEVICE_NAME = "smart board"
ACC_PER_G = 16384.0          # MPU6050 default range (+-2g)
COUNTDOWN_SECONDS = 3
DURATIONS = [("5s", 5), ("10s", 10), ("20s", 20), ("Manual", 0)]
TARGET_SECONDS = 30          # aim for this much data per label

# (group title, colour, [(label_id, button text), ...])
LABEL_GROUPS = [
    ("BALANCE", "#4ade80", [
        ("neutral", "Neutral"),
        ("circle", "Circle"),
        ("wobble", "Wobble"),
    ]),
    ("TILT  (not touching)", "#60a5fa", [
        ("tilt_forward", "Forward"), ("tilt_back", "Back"),
        ("tilt_left", "Left"), ("tilt_right", "Right"),
        ("tilt_fwd_left", "Fwd-Left"), ("tilt_fwd_right", "Fwd-Right"),
        ("tilt_back_left", "Back-Left"), ("tilt_back_right", "Back-Right"),
    ]),
    ("TOUCHING GROUND", "#f87171", [
        ("touch_forward", "Forward"), ("touch_back", "Back"),
        ("touch_left", "Left"), ("touch_right", "Right"),
        ("touch_fwd_left", "Fwd-Left"), ("touch_fwd_right", "Fwd-Right"),
        ("touch_back_left", "Back-Left"), ("touch_back_right", "Back-Right"),
    ]),
]

BG = "#111318"
PANEL = "#1b1e26"
TEXT = "#e8eaf0"
MUTED = "#8a90a0"
REC = "#ef4444"


# ---------------------------------------------------------------- helpers
def desktop_folder() -> Path:
    home = Path.home()
    for cand in (home / "OneDrive" / "Desktop", home / "Desktop"):
        if cand.exists():
            return cand
    return home


def parse_line(line: str):
    """'gx,gy,gz,ax,ay,az,ts' -> dict, or None if it isn't sensor data."""
    parts = [p.strip() for p in line.strip().split(",")]
    if len(parts) < 6:
        return None
    try:
        nums = [float(p) for p in parts[:7]]
    except ValueError:
        return None
    return {
        "gx": nums[0], "gy": nums[1], "gz": nums[2],
        "ax": nums[3], "ay": nums[4], "az": nums[5],
        "device_ms": int(nums[6]) if len(nums) > 6 else "",
    }


def tilt_from_accel(ax, ay, az):
    """Rough roll/pitch in degrees (for the live dot only)."""
    roll = math.degrees(math.atan2(ay, az if az != 0 else 1e-6))
    pitch = math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az) or 1e-6))
    return roll, pitch


# ---------------------------------------------------------------- data sources
class BLESource(threading.Thread):
    """Finds 'smart board', subscribes to every notify characteristic, pushes lines."""

    def __init__(self, out_q: queue.Queue, status_q: queue.Queue):
        super().__init__(daemon=True)
        self.out_q, self.status_q = out_q, status_q
        self.running = True
        self._buf = ""
        self._saw_newline = False

    def status(self, text, ok=False):
        self.status_q.put((text, ok))

    def _on_data(self, _sender, data: bytearray):
        self._buf += data.decode("utf-8", errors="ignore")
        while "\n" in self._buf:
            self._saw_newline = True
            line, self._buf = self._buf.split("\n", 1)
            if line.strip():
                self.out_q.put(line)
        # some firmware sends one full line per packet without a newline; only
        # trust that once we know newlines never come, or a line split mid-number
        # would be emitted cut short
        if not self._saw_newline and self._buf and parse_line(self._buf) \
                and len(self._buf.split(",")) >= 7:
            self.out_q.put(self._buf)
            self._buf = ""

    def run(self):
        asyncio.run(self._main())

    async def _main(self):
        from bleak import BleakClient, BleakScanner

        want = DEVICE_NAME.replace(" ", "").lower()

        def match(dev, adv):
            name = (dev.name or adv.local_name or "").replace(" ", "").lower()
            return want in name

        while self.running:
            try:
                self.status(f"Searching for '{DEVICE_NAME}'...")
                dev = await BleakScanner.find_device_by_filter(match, timeout=8)
                if dev is None:
                    self.status("Board not found - is it on? Retrying...")
                    continue
                self.status("Connecting...")
                async with BleakClient(dev) as client:
                    count = 0
                    for service in client.services:
                        for ch in service.characteristics:
                            if "notify" in ch.properties or "indicate" in ch.properties:
                                try:
                                    await client.start_notify(ch, self._on_data)
                                    count += 1
                                except Exception:
                                    pass
                    if count == 0:
                        self.status("Connected, but board sends no data (no notify characteristic)")
                    else:
                        self.status(f"Connected to {dev.name or DEVICE_NAME}", ok=True)
                    while self.running and client.is_connected:
                        await asyncio.sleep(0.2)
                self.status("Disconnected - reconnecting...")
            except Exception as e:  # keep trying, never crash the app
                self.status(f"Bluetooth problem: {e} - retrying...")
                await asyncio.sleep(2)


class DemoSource(threading.Thread):
    """Fake board for testing without hardware. Follows the selected label."""

    def __init__(self, out_q: queue.Queue, status_q: queue.Queue, app):
        super().__init__(daemon=True)
        self.out_q, self.app = out_q, app
        status_q.put(("DEMO MODE - fake data", True))
        self.running = True

    def run(self):
        t0 = time.time()
        while self.running:
            t = time.time() - t0
            label = self.app.selected or "neutral"
            dirs = {"forward": (0, 1), "back": (0, -1), "left": (-1, 0), "right": (1, 0),
                    "fwd_left": (-0.7, 0.7), "fwd_right": (0.7, 0.7),
                    "back_left": (-0.7, -0.7), "back_right": (0.7, -0.7)}
            dx = dy = 0.0
            spike = 0.0
            if label == "circle":
                dx, dy = math.cos(t * 2) * 10, math.sin(t * 2) * 10
            elif label == "wobble":
                dx, dy = random.uniform(-5, 5), random.uniform(-5, 5)
            elif label.startswith(("tilt_", "touch_")):
                kind, d = label.split("_", 1)
                ang = 9 if kind == "tilt" else 16
                dx, dy = dirs[d][0] * ang, dirs[d][1] * ang
                if kind == "touch" and random.random() < 0.08:
                    spike = random.uniform(0.3, 0.8)
            roll, pitch = math.radians(dx), math.radians(dy)
            ax = -math.sin(pitch) * ACC_PER_G + random.gauss(0, 150)
            ay = math.sin(roll) * ACC_PER_G + random.gauss(0, 150)
            az = math.cos(roll) * math.cos(pitch) * ACC_PER_G * (1 + spike) + random.gauss(0, 150)
            gx, gy, gz = (random.gauss(0, 80) for _ in range(3))
            self.out_q.put(f"{gx:.0f},{gy:.0f},{gz:.0f},{ax:.0f},{ay:.0f},{az:.0f},{int(t*1000)}")
            time.sleep(0.05)


# ---------------------------------------------------------------- app
class App:
    def __init__(self, root: tk.Tk, demo: bool):
        self.root = root
        self.selected = None
        self.state = "idle"            # idle | countdown | recording
        self.duration = tk.IntVar(value=10)
        self.takes = []                # list of dicts: take, label, rows
        self.cur_rows = []
        self.rec_start = 0.0
        self.latest = None
        self.hz_count, self.hz, self.hz_t = 0, 0, time.time()

        out_dir = desktop_folder() / "smart_board_data"
        out_dir.mkdir(parents=True, exist_ok=True)
        self.csv_path = out_dir / f"labels_{datetime.now():%Y%m%d_%H%M%S}.csv"

        self.data_q, self.status_q = queue.Queue(), queue.Queue()
        self._build_ui()
        self.source = DemoSource(self.data_q, self.status_q, self) if demo \
            else BLESource(self.data_q, self.status_q)
        self.source.start()
        self.root.after(20, self._tick)

    # ---------- UI
    def _build_ui(self):
        r = self.root
        r.title("Smart Board - Label Recorder")
        r.configure(bg=BG)
        r.geometry("1040x720")
        r.minsize(900, 640)

        top = tk.Frame(r, bg=BG)
        top.pack(fill="x", padx=16, pady=(14, 6))
        tk.Label(top, text="SMART BOARD", fg=TEXT, bg=BG,
                 font=("Segoe UI", 20, "bold")).pack(side="left")
        self.conn_lbl = tk.Label(top, text="Starting...", fg=MUTED, bg=BG, font=("Segoe UI", 11))
        self.conn_lbl.pack(side="right")

        body = tk.Frame(r, bg=BG)
        body.pack(fill="both", expand=True, padx=16)

        # left: labels
        left = tk.Frame(body, bg=BG)
        left.pack(side="left", fill="both", expand=True)
        self.buttons = {}
        self.colours = {}
        for title, colour, items in LABEL_GROUPS:
            tk.Label(left, text=title, fg=colour, bg=BG,
                     font=("Segoe UI", 11, "bold")).pack(anchor="w", pady=(10, 4))
            grid = tk.Frame(left, bg=BG)
            grid.pack(anchor="w")
            for i, (lid, text) in enumerate(items):
                b = tk.Button(grid, text=text, width=11, height=2, relief="flat",
                              bg=PANEL, fg=TEXT, activebackground="#2c303a", activeforeground=TEXT,
                              bd=0, highlightthickness=0,
                              font=("Segoe UI", 10, "bold"), cursor="hand2",
                              command=lambda l=lid: self.select(l))
                b.grid(row=i // 4, column=i % 4, padx=4, pady=4)
                self.buttons[lid] = (b, text)
                self.colours[lid] = colour

        # right: live view + controls
        right = tk.Frame(body, bg=PANEL, width=360)
        right.pack(side="right", fill="y", padx=(12, 0), pady=6)
        right.pack_propagate(False)

        self.big = tk.Label(right, text="Pick a move", fg=TEXT, bg=PANEL,
                            font=("Segoe UI", 22, "bold"), wraplength=330)
        self.big.pack(pady=(18, 2))
        self.sub = tk.Label(right, text="then press SPACE", fg=MUTED, bg=PANEL,
                            font=("Segoe UI", 11))
        self.sub.pack()

        self.canvas = tk.Canvas(right, width=240, height=240, bg=PANEL, highlightthickness=0)
        self.canvas.pack(pady=12)
        c = self.canvas
        c.create_oval(10, 10, 230, 230, outline="#3a3f4c", width=2)
        c.create_oval(65, 65, 175, 175, outline="#2c303a", width=1)
        c.create_line(120, 10, 120, 230, fill="#2c303a")
        c.create_line(10, 120, 230, 120, fill="#2c303a")
        c.create_text(120, 22, text="FWD", fill=MUTED, font=("Segoe UI", 8))
        self.dot = c.create_oval(110, 110, 130, 130, fill="#4ade80", outline="")

        self.nums = tk.Label(right, text="", fg=MUTED, bg=PANEL, font=("Consolas", 10), justify="left")
        self.nums.pack()

        dur = tk.Frame(right, bg=PANEL)
        dur.pack(pady=(12, 4))
        tk.Label(dur, text="Length:", fg=MUTED, bg=PANEL, font=("Segoe UI", 10)).pack(side="left", padx=4)
        for text, val in DURATIONS:
            tk.Radiobutton(dur, text=text, variable=self.duration, value=val, fg=TEXT, bg=PANEL,
                           selectcolor="#2c303a", activebackground=PANEL, activeforeground=TEXT,
                           bd=0, highlightthickness=0,
                           font=("Segoe UI", 10), takefocus=0).pack(side="left")

        self.go = tk.Button(right, text="START  (Space)", height=2, relief="flat", bg="#2563eb", fg="white",
                            activebackground="#1d4ed8", bd=0, highlightthickness=0, font=("Segoe UI", 13, "bold"),
                            cursor="hand2", command=self.toggle, takefocus=0)
        self.go.pack(fill="x", padx=18, pady=(8, 4))
        tk.Button(right, text="Undo last take  (Backspace)", relief="flat", bg="#2c303a", fg=TEXT,
                  bd=0, highlightthickness=0, activebackground="#3a3f4c", activeforeground=TEXT,
                  font=("Segoe UI", 10), command=self.undo, takefocus=0).pack(fill="x", padx=18)

        self.footer = tk.Label(r, text="", fg=MUTED, bg=BG, font=("Segoe UI", 9), anchor="w")
        self.footer.pack(fill="x", padx=16, pady=(4, 10))

        r.bind("<space>", lambda e: self.toggle())
        r.bind("<BackSpace>", lambda e: self.undo())
        self._refresh_buttons()
        self._refresh_footer()

    def _refresh_buttons(self):
        secs = {}
        for t in self.takes:
            secs[t["label"]] = secs.get(t["label"], 0) + t["seconds"]
        for lid, (b, text) in self.buttons.items():
            s = secs.get(lid, 0)
            tick = "  ✓" if s >= TARGET_SECONDS else ""
            b.configure(text=f"{text}\n{s:.0f}s{tick}")
            if lid == self.selected:
                b.configure(bg=self.colours[lid], fg="#0b0d12")
            else:
                b.configure(bg=PANEL, fg=TEXT)

    def _refresh_footer(self):
        n = sum(len(t["rows"]) for t in self.takes)
        self.footer.configure(
            text=f"{len(self.takes)} takes · {n} samples · {self.hz} Hz   |   saving to {self.csv_path}")

    def _pretty(self, lid):
        for title, _, items in LABEL_GROUPS:
            for l, text in items:
                if l == lid:
                    head = title.split()[0].capitalize()
                    return text if head == "Balance" else f"{head} - {text}"
        return lid

    # ---------- actions
    def select(self, lid):
        if self.state != "idle":
            return
        self.selected = lid
        self.big.configure(text=self._pretty(lid), fg=self.colours[lid])
        self.sub.configure(text="press SPACE to record")
        self._refresh_buttons()

    def toggle(self):
        if self.state == "idle":
            if not self.selected:
                self.sub.configure(text="pick a move first")
                return
            if self.latest is None:
                self.sub.configure(text="no data from the board yet")
                return
            self.state = "countdown"
            self.go.configure(text="CANCEL  (Space)", bg="#52525b")
            self._countdown(COUNTDOWN_SECONDS)
        elif self.state == "countdown":
            self.state = "idle"
            self._reset_controls("cancelled")
        elif self.state == "recording":
            self._stop()

    def _countdown(self, n):
        if self.state != "countdown":
            return
        if n == 0:
            self.state = "recording"
            self.cur_rows = []
            self.rec_start = time.time()
            self.go.configure(text="STOP  (Space)", bg=REC)
            self._beep(1200)
            return
        self.sub.configure(text=f"get ready... {n}", fg=TEXT)
        self._beep(700)
        self.root.after(1000, lambda: self._countdown(n - 1))

    def _stop(self):
        secs = time.time() - self.rec_start
        self.state = "idle"
        if self.cur_rows:
            self.takes.append({"take": len(self.takes) + 1 if not self.takes else self.takes[-1]["take"] + 1,
                               "label": self.selected, "rows": self.cur_rows, "seconds": secs})
            self._save()
            msg = f"saved {len(self.cur_rows)} samples ({secs:.1f}s)"
        else:
            msg = "nothing recorded - is the board sending data?"
        self.cur_rows = []
        self._beep(500)
        self._reset_controls(msg)

    def undo(self):
        if self.state != "idle" or not self.takes:
            return
        t = self.takes.pop()
        self._save()
        self._reset_controls(f"deleted take {t['take']} ({self._pretty(t['label'])})")

    def _reset_controls(self, msg):
        self.go.configure(text="START  (Space)", bg="#2563eb")
        self.sub.configure(text=msg, fg=MUTED)
        self._refresh_buttons()
        self._refresh_footer()

    def _save(self):
        with open(self.csv_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["take", "label", "pc_time", "gx", "gy", "gz", "ax", "ay", "az", "device_ms"])
            for t in self.takes:
                for row in t["rows"]:
                    w.writerow([t["take"], t["label"], f"{row['pc_time']:.3f}",
                                row["gx"], row["gy"], row["gz"],
                                row["ax"], row["ay"], row["az"], row["device_ms"]])

    def _beep(self, freq):
        try:
            import winsound
            threading.Thread(target=winsound.Beep, args=(freq, 120), daemon=True).start()
        except Exception:
            self.root.bell()

    # ---------- loop
    def _tick(self):
        while not self.status_q.empty():
            text, ok = self.status_q.get()
            self.conn_lbl.configure(text=("● " if ok else "○ ") + text, fg="#4ade80" if ok else "#fbbf24")

        got = 0
        while not self.data_q.empty():
            s = parse_line(self.data_q.get())
            if not s:
                continue
            s["pc_time"] = time.time()
            self.latest = s
            got += 1
            if self.state == "recording":
                self.cur_rows.append(s)
        self.hz_count += got

        now = time.time()
        if now - self.hz_t >= 1:
            self.hz, self.hz_count, self.hz_t = self.hz_count, 0, now
            self._refresh_footer()

        if self.latest:
            s = self.latest
            roll, pitch = tilt_from_accel(s["ax"], s["ay"], s["az"])
            g = math.sqrt(s["ax"] ** 2 + s["ay"] ** 2 + s["az"] ** 2) / ACC_PER_G
            scale = 110 / 25.0                      # 25 degrees = edge of circle
            x = 120 + max(-110, min(110, roll * scale))
            y = 120 - max(-110, min(110, pitch * scale))
            self.canvas.coords(self.dot, x - 10, y - 10, x + 10, y + 10)
            self.canvas.itemconfigure(self.dot, fill=REC if self.state == "recording" else "#4ade80")
            self.nums.configure(text=f"roll {roll:6.1f}°   pitch {pitch:6.1f}°   |a| {g:4.2f}g\n"
                                     f"gyro {s['gx']:7.0f} {s['gy']:7.0f} {s['gz']:7.0f}")

        if self.state == "recording":
            el = now - self.rec_start
            dur = self.duration.get()
            left = f"  ·  {dur - el:.0f}s left" if dur else ""
            self.sub.configure(text=f"● RECORDING  {el:.1f}s  ·  {len(self.cur_rows)} samples{left}", fg=REC)
            if dur and el >= dur:
                self._stop()

        self.root.after(20, self._tick)


def main():
    demo = "--demo" in sys.argv
    if not demo:
        try:
            import bleak  # noqa: F401
        except ImportError:
            print("Missing library. Run:  pip install bleak")
            sys.exit(1)
    root = tk.Tk()
    App(root, demo)
    root.mainloop()


if __name__ == "__main__":
    main()
