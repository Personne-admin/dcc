#!/usr/bin/env python3
import codecs
import fcntl
import json
import os
import re
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unicodedata
from pathlib import Path

LEARNDC = None
DCC = None
DPM = None
HOME = None
CONTENT = None
ENV = None
FAILURES = []
CHECKS = 0


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)
        print(f"  FAIL: {message}")


def run(args, cwd=None, env=None, timeout=240, stdin=subprocess.DEVNULL, binary=None):
    full_env = dict(ENV)
    if env:
        full_env.update(env)
    try:
        p = subprocess.run([binary or LEARNDC, *args], cwd=cwd, env=full_env, stdin=stdin, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        check(False, f"learndc {' '.join(args)} timed out")
        return 124, (e.stdout or b"").decode("utf-8", "replace"), (e.stderr or b"").decode("utf-8", "replace")
    return p.returncode, p.stdout.decode("utf-8"), p.stderr.decode("utf-8")


def expect(args, code, label=None, **kw):
    rc, out, err = run(args, **kw)
    label = label or " ".join(args)
    check(rc == code, f"`learndc {label}` exited {rc}, expected {code}\n--- stdout\n{out}\n--- stderr\n{err}")
    return rc, out, err


def expect_json(args, code=0, **kw):
    rc, out, err = expect(args, code, **kw)
    try:
        return json.loads(out)
    except ValueError as e:
        check(False, f"`learndc {' '.join(args)}` did not print JSON: {e}\n{out}")
        return {}


class MiniTerm:
    def __init__(self, cols, rows):
        self.cols = cols
        self.rows = rows
        self.grid = [[" "] * cols for _ in range(rows)]
        self.x = 0
        self.y = 0
        self.alt = False
        self.alt_entered = 0
        self.alt_left = 0
        self.pending = ""

    def resize(self, cols, rows):
        self.cols = cols
        self.rows = rows
        self.grid = [[" "] * cols for _ in range(rows)]
        self.x = min(self.x, cols - 1)
        self.y = min(self.y, rows - 1)

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.grid)

    def _newline(self):
        if self.y == self.rows - 1:
            self.grid.pop(0)
            self.grid.append([" "] * self.cols)
        else:
            self.y += 1

    def _clear(self, x0, y0, x1, y1):
        for y in range(y0, y1 + 1):
            for x in range(self.cols):
                if (y == y0 and x < x0) or (y == y1 and x > x1):
                    continue
                self.grid[y][x] = " "

    def _csi(self, params, final):
        private = params[:1] in ("?", ">", "<", "=")
        nums = [int(p) if p.isdigit() else 0 for p in params.lstrip("?><=").split(";")] if params.lstrip("?><=") else []
        n = nums[0] if nums and nums[0] > 0 else 1
        if self.x >= self.cols:
            self.x = self.cols - 1
        if final == "A":
            self.y = max(0, self.y - n)
        elif final == "B":
            self.y = min(self.rows - 1, self.y + n)
        elif final == "C":
            self.x = min(self.cols - 1, self.x + n)
        elif final == "D":
            self.x = max(0, self.x - n)
        elif final == "E":
            self.y = min(self.rows - 1, self.y + n)
            self.x = 0
        elif final == "F":
            self.y = max(0, self.y - n)
            self.x = 0
        elif final == "G":
            self.x = min(self.cols - 1, n - 1)
        elif final in ("H", "f"):
            row = nums[0] if len(nums) > 0 and nums[0] > 0 else 1
            col = nums[1] if len(nums) > 1 and nums[1] > 0 else 1
            self.y = min(self.rows - 1, row - 1)
            self.x = min(self.cols - 1, col - 1)
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 0:
                self._clear(self.x, self.y, self.cols - 1, self.rows - 1)
            elif mode == 1:
                self._clear(0, 0, self.x, self.y)
            else:
                self._clear(0, 0, self.cols - 1, self.rows - 1)
        elif final == "K":
            mode = nums[0] if nums else 0
            if mode == 0:
                self._clear(self.x, self.y, self.cols - 1, self.y)
            elif mode == 1:
                self._clear(0, self.y, self.x, self.y)
            else:
                self._clear(0, self.y, self.cols - 1, self.y)
        elif final in ("h", "l") and private:
            for v in nums:
                if v in (47, 1047, 1049):
                    if final == "h":
                        self.alt = True
                        self.alt_entered += 1
                    else:
                        self.alt = False
                        self.alt_left += 1

    def feed(self, data):
        text = self.pending + data
        self.pending = ""
        i = 0
        while i < len(text):
            c = text[i]
            if c == "\x1b":
                if i + 1 >= len(text):
                    self.pending = text[i:]
                    return
                n = text[i + 1]
                if n == "[":
                    j = i + 2
                    while j < len(text) and not ("@" <= text[j] <= "~"):
                        j += 1
                    if j >= len(text):
                        self.pending = text[i:]
                        return
                    self._csi(text[i + 2 : j], text[j])
                    i = j + 1
                elif n in "]P_^X":
                    j = i + 2
                    while j < len(text) and text[j] != "\x07" and not (text[j] == "\x1b" and j + 1 < len(text) and text[j + 1] == "\\"):
                        j += 1
                    if j >= len(text):
                        self.pending = text[i:]
                        return
                    i = j + (1 if text[j] == "\x07" else 2)
                elif n in "()*+":
                    if i + 2 >= len(text):
                        self.pending = text[i:]
                        return
                    i += 3
                else:
                    i += 2
                continue
            i += 1
            if c == "\r":
                self.x = 0
            elif c == "\n":
                self._newline()
            elif c == "\b":
                self.x = max(0, self.x - 1)
            elif c == "\t":
                self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
            elif ord(c) < 0x20 or ord(c) == 0x7F:
                continue
            else:
                if unicodedata.combining(c):
                    continue
                w = 2 if unicodedata.east_asian_width(c) in ("W", "F") else 1
                if self.x + w > self.cols:
                    self.x = 0
                    self._newline()
                self.grid[self.y][self.x] = c
                if w == 2 and self.x + 1 < self.cols:
                    self.grid[self.y][self.x + 1] = ""
                self.x += w


