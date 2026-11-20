#!/usr/bin/env python3
"""Interactive UI QA for myOS: boots the normal ISO headless in QEMU and drives it
through QMP with real PS/2 mouse/keyboard events (input-send-event), taking a
PNG screenshot after each step into build/qa/. Usage: python3 -I tools/qa_ui.py
"""
import json, os, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "qa")
SOCK = os.path.join(ROOT, "build", "qa.sock")
DISK = os.path.join(ROOT, "build", "disk-qa.img")
SERIAL = os.path.join(OUT, "serial.log")


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            msg = json.loads(self.f.readline())
            if "return" in msg:
                return msg["return"]
            if "error" in msg:
                raise RuntimeError(msg["error"])

    def events(self, evs):
        self.cmd("input-send-event", events=evs)


class UI:
    def __init__(self, q):
        self.q, self.x, self.y, self.n = q, 0, 0, 0

    def move_rel(self, dx, dy):
        # PS/2 packets carry at most +-255; QEMU splits, but keep steps small and paced.
        while dx or dy:
            sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
            self.q.events([{"type": "rel", "data": {"axis": "x", "value": sx}},
                           {"type": "rel", "data": {"axis": "y", "value": sy}}])
            dx -= sx; dy -= sy
            time.sleep(0.02)
        time.sleep(0.15)

    def home(self):
        self.move_rel(-1500, -1000)
        self.x, self.y = 0, 0

    def goto(self, x, y):
        self.move_rel(x - self.x, y - self.y)
        self.x, self.y = x, y

    def button(self, down):
        self.q.events([{"type": "btn", "data": {"down": down, "button": "left"}}])
        time.sleep(0.15)

    def click(self, x, y):
        self.goto(x, y); self.button(True); self.button(False); time.sleep(0.3)

    def dblclick(self, x, y):
        self.goto(x, y)
        for down in (True, False, True, False):      # well inside the 400 ms double-click window
            self.q.events([{"type": "btn", "data": {"down": down, "button": "left"}}]); time.sleep(0.04)
        time.sleep(0.4)

    def drag(self, x0, y0, x1, y1):
        self.goto(x0, y0); self.button(True)
        steps = 8
        for i in range(1, steps + 1):
            self.goto(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.button(False); time.sleep(0.3)

    def key(self, qcode):
        for down in (True, False):
            self.q.events([{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])
            time.sleep(0.05)

    def chord(self, mods, qcode):
        evs = lambda down: [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": k}}}
                            for k in (mods + [qcode] if down else [qcode] + mods[::-1])]
        self.q.events(evs(True)); time.sleep(0.05); self.q.events(evs(False)); time.sleep(0.1)

    def type(self, text):
        names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}
        for ch in text:
            if ch == ">":
                self.chord(["shift"], "dot")
            elif ch == "_":
                self.chord(["shift"], "minus")
            else:
                self.key(names.get(ch, ch))

    def shot(self, name):
        self.n += 1
        path = os.path.join(OUT, f"{self.n:02d}-{name}.png")
        time.sleep(0.4)
        self.q.cmd("screendump", filename=path, format="png")
        print("  shot", os.path.relpath(path, ROOT))
        return path


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    shutil.copy(os.path.join(ROOT, "build", "disk.img"), DISK)
    if os.path.exists(SOCK):
        os.remove(SOCK)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-M", "q35", "-m", "256M", "-no-reboot", "-rtc", "base=localtime",
        "-nic", "user,model=e1000", "-boot", "order=d",
        "-drive", f"file={DISK},format=raw,if=none,id=d0", "-device", "ide-hd,drive=d0,bus=ide.0",
        "-display", "none", "-serial", f"file:{SERIAL}", "-qmp", f"unix:{SOCK},server,nowait",
        "-cdrom", os.path.join(ROOT, "build", "myos.iso")], cwd=ROOT)
    try:
        q = QMP(SOCK)
        ui = UI(q)
        t0 = time.time()
        while "[OK] desktop" not in open(SERIAL, errors="replace").read():
            if time.time() - t0 > 30:
                sys.exit("desktop never came up")
            time.sleep(0.2)
        time.sleep(1.0)
        ui.shot("boot")

        ui.home()
        ui.goto(635, 761); ui.shot("hover-dock-clock")          # magnification + tooltip, dy direction
        ui.click(635, 761); ui.shot("open-clock")
        ui.drag(680, 219, 380, 419); ui.shot("drag-welcome")      # title bar drag (-300, +200)
        ui.click(188, 163); ui.shot("close-notes")                # Notes red light
        ui.click(138 + 20, 419); ui.shot("minimize-welcome")      # welcome yellow light after drag
        ui.click(803, 761); ui.shot("restore-welcome-from-dock")
        ui.click(22, 11); ui.shot("logo-menu")
        ui.key("esc"); ui.click(1000, 600); ui.shot("menu-dismissed")
        for k in ["h", "e", "l", "l", "o", "ret"]:
            ui.key(k)
        ui.shot("after-typing")

        # stress: open/close the clock repeatedly through the dock and its close light
        for i in range(25):
            ui.click(635, 761)
            time.sleep(0.2)
        ui.shot("clock-reopened-25x")
        time.sleep(3)
        ui.shot("clock-3s-later")

        # traffic lights on the Clock window (opens at 154,126; lights at y+14, x+18/+38/+58)
        ui.click(154 + 18, 126 + 14); ui.shot("clock-closed-red")
        ui.click(635, 761); ui.click(154 + 38, 126 + 14); ui.shot("clock-minimized-yellow")
        ui.click(635, 761); ui.shot("clock-restored-dock")
        ui.click(154 + 58, 126 + 14); ui.shot("clock-zoom-green")
        # Welcome "Get Started" button closes the window (welcome frame at 100,410 after the drag)
        ui.click(803, 761); ui.click(100 + 560 - 32 - 60, 410 + 28 + 256 + 8); ui.shot("welcome-get-started")

        # phase 11 apps, opened from the dock (9 icons: files 411, terminal 467, editor 523, ... settings 747)
        ui.click(411, 761); ui.shot("open-files")
        ui.click(467, 761); ui.shot("open-terminal")
        for k in ["l", "s", "ret", "p", "w", "d", "ret"]:
            ui.key(k)
        ui.shot("terminal-ls-pwd")
        ui.click(523, 761); ui.shot("open-editor")
        for k in ["h", "i", "spc", "m", "y", "o", "s", "ret"]:
            ui.key(k)
        ui.shot("editor-typing")
        ui.click(747, 761); ui.shot("open-settings")
        ui.click(22, 11); ui.click(60, 42); ui.shot("about-from-logo-menu")

        # --- deeper app checks: Files -> Editor hand-off, read-only save, Terminal file ops and fetch
        ui.click(22, 11); ui.click(60, 95); ui.shot("close-all")          # logo menu > Close All Windows
        ui.click(411, 761); ui.dblclick(300, 310); ui.shot("files-open-readme")  # double click README.txt
        ui.chord(["ctrl"], "s"); ui.shot("editor-save-readonly")
        ui.click(467, 761)
        ui.type("echo hola > /disk/qa.txt\n"); ui.type("cat /disk/qa.txt\n")
        ui.type("mv /disk/qa.txt /disk/qb.txt\n"); ui.type("ls /disk\n")
        ui.type("rm /disk/qb.txt\n"); ui.type("ls /disk\n"); ui.type("echo x > /nope.txt\n")
        ui.type("echo keep > /disk/s.txt\n"); ui.type("cp /disk/s.txt /disk/s.txt\n"); ui.type("mv /disk/s.txt /disk\n")
        ui.type("cat /disk/s.txt\n"); ui.type("rm /disk/s.txt\n")
        ui.shot("terminal-same-file")
        ui.shot("terminal-file-ops")
        ui.type("fetch example.com /\n"); time.sleep(8)
        ui.shot("terminal-fetch")

        # Browser (dock icon 6 of 9 at x=691): home page, then load example.com through the address bar
        ui.click(691, 761); ui.shot("browser-home")
        ui.chord(["ctrl"], "l"); ui.type("example.com\n"); time.sleep(14)
        ui.shot("browser-example-com")
    finally:
        try:
            q.cmd("quit")
        except Exception:
            pass
        qemu.wait(timeout=10)
        log = open(SERIAL, errors="replace").read()
        expected = ("user/faulty", "[arch] user exception 14")   # intentional ring-3 fault selftest
        bad = [l for l in log.splitlines()
               if any(k in l for k in ("[FAIL]", "exception", "panic", "PANIC")) and not any(e in l for e in expected)]
        print("serial problems:", bad if bad else "none")


if __name__ == "__main__":
    main()
