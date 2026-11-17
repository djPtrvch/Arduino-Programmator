#!/usr/bin/env python3
"""Сборка всех прошивок, моста и веб-интерфейса в папку dist/ (для GitHub Pages).

    python3 tools/build.py              # всё
    python3 tools/build.py --no-bridge  # без моста ESP32 (быстрее)
    python3 tools/build.py --webui-only # только перегенерировать bridge/web_ui.h

Нужен arduino-cli в PATH.
"""
import argparse
import datetime
import glob
import gzip
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
ESP32_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
BRIDGE_FQBN = "esp32:esp32:esp32"


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def cli(*args):
    cmd = ["arduino-cli", *args, "--additional-urls", ESP32_INDEX]
    print("$", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def render_html(boards):
    with open(os.path.join(ROOT, "web", "index.html"), encoding="utf-8") as f:
        html = f.read()
    marker = "/*@BOARDS@*/null"
    if marker not in html:
        sys.exit("web/index.html: не найден маркер " + marker)
    return html.replace(marker, json.dumps(boards, ensure_ascii=False))


def write_webui_header(html):
    data = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)
    lines = []
    for i in range(0, len(data), 20):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    header = (
        "// Сгенерировано tools/build.py из web/index.html — не редактировать вручную.\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        f"const size_t WEB_UI_GZ_LEN = {len(data)};\n"
        "const uint8_t WEB_UI_GZ[] PROGMEM = {\n" + "\n".join(lines) + "\n};\n"
    )
    with open(os.path.join(ROOT, "bridge", "web_ui.h"), "w", encoding="utf-8") as f:
        f.write(header)


def compile_sketch(sketch_dir, fqbn):
    out = tempfile.mkdtemp(prefix="build-")
    cli("compile", "--fqbn", fqbn, "--output-dir", out, "--warnings", "default", sketch_dir)
    return out


def pick_output(out_dir, board):
    if board["fqbn"].startswith("esp32:"):
        files = [f for f in glob.glob(os.path.join(out_dir, "*.ino.bin"))]
        return files[0] if files else None
    if board.get("withBootloader"):
        files = glob.glob(os.path.join(out_dir, "*.ino.with_bootloader.hex"))
        if files:
            return files[0]
    files = [f for f in glob.glob(os.path.join(out_dir, "*.ino.hex")) if "with_bootloader" not in f]
    return files[0] if files else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-bridge", action="store_true")
    ap.add_argument("--webui-only", action="store_true")
    args = ap.parse_args()

    boards = load_json(os.path.join(ROOT, "boards.json"))
    html = render_html(boards)
    write_webui_header(html)
    if args.webui_only:
        return

    projects = []
    for pj in sorted(glob.glob(os.path.join(ROOT, "firmware", "*", "project.json"))):
        meta = load_json(pj)
        meta["id"] = os.path.basename(os.path.dirname(pj))
        meta["dir"] = os.path.dirname(pj)
        for b in meta["boards"]:
            if b not in boards:
                sys.exit(f"{pj}: неизвестная плата '{b}' (нет в boards.json)")
        projects.append(meta)

    cores = {boards[b]["core"] for p in projects for b in p["boards"]}
    if not args.no_bridge:
        cores.add("esp32:esp32")
    libs = sorted({lib for p in projects for lib in p.get("libraries", [])})

    cli("core", "update-index")
    for core in sorted(cores):
        cli("core", "install", core)
    if libs:
        cli("lib", "install", *libs)

    shutil.rmtree(DIST, ignore_errors=True)
    os.makedirs(DIST)
    failed = []
    manifest = {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "commit": os.environ.get("GITHUB_SHA", "local"),
        "boards": boards,
        "projects": [],
    }

    for p in projects:
        entry = {"id": p["id"], "name": p["name"], "description": p.get("description", ""), "builds": {}}
        for b in p["boards"]:
            board = boards[b]
            try:
                out = compile_sketch(p["dir"], board["fqbn"])
                src = pick_output(out, board)
                if not src:
                    raise RuntimeError("не найден файл прошивки в " + out)
                ext = os.path.splitext(src)[1]
                rel = f"firmware/{p['id']}/{b}{ext}"
                os.makedirs(os.path.join(DIST, os.path.dirname(rel)), exist_ok=True)
                shutil.copy(src, os.path.join(DIST, rel))
                entry["builds"][b] = {"file": rel, "size": os.path.getsize(src)}
            except Exception as e:  # noqa: BLE001
                print(f"!! {p['id']} / {b}: {e}", flush=True)
                failed.append(f"{p['id']}/{b}")
        manifest["projects"].append(entry)

    if not args.no_bridge:
        try:
            out = compile_sketch(os.path.join(ROOT, "bridge"), BRIDGE_FQBN)
            src = glob.glob(os.path.join(out, "bridge.ino.bin"))[0]
            os.makedirs(os.path.join(DIST, "bridge"))
            shutil.copy(src, os.path.join(DIST, "bridge", "bridge.bin"))
            merged = glob.glob(os.path.join(out, "bridge.ino.merged.bin"))
            if merged:  # полный образ для первой прошивки с компьютера (адрес 0x0)
                shutil.copy(merged[0], os.path.join(DIST, "bridge", "bridge-full.bin"))
            manifest["bridge"] = {"file": "bridge/bridge.bin"}
        except Exception as e:  # noqa: BLE001
            print(f"!! bridge: {e}", flush=True)
            failed.append("bridge")

    with open(os.path.join(DIST, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    with open(os.path.join(DIST, "index.html"), "w", encoding="utf-8") as f:
        f.write(html)
    open(os.path.join(DIST, ".nojekyll"), "w").close()

    if failed:
        sys.exit("Не собрались: " + ", ".join(failed))
    print("OK →", DIST)


if __name__ == "__main__":
    main()