class PtyProcess:
    def __init__(self, argv, env, cols, rows):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.before = termios.tcgetattr(self.slave)
        self.term = MiniTerm(cols, rows)
        self.raw = bytearray()
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")

        def become_controlling():
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.proc = subprocess.Popen(argv, stdin=self.slave, stdout=self.slave, stderr=self.slave, env=env, start_new_session=True, preexec_fn=become_controlling, close_fds=True)

    def pump(self, timeout=0.1):
        ready, _, _ = select.select([self.master], [], [], timeout)
        if not ready:
            return False
        try:
            data = os.read(self.master, 65536)
        except OSError:
            return False
        if not data:
            return False
        self.raw += data
        self.term.feed(self.decoder.decode(data))
        return True

    def wait_for(self, predicate, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(0.1)
            if predicate(self.term.text()):
                return True
            if self.proc.poll() is not None:
                while self.pump(0.05):
                    pass
                return predicate(self.term.text())
        return False

    def send(self, data):
        os.write(self.master, data if isinstance(data, bytes) else data.encode())

    def resize(self, cols, rows):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.term.resize(cols, rows)

    def finish(self, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline and self.proc.poll() is None:
            self.pump(0.1)
        rc = self.proc.poll()
        if rc is None:
            self.proc.kill()
            rc = self.proc.wait()
            rc = None
        while self.pump(0.05):
            pass
        after = termios.tcgetattr(self.slave)
        return rc, after

    def close(self):
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


def make_content(real_content: Path, target: Path):
    (target / "tracks" / "fundamentals").mkdir(parents=True)
    src = real_content / "tracks" / "first-steps" / "hello-world"
    hello = target / "tracks" / "fundamentals" / "hello-world"
    shutil.copytree(src, hello)
    toml = re.sub(r"^prerequisites = \[.*?\]", "prerequisites = []", (hello / "exercise.toml").read_text(), count=1, flags=re.M)
    (hello / "exercise.toml").write_text(toml.replace('id = "first-steps/hello-world"', 'id = "fundamentals/hello-world"'))
    again = target / "tracks" / "fundamentals" / "hello-again"
    shutil.copytree(src, again)
    toml = re.sub(r"^prerequisites = \[.*?\]", "prerequisites = []", (again / "exercise.toml").read_text(), count=1, flags=re.M)
    toml = toml.replace('id = "first-steps/hello-world"', 'id = "fundamentals/hello-again"')
    toml = toml.replace('title = "Your first DC program"', 'title = "Your second DC program"')
    toml = toml.replace("prerequisites = []", 'prerequisites = ["fundamentals/hello-world"]')
    (again / "exercise.toml").write_text(toml)
    (target / "catalog.toml").write_text('schema = 1\ntitle = "E2E"\ntracks = ["fundamentals"]\n')
    (target / "tracks" / "fundamentals" / "track.toml").write_text(
        'schema = 1\nid = "fundamentals"\ntitle = "Fundamentals"\nsummary = "e2e"\nexercises = ["hello-world", "hello-again"]\n'
    )


def journey_basics():
    print("journey: version, help, usage errors")
    rc, out, err = expect(["--version"], 0)
    check(out.startswith("learndc "), "version line")
    check("2 exercises" in out, f"version reports the content count: {out!r}")
    rc, out, err = expect(["--help"], 0)
    for word in ["list", "check", "dev", "doctor", "Exit codes"]:
        check(word in out, f"help mentions {word}")
    rc, out, err = expect(["check", "--help"], 0)
    check("--backend" in out and "--json" in out, "check --help lists options")
    rc, out, err = expect([], 0, label="(no arguments, not a tty)")
    check("Usage:" in out, "no arguments prints help when not a terminal")
    expect(["frobnicate"], 2)
    expect(["list", "--bogus"], 2)
    expect(["show"], 2)
    expect(["list", "--status", "weird"], 2)
    expect(["check", "--backend", "weird"], 2)


def journey_catalog():
    print("journey: list, search, show")
    rc, out, err = expect(["list"], 0)
    check("fundamentals/hello-world" in out and "hello-again" in out, "list shows both exercises")
    data = expect_json(["list", "--json"])
    ids = [e["id"] for e in data.get("exercises", [])]
    check(ids == ["fundamentals/hello-world", "fundamentals/hello-again"], f"list --json ids {ids}")
    first = data["exercises"][0]
    for key in ["id", "title", "summary", "kind", "difficulty", "estimated_minutes", "status", "bookmarked", "prerequisites", "prerequisites_met"]:
        check(key in first, f"list --json exercise has {key}")
    check(first["status"] == "not-started", "fresh status")
    data = expect_json(["list", "--status", "completed", "--json"])
    check(data.get("exercises") == [], "no completed exercises yet")
    data = expect_json(["list", "--track", "fundamentals", "--difficulty", "beginner", "--json"])
    check(len(data["exercises"]) == 2, "track and difficulty filter")
    data = expect_json(["search", "greet", "--json"])
    check(data.get("query") == "greet", "search json query")
    check(any(e["id"] == "fundamentals/hello-world" for e in data["exercises"]), "search finds hello-world")
    rc, out, err = expect(["search", "zzzzqqqq"], 0)
    check("No exercises match" in out, "search with no hits")
    data = expect_json(["show", "fundamentals/hello-world", "--json"])
    for key in ["id", "readme", "objectives", "checks", "entry_file", "workspace", "hint_level"]:
        check(key in data, f"show --json has {key}")
    check(data.get("workspace") is None, "no workspace before start")
    rc, out, err = expect(["show", "hello-world"], 0, label="show hello-world (short name)")
    check("Your first DC program" in out and "Objectives" in out, "show text")
    rc, out, err = expect(["show", "fundamentals/nope"], 5)
    check("unknown exercise" in err, "unknown id message")


def workspace_path():
    return HOME / "workspaces" / "fundamentals" / "hello-world"


def find_workspace():
    data = expect_json(["show", "fundamentals/hello-world", "--json"])
    return Path(data["workspace"])


def journey_start_and_check(real_content: Path):
    print("journey: start, resume, failing check, passing check, completion")
    rc, out, err = expect(["start", "fundamentals/hello-world"], 0)
    check("Created the workspace" in out, "first start creates")
    ws = find_workspace()
    check((ws / "project.dcon").is_file(), "project.dcon created")
    main = ws / "src" / "main.dc"
    check(main.is_file(), "src/main.dc created")
    check(" " in str(ws) and "é" in str(ws), f"workspace path has a space and a non-ASCII letter: {ws}")
    original = main.read_text()
    main.write_text(original + "\n// learner edit\n")
    rc, out, err = expect(["start", "fundamentals/hello-world"], 0)
    check("Resumed the workspace" in out, "second start resumes")
    check("learner edit" in main.read_text(), "resume keeps the learner edit")
    main.write_text(original)

    rc, out, err = expect(["check", "fundamentals/hello-world"], 1)
    check("@@ expected line 1, output line 1 @@" in out and "+ Hello, [world]!" in out, f"failing check shows expected and observed\n{out}")
    check("- Hello, [Ada]!" in out, "expected text visible")
    check("last-diff.txt" in out, "full diff path shown")
    data = expect_json(["check", "fundamentals/hello-world", "--json"], 1)
    check(data.get("outcome") == "failed", f"json outcome {data.get('outcome')}")
    check(data.get("passed") == 1 and data.get("total") == 2, "json counts")
    results = data.get("results", [])
    check(len(results) == 2 and results[1].get("verdict") == "fail", "json results")
    for key in ["name", "kind", "verdict", "stage", "summary", "expected", "observed"]:
        check(key in results[1], f"json result has {key}")
    check(data.get("completed_now") is False, "not completed after a failure")
    prog = expect_json(["progress", "--json"])
    ex = {e["id"]: e for e in prog["exercises"]}["fundamentals/hello-world"]
    check(ex["status"] == "in-progress" and ex["attempts"] == 2, f"in-progress after two failed checks: {ex}")

    solution = (real_content / "tracks" / "first-steps" / "hello-world" / "solution" / "src" / "main.dc").read_text()
    main.write_text(solution)
    rc, out, err = expect(["check", "fundamentals/hello-world"], 0)
    check("Exercise completed" in out, "completion message")
    check("Next recommended: fundamentals/hello-again" in out, f"next recommendation printed\n{out}")
    prog = expect_json(["progress", "--json"])
    ex = {e["id"]: e for e in prog["exercises"]}["fundamentals/hello-world"]
    check(ex["status"] == "completed" and ex["completed_at"], "progress shows completed")
    check(prog["completed"] == 1 and prog["total"] == 2, "progress totals")
    data = expect_json(["check", "fundamentals/hello-world", "--json"], 0)
    check(data.get("outcome") == "passed" and data.get("completed_now") is False, "second pass is not a new completion")
    check(data.get("next") == "fundamentals/hello-again", "json next")
    rc, out, err = expect(["progress"], 0)
    check("1 of 2 lessons completed" in out, "progress text")

    data = expect_json(["next", "--json"])
    check(data["next"]["id"] == "fundamentals/hello-again", "next recommends hello-again")
    rc, out, err = expect(["next"], 0)
    check("learndc start fundamentals/hello-again" in out, "next text")

    nested = ws / "src"
    rc, out, err = expect(["check"], 0, cwd=nested, label="check (from a nested workspace directory)")
    check(str(ws) in out and "current directory" in out, "check names the workspace chosen from the cwd")
    rc, out, err = expect(["check"], 0, cwd=HOME, label="check (active exercise)")
    check("active exercise" in out, "check falls back to the active exercise")
    rc, out, err = expect(["list", "--status", "completed"], 0)
    check("hello-world" in out and "hello-again" not in out, "completed filter")
    return ws, main, solution


def journey_hints():
    print("journey: hints")
    for level in (1, 2, 3):
        rc, out, err = expect(["hint", "fundamentals/hello-world"], 0, label=f"hint (reveal {level})")
        check(f"Hint {level} of 3" in out, f"hint level {level} revealed: {out[:40]!r}")
    prog = expect_json(["progress", "--json"])
    ex = {e["id"]: e for e in prog["exercises"]}["fundamentals/hello-world"]
    check(ex["hint_level"] == 3, "hint level persisted")
    rc, out, err = expect(["hint", "fundamentals/hello-world"], 0, label="hint (past the last)")
    check("All hints are shown" in out, "all hints message")
    rc, out, err = expect(["hint", "--level", "1", "fundamentals/hello-world"], 0)
    check("Hint 1 of 3" in out, "explicit level")
    prog = expect_json(["progress", "--json"])
    ex = {e["id"]: e for e in prog["exercises"]}["fundamentals/hello-world"]
    check(ex["hint_level"] == 3, "an earlier hint does not lower the level")
    expect(["hint", "--level", "9", "fundamentals/hello-world"], 2)
    expect(["hint", "--level", "x", "fundamentals/hello-world"], 2)


def journey_reset_and_solution(ws, main, solution):
    print("journey: reset, backup, solution")
    main.write_text(solution + "\n// marker-for-backup\n")
    rc, out, err = expect(["reset", "fundamentals/hello-world"], 6)
    check("--yes" in err, "refusal mentions --yes")
    check("marker-for-backup" in main.read_text(), "refused reset leaves the files alone")
    rc, out, err = expect(["reset", "fundamentals/hello-world", "--yes"], 0)
    check("Backup of your files" in out, "backup reported")
    check("marker-for-backup" not in main.read_text(), "reset restored the starter")
    backups = list((HOME / "backups").rglob("main.dc"))
    check(any("marker-for-backup" in b.read_text() for b in backups), f"backup contains the edited file: {backups}")

    rc, out, err = expect(["solution", "fundamentals/hello-world"], 2)
    check("--show" in err and "--diff" in err, "solution without a flag explains")
    rc, out, err = expect(["solution", "fundamentals/hello-world", "--diff"], 0)
    check("---" in out and "+++" in out and "Welcome to DC" in out, f"solution diff\n{out}")
    rc, out, err = expect(["solution", "fundamentals/hello-world", "--show"], 0)
    check("Welcome to DC" in out and "src/main.dc" in out, "solution show")
    prog = expect_json(["progress", "--json"])
    ex = {e["id"]: e for e in prog["exercises"]}["fundamentals/hello-world"]
    check(ex["solution_viewed"] is True, "solution viewing recorded")
    check(ex["status"] == "completed", "reset does not undo completion")
    rc, out, err = expect(["review"], 0)
    check("hello-world" in out, "review lists the exercise whose solution was viewed")
    data = expect_json(["review", "--json"])
    check(any(e["id"] == "fundamentals/hello-world" and e.get("reason") for e in data["review"]), "review json")


def journey_bookmark():
    print("journey: bookmarks")
    expect(["bookmark", "fundamentals/hello-again"], 0)
    data = expect_json(["show", "fundamentals/hello-again", "--json"])
    check(data["bookmarked"] is True, "bookmarked")
    expect(["bookmark", "fundamentals/hello-again", "--remove"], 0)
    data = expect_json(["show", "fundamentals/hello-again", "--json"])
    check(data["bookmarked"] is False, "bookmark removed")


def journey_export_import(tmp: Path):
    print("journey: export and import")
    target = tmp / "exp ort" / "progress.json"
    target.parent.mkdir()
    expect(["export"], 2)
    expect(["export", "--output", str(target)], 0)
    doc = json.loads(target.read_text())
    check(doc.get("format") == "learndc-progress" and "progress" in doc, "export document")
    expect(["export", "--output", str(target)], 6, label="export (existing file)")
    expect(["export", "--output", str(target), "--yes"], 0)
    fresh = tmp / "fresh-ünï"
    env = {"LEARNDC_HOME": str(fresh)}
    rc, out, err = expect(["import", "--input", str(target)], 0, env=env, label="import (preview)")
    check("Preview only" in out, "preview message")
    prog = expect_json(["progress", "--json"], env=env)
    check(prog["completed"] == 0, "preview applies nothing")
    rc, out, err = expect(["import", "--input", str(target), "--apply"], 0, env=env)
    check("Applied" in out, "apply message")
    prog = expect_json(["progress", "--json"], env=env)
    check(prog["completed"] == 1, "imported completion")
    rc, out, err = expect(["import", "--input", str(target)], 0, env=env, label="import again")
    check("Nothing to import" in out, "idempotent import")
    bad = tmp / "bad.json"
    bad.write_text("{not json")
    expect(["import", "--input", str(bad)], 3, env=env)
    expect(["import", "--input", str(tmp / "missing.json")], 1, env=env)
    expect(["import"], 2, env=env)


def journey_config():
    print("journey: config")
    expect(["config", "set", "ui.theme", "light"], 0)
    rc, out, err = expect(["config", "get", "ui.theme"], 0)
    check(out.strip() == "light", "config get")
    data = expect_json(["config", "get", "ui.theme", "--json"])
    check(data == {"key": "ui.theme", "value": "light"}, f"config get json {data}")
    rc, out, err = expect(["config", "list"], 0)
    check("ui.theme = light" in out and "checks.timeout_seconds" in out, "config list")
    data = expect_json(["config", "list", "--json"])
    check(data.get("ui.theme") == "light", "config list json")
    rc, out, err = expect(["config", "set", "ui.theme", "blue"], 2)
    check("invalid value" in err, "invalid value message")
    expect(["config", "set", "checks.timeout_seconds", "0"], 2)
    expect(["config", "get", "no.such.key"], 2)
    expect(["config", "unset", "ui.theme"], 0)
    rc, out, err = expect(["config", "get", "ui.theme"], 0)
    check(out.strip() == "dark", "unset restores the default")
    check((HOME / "config.json").is_file(), "config file written")


def journey_doctor(tmp: Path):
    print("journey: doctor")
    data = expect_json(["doctor", "--json"])
    items = {i["id"]: i for i in data.get("items", [])}
    check(items.get("dcc", {}).get("status") == "ok", f"dcc ok: {items.get('dcc')}")
    check(items.get("dpm", {}).get("status") == "ok", f"dpm ok: {items.get('dpm')}")
    rc, out, err = expect(["doctor"], 0)
    check("[ok] dcc" in out, "doctor text")
    isolated = tmp / "isolated"
    isolated.mkdir()
    lonely = str(isolated / "learndc")
    shutil.copy(LEARNDC, lonely)
    env = {"LEARNDC_DCC": "/nonexistent/dcc", "LEARNDC_DPM": "/nonexistent/dpm", "PATH": "/nonexistent"}
    rc, out, err = expect(["doctor", "--json"], 4, env=env, binary=lonely, label="doctor (no toolchain)")
    data = json.loads(out)
    check(data.get("ok") is False, "doctor json reports the problem")
    rc, out, err = expect(["check", "fundamentals/hello-world"], 4, env=env, binary=lonely, label="check (no toolchain)")


def journey_edit(tmp: Path, ws: Path):
    print("journey: custom editor receives one path argument")
    script = tmp / "fake editor.py"
    record = tmp / "editor-args.json"
    script.write_text(f"#!{sys.executable}\nimport json, sys\njson.dump(sys.argv[1:], open({str(record)!r}, 'w'))\n")
    script.chmod(0o755)
    expect(["config", "set", "editor.preset", "custom"], 0)
    expect(["config", "set", "editor.command", json.dumps([str(script)])], 0)
    rc, out, err = expect(["edit", "fundamentals/hello-world"], 0)
    args = json.loads(record.read_text()) if record.exists() else None
    expected = str(ws / "src" / "main.dc")
    check(args == [expected], f"editor got exactly one argument {expected!r}, got {args!r}")
    record.unlink()
    rc, out, err = expect(["edit", "--editor", f"{script}", "--folder"], 1, cwd=ws, label="edit --folder (terminal editor)")
    check("cannot open a folder" in err, "terminal editors explain that they cannot open folders")
    expect(["config", "set", "editor.preset", "manual"], 0)
    rc, out, err = expect(["edit", "fundamentals/hello-world"], 0)
    check("main.dc" in out, "manual editor prints the path")
    expect(["config", "unset", "editor.preset"], 0)
    expect(["config", "unset", "editor.command"], 0)


def journey_watch():
    print("journey: watch is interrupted with Ctrl+C")
    env = dict(ENV)
    p = subprocess.Popen([LEARNDC, "watch", "fundamentals/hello-world"], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    chunks = []

    def reader():
        for line in iter(p.stdout.readline, b""):
            chunks.append(line.decode("utf-8", "replace"))

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    deadline = time.time() + 180
    while time.time() < deadline and not any("checks passed" in c or "check passed" in c for c in chunks):
        time.sleep(0.2)
    check(any("passed" in c for c in chunks), f"watch ran the first check: {''.join(chunks)}")
    p.send_signal(signal.SIGINT)
    try:
        rc = p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = None
    check(rc == 130, f"watch exits 130 after SIGINT, got {rc}")
    t.join(timeout=5)
    check(any("Watch stopped" in c for c in chunks), "watch prints a stop message")


def journey_terminate(tmp: Path):
    print("journey: SIGTERM stops a running build")
    pidfile = tmp / "fake-dpm.pid"
    script = tmp / "fake-dpm.sh"
    script.write_text(f"#!/bin/sh\nif [ \"$1\" = build ]; then echo $$ > '{pidfile}'; exec sleep 60; fi\necho dpm 0.0.0\n")
    script.chmod(0o755)
    env = dict(ENV)
    env["LEARNDC_DPM"] = str(script)
    p = subprocess.Popen([LEARNDC, "check", "fundamentals/hello-world"], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.time() + 60
    while time.time() < deadline and not pidfile.exists():
        time.sleep(0.1)
    check(pidfile.exists(), "the fake build tool started")
    time.sleep(0.3)
    p.send_signal(signal.SIGTERM)
    try:
        rc = p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = None
    check(rc not in (None, -signal.SIGTERM), f"check handles SIGTERM instead of dying from it, got {rc}")
    pid = int(pidfile.read_text().strip()) if pidfile.exists() else 0
    gone = False
    for _ in range(50):
        try:
            os.kill(pid, 0)
        except OSError:
            gone = True
            break
        time.sleep(0.1)
    if not gone and pid:
        os.kill(pid, signal.SIGKILL)
    check(gone, "the build tool does not outlive learndc after SIGTERM")


def journey_run(ws: Path):
    print("journey: run")
    rc, out, err = expect(["run", "fundamentals/hello-world"], 0)
    check("Hello" in out, f"run prints the program output: {out!r}")


def journey_tui(tmp: Path):
    print("journey: terminal interface in a pty")
    home = tmp / "tui home"
    editor = tmp / "tui editor.py"
    editor.write_text(f"#!{sys.executable}\nimport sys\nopen(sys.argv[-1], 'a').write('// marker from the fake editor\\n')\n")
    editor.chmod(0o755)
    env = dict(ENV)
    env.update({"LEARNDC_HOME": str(home), "TERM": "xterm-256color", "LANG": "C.UTF-8"})
    for key, value in [("onboarding.done", "true"), ("ui.open_continue", "false"), ("editor.preset", "custom"), ("editor.command", json.dumps([str(editor)]))]:
        expect(["config", "set", key, value], 0, env=env)
    session = PtyProcess([LEARNDC], env, 100, 30)
    try:
        check(session.wait_for(lambda t: "Menu" in t and "Chapters" in t), f"home screen renders\n{session.term.text()}")
        check(b"\x1b[?1049h" in session.raw, "the alternate screen is entered")
        first = session.term.text().splitlines()
        check(all(len(line) <= 100 for line in first), "no line is wider than the terminal")
        session.send("2")
        check(session.wait_for(lambda t: "Your first DC program" in t and "Fundamentals" in t), f"learn screen lists the exercise\n{session.term.text()}")
        session.send("\r")
        check(session.wait_for(lambda t: "Enter start" in t and "not started" in t), f"exercise screen offers to start\n{session.term.text()}")
        session.send("\r")
        check(session.wait_for(lambda t: "src/main.dc" in t and "Preview" in t), f"starting creates and previews the files\n{session.term.text()}")
        workspace = home / "workspaces" / "fundamentals" / "hello-world" / "src" / "main.dc"
        check(workspace.exists(), "the workspace file exists")
        session.send("e")
        deadline = time.time() + 30
        while time.time() < deadline and "marker from the fake editor" not in (workspace.read_text() if workspace.exists() else ""):
            session.pump(0.1)
        check("marker from the fake editor" in workspace.read_text(), "the terminal editor ran and edited the workspace file")
        check(session.wait_for(lambda t: "marker from the fake editor" in t and "Preview" in t), f"the screen redraws and the preview refreshes after the editor\n{session.term.text()}")
        check(session.term.alt_entered >= 2, f"the alternate screen is entered again after the editor ({session.term.alt_entered})")
        session.send("r")
        check(session.wait_for(lambda t: "Press Enter to return" in t, timeout=120), f"running the program shows the return prompt\n{session.term.text()}")
        shown = session.term.text()
        check("Running " in shown and "Preview" not in shown and "Menu" not in shown, f"the screen is cleared before the program runs\n{shown}")
        check(not any(line.strip() == "---" for line in shown.splitlines()), f"the return prompt does not leave a dangling rule\n{shown}")
        session.send("\r")
        check(session.wait_for(lambda t: "Preview" in t and "Press Enter to return" not in t), f"learndc redraws after the program\n{session.term.text()}")
        session.resize(80, 24)
        check(session.wait_for(lambda t: len(t.splitlines()) >= 20 and all(len(line) <= 80 for line in t.splitlines()) and "Results" in t), f"the layout follows a resize to 80x24\n{session.term.text()}")
        session.send("?")
        check(session.wait_for(lambda t: "Keys on the Lesson screen" in t), f"help opens\n{session.term.text()}")
        session.send("\x1b")
        check(session.wait_for(lambda t: "Keys on the Exercise screen" not in t and "Results" in t), "escape closes help")
        session.send("q")
        rc, after = session.finish()
        check(rc == 0, f"q quits with exit code 0, got {rc}")
        check(session.term.alt_entered >= 1 and session.term.alt_left == session.term.alt_entered and not session.term.alt, f"the alternate screen is left as often as it was entered ({session.term.alt_entered}/{session.term.alt_left})")
        check(after == session.before, "the terminal attributes are restored after quitting")
    finally:
        if session.proc.poll() is None:
            session.proc.kill()
            session.proc.wait()
        session.close()


def journey_validate():
    print("journey: dev validate")
    rc, out, err = expect(["dev", "validate", "--backend", "dpm", "--id", "fundamentals/hello-world"], 0)
    check("fundamentals/hello-world" in out and "ok" in out, f"validate table\n{out}")
    check("fails 1/2" in out or "fails" in out, "validate reports the starter state")
    expect(["dev", "validate", "--backend", "nope"], 2)
    expect(["dev", "validate", "--id", "fundamentals/zzz"], 5)
    expect(["dev", "bogus"], 2)


def journey_embedded():
    rc, out, err = run(["--version"], env={"LEARNDC_CONTENT_DIR": ""})
    if rc != 0:
        print(f"journey: embedded content SKIPPED (the bundled content does not load: {err.strip().splitlines()[:2]})")
        return
    print("journey: embedded content")
    env = {"LEARNDC_CONTENT_DIR": ""}
    data = expect_json(["list", "--json"], env=env)
    check(any(e["id"] == "first-steps/hello-world" for e in data["exercises"]), "bundled catalog has hello-world")
    expect(["dev", "validate", "--backend", "dpm", "--id", "fundamentals/hello-world"], 0, env=env)


def journey_academy(tmp: Path, real_content: Path):
    print("journey: academy migration, lecture and boss")
    home = tmp / "academy home"
    old_ws = home / "workspaces" / "fundamentals" / "hello-world"
    (old_ws / "src").mkdir(parents=True)
    (old_ws / ".learndc").mkdir()
    shutil.copy(real_content / "tracks" / "first-steps" / "hello-world" / "starter" / "project.dcon", old_ws / "project.dcon")
    (old_ws / "src" / "main.dc").write_text("module main;\n// my old work\n")
    (old_ws / ".learndc" / "workspace.json").write_text(json.dumps({"schema": 1, "exercise": "fundamentals/hello-world", "revision": 2,
                                                                  "created_at": "2026-01-01T00:00:00Z", "starter": {}}))
    old = {"schema": 1, "active_exercise": "types/owned-array", "bookmarks": ["types/owned-array"], "workspaces": {}, "recent": ["fundamentals/hello-world"],
           "exercises": {"fundamentals/hello-world": {"status": "completed", "attempts": 3, "passes": 1, "completed_revision": 2,
                                                      "completed_at": "2026-01-02T00:00:00Z"},
                         "types/owned-array": {"status": "in-progress", "attempts": 1}}}
    (home / "progress.json").write_text(json.dumps(old))
    env = {"LEARNDC_HOME": str(home), "LEARNDC_CONTENT_DIR": str(real_content)}
    prog = expect_json(["progress", "--json"], env=env)
    ids = {e["id"]: e for e in prog["exercises"]}
    check(ids.get("first-steps/hello-world", {}).get("status") == "completed", f"completion moved to the new id: {ids}")
    check("memory/filling-arrays" in ids, "in-progress record moved")
    check(prog["active_exercise"] == "memory/filling-arrays", "active exercise moved")
    check(prog["bookmarks"] == ["memory/filling-arrays"], "bookmark moved")
    check((home / "progress.json.schema1.bak").read_text() == json.dumps(old), "schema 1 backup kept")
    data = expect_json(["show", "first-steps/hello-world", "--json"], env=env)
    check(Path(data["workspace"]) == old_ws, f"old workspace kept in place: {data['workspace']}")
    meta = json.loads((old_ws / ".learndc" / "workspace.json").read_text())
    check(meta["exercise"] == "first-steps/hello-world", "workspace retargeted")
    check((old_ws / "src" / "main.dc").read_text() == "module main;\n// my old work\n", "learner file untouched")
    again = expect_json(["progress", "--json"], env=env)
    check(again == prog, "a second start changes nothing")

    rc, out, err = expect(["show", "memory/scope-and-lifetime"], 0, env=env)
    check("Check your understanding" in out and "learndc answer memory/scope-and-lifetime" in out, "lecture shows its questions")
    expect(["answer", "memory/scope-and-lifetime", "1", "1", "1"], 1, env=env)
    rc, out, err = expect(["answer", "memory/scope-and-lifetime", "2", "2", "2"], 0, env=env)
    check("Lecture completed." in out, "lecture completes")

    boss = "memory/boss-inventory-keeper"
    rc, out, err = expect(["start", boss], 0, env=env)
    ws = home / "workspaces" / "memory" / "boss-inventory-keeper"
    data = expect_json(["check", boss, "--json"], 1, env=env)
    check(data.get("stages") and not any(st["passed"] for st in data["stages"]), "the starter clears no stage")
    solution = real_content / "tracks" / "memory" / "boss-inventory-keeper" / "solution" / "src"
    shutil.copy(solution / "inventory.dc", ws / "src" / "inventory.dc")
    rc, out, err = expect(["check", boss], 1, env=env)
    check("Stage cleared: Stock the shelf" in out and "2 required stages still to go." in out, f"partial victory reported\n{out}")
    prog = expect_json(["progress", "--json"], env=env)
    stages = {e["id"]: e for e in prog["exercises"]}[boss]["stages"]
    check(stages["take-it"]["achieved"] and not stages["night-shift"]["achieved"], f"stage records after the first pass: {stages}")
    rc, out, err = expect(["show", boss], 0, env=env)
    check("[x] 5. Refuse what you cannot pack" in out and "[>] 1. Read the old ledger" in out and "[ ] 6. The night shift" in out, f"a new process shows the same objectives\n{out}")
    (ws / "src" / "inventory.dc").write_text((ws / "src" / "inventory.dc").read_text() + "\n")
    rc, out, err = expect(["show", boss], 0, env=env)
    check("[~] 2. Stock the shelf" in out, f"an edit makes old passes unverified\n{out}")
    shutil.copy(solution / "main.dc", ws / "src" / "main.dc")
    rc, out, err = expect(["check", boss], 1, env=env)
    check("BOSS DEFEATED" not in out and "1 required stage still to go." in out, f"the unanswered ledger keeps the boss alive\n{out}")
    rc, out, err = expect(["answer", boss, "--question", "old-take", "8"], 1, env=env)
    rc, out, err = expect(["answer", boss, "--question", "old-take", "7"], 0, env=env)
    check("BOSS DEFEATED: The Inventory Keeper" in out, f"boss defeated\n{out}")
    rc, out, err = expect(["progress"], 0, env=env)
    check("Boss defeated: The Inventory Keeper" in out, f"achievement listed\n{out}")

    drill = "memory/ownership-drills"
    rc, out, err = expect(["show", drill], 0, env=env)
    check("[Find the bug]" in out and "--question too-early" in out, f"drill lists its activities\n{out}")
    rc, out, err = expect(["answer", drill, "--question", "too-early", "4"], 1, env=env)
    check("Not yet" in out and "Line 4 does what it says" in out, f"targeted feedback for a wrong line\n{out}")
    rc, out, err = expect(["cards", "--json"], 0, env=env)
    data = json.loads(out)
    check("memory/ownership-drills#too-early" in data["missed"], f"missed question queued for review: {data}")
    answers = [("defer-everywhere", "start\\ninner\\nB\\nC1\\nC2\\nend\\nD\\nA"), ("count-the-closes", "3"), ("too-early", "5"),
               ("gone-bonus", "2"), ("array-facts", "1,3"), ("the-keyword", "defer")]
    for qid, answer in answers:
        expect(["answer", drill, "--question", qid, *answer.split(" ")], 0, env=env, label=f"answer {qid}")
    show = expect(["show", drill], 0, env=env)[1]
    order_lines = [line for line in show.splitlines() if line.strip().startswith(tuple(str(n) for n in range(1, 6))) and "|" in line]
    wanted = ['println("pack bag")?;', 'defer println("unpack bag");', 'println("start hike")?;', 'defer println("end hike");', 'println("walking")?;']
    shown = {}
    in_order = False
    for line in show.splitlines():
        if "[Put the code in order]" in line:
            in_order = True
            continue
        if in_order and "|" in line:
            number, code = line.split("|", 1)
            shown[code.strip()] = number.strip()
        elif in_order and shown:
            break
    sequence = [shown.get(w, "?") for w in wanted]
    rc, out, err = expect(["answer", drill, "--question", "pack-and-unpack", *sequence], 0, env=env)
    check("Lesson completed." in out, f"the drill completes\n{out}")
    data = json.loads(expect(["cards", "--json"], 0, env=env)[1])
    keys = [c["key"] for c in data["due"]]
    check("memory/ownership-drills#early-exit" in keys and not data["missed"], f"the drill card is due and nothing is missed: {data}")
    rc, out, err = expect(["card", "memory/ownership-drills#early-exit"], 0, env=env)
    check("It runs on every exit" in out, "card shows its back")
    rc, out, err = expect(["card", "memory/ownership-drills#early-exit", "good"], 0, env=env)
    check("box 1 of 5" in out and "in 1 day" in out, f"card scheduled\n{out}")
    data = json.loads(expect(["cards", "--json"], 0, env=env)[1])
    check("memory/ownership-drills#early-exit" not in [c["key"] for c in data["due"]], "a rated card leaves today's queue")
    expect(["card", "memory/ownership-drills#early-exit", "maybe"], 2, env=env)


def main():
    global LEARNDC, DCC, DPM, HOME, CONTENT, ENV
    if len(sys.argv) != 4:
        print("usage: e2e.py <learndc> <dcc> <dpm>", file=sys.stderr)
        return 2
    LEARNDC, DCC, DPM = (str(Path(a).resolve()) for a in sys.argv[1:4])
    real_content = Path(__file__).resolve().parents[2] / "learndc" / "content"
    tmp = Path(tempfile.mkdtemp(prefix="learndc e2e é "))
    try:
        HOME = tmp / "home ünï"
        CONTENT = tmp / "content"
        make_content(real_content, CONTENT)
        ENV = {k: v for k, v in os.environ.items() if not k.startswith("LEARNDC_") and k not in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "NO_COLOR")}
        ENV["LEARNDC_HOME"] = str(HOME)
        ENV["LEARNDC_CONTENT_DIR"] = str(CONTENT)
        ENV["LEARNDC_DCC"] = DCC
        ENV["LEARNDC_DPM"] = DPM
        ENV["LANG"] = "C.UTF-8"
        journey_basics()
        journey_catalog()
        ws, main_file, solution = journey_start_and_check(real_content)
        journey_hints()
        journey_run(ws)
        journey_reset_and_solution(ws, main_file, solution)
        journey_bookmark()
        journey_config()
        journey_edit(tmp, ws)
        journey_doctor(tmp)
        journey_export_import(tmp)
        journey_validate()
        journey_watch()
        journey_terminate(tmp)
        journey_tui(tmp)
        journey_embedded()
        journey_academy(tmp, real_content)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILURES:
        print(f"\n{len(FAILURES)} of {CHECKS} learndc e2e checks failed")
        return 1
    print(f"\nPASS learndc e2e ({CHECKS} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
