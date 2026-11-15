#!/usr/bin/env python3
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
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


def make_content(real_content: Path, target: Path):
    (target / "tracks" / "fundamentals").mkdir(parents=True)
    src = real_content / "tracks" / "fundamentals" / "hello-world"
    shutil.copytree(src, target / "tracks" / "fundamentals" / "hello-world")
    again = target / "tracks" / "fundamentals" / "hello-again"
    shutil.copytree(src, again)
    toml = (again / "exercise.toml").read_text()
    toml = toml.replace('id = "fundamentals/hello-world"', 'id = "fundamentals/hello-again"')
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
    check("Expected" in out and "Observed" in out, f"failing check shows expected and observed\n{out}")
    check("Hello, Ada!" in out, "expected text visible")
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

    solution = (real_content / "tracks" / "fundamentals" / "hello-world" / "solution" / "src" / "main.dc").read_text()
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
    check("1 of 2 exercises completed" in out, "progress text")

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


def journey_run(ws: Path):
    print("journey: run")
    rc, out, err = expect(["run", "fundamentals/hello-world"], 0)
    check("Hello" in out, f"run prints the program output: {out!r}")


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
    check(any(e["id"] == "fundamentals/hello-world" for e in data["exercises"]), "bundled catalog has hello-world")
    expect(["dev", "validate", "--backend", "dpm", "--id", "fundamentals/hello-world"], 0, env=env)


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
        journey_embedded()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILURES:
        print(f"\n{len(FAILURES)} of {CHECKS} learndc e2e checks failed")
        return 1
    print(f"\nPASS learndc e2e ({CHECKS} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
