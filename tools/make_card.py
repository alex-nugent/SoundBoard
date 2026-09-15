#!/usr/bin/env python3
"""Verify a SoundBoard card folder (FirmwareSpec §20.2).

  python3 tools/make_card.py <card folder> [--table lib/sbcore/src/config/settings_table.cpp]

Checks, and reports every problem it finds:
  - config.json parses; every scalar setting is a known path with a value inside the
    firmware's own range table (read from settings_table.cpp, so the tool and the firmware
    never disagree); enums use a listed name; strings fit their field.
  - levels: 1..MAX_LEVELS, each with 1..3 buttons; a button has a sound, an action, a key or
    typed text; sounds it names exist in /sounds; labels fit (24), names fit (16).
  - cues named in audio.cues exist.
  - every WAV in /sounds is canonical (mono, 44.1 kHz, 16-bit PCM) and not empty.
Exit status 0 when the card is clean, 1 otherwise. The card is then applied through the
settings page (Backup upload + Sounds upload) or copied to a bench unit's card directly.
"""
import argparse, json, os, re, sys, wave

ACTIONS = {"none", "volumeUp", "volumeDown", "mute", "nextLevel", "previousLevel", "goToLevel", "vibrate"}
KEYMODES = {"type", "hold", "tap"}
JACKS = {"follow", "pulse", "off", "none"}
MAX_LEVELS, MAX_SOUND_PADS = 12, 3
STRUCTURED = {"device.ownerLabel", "hardware.padChannels", "pads.roles", "touch.padPressPct", "audio.cues",
              "vibration.pattern", "levels", "schema", "revision"}


def parse_table(path):
    """settings_table.cpp rows → {path: {type, min, max, default, enums}}."""
    rows = {}
    row_re = re.compile(r'^\s*\{\s*"([^"]+)",\s*SType::(\w+),\s*FIELD\([^)]*\),\s*([-\d.]+)f?,\s*([-\d.]+)f?,\s*[-\d.]+f?,\s*"([^"]*)",\s*(nullptr|"[^"]*")')
    for line in open(path, encoding="utf-8"):
        m = row_re.match(line)
        if m:
            p, t, lo, hi, default, enums = m.groups()
            rows[p] = {"type": t, "min": float(lo), "max": float(hi), "default": default,
                       "enums": None if enums == "nullptr" else enums.strip('"').split("|")}
    return rows


def flatten(d, prefix=""):
    for k, v in d.items():
        p = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict) and p not in STRUCTURED:
            yield from flatten(v, p)
        else:
            yield p, v


def check_scalar(path, v, row, err):
    t = row["type"]
    if t == "Bool":
        if not isinstance(v, bool): err(f"{path}: expected true/false, got {v!r}")
    elif t in ("U8", "U16", "U32"):
        if isinstance(v, bool) or not isinstance(v, int): err(f"{path}: expected a whole number, got {v!r}")
        elif not row["min"] <= v <= row["max"]: err(f"{path}: {v} is outside {int(row['min'])}..{int(row['max'])}")
    elif t == "F32":
        if isinstance(v, bool) or not isinstance(v, (int, float)): err(f"{path}: expected a number, got {v!r}")
        elif not row["min"] <= v <= row["max"]: err(f"{path}: {v} is outside {row['min']}..{row['max']}")
    elif t == "Enum":
        if v not in (row["enums"] or []): err(f"{path}: {v!r} is not one of {'|'.join(row['enums'] or [])}")
    elif t == "String":
        if not isinstance(v, str): err(f"{path}: expected text, got {v!r}")
        elif not row["min"] <= len(v) <= row["max"]: err(f"{path}: {len(v)} characters, allowed {int(row['min'])}..{int(row['max'])}")


def check_wav(path):
    try:
        with wave.open(path, "rb") as w:
            ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    except Exception as e:
        return f"not a readable WAV ({e})"
    if (ch, sw, sr) != (1, 2, 44100):
        return f"not canonical: {ch} ch, {sw * 8}-bit, {sr} Hz"
    if n == 0:
        return "empty"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("card")
    ap.add_argument("--table", default=os.path.join(os.path.dirname(__file__), "..", "lib", "sbcore", "src", "config", "settings_table.cpp"))
    a = ap.parse_args()
    problems = []
    err = lambda m: problems.append(m)
    table = parse_table(a.table)
    if len(table) < 50:
        sys.exit(f"could not read the settings table at {a.table} ({len(table)} rows)")
    sounds_dir = os.path.join(a.card, "sounds")
    present = set()
    if not os.path.isdir(sounds_dir):
        err("no /sounds folder")
    else:
        for f in sorted(os.listdir(sounds_dir)):
            if f.startswith("."): continue
            if not f.lower().endswith(".wav"):
                err(f"sounds/{f}: not a .wav (the board plays WAV only)"); continue
            bad = check_wav(os.path.join(sounds_dir, f))
            if bad: err(f"sounds/{f}: {bad}")
            else: present.add(f)
    cfg_path = os.path.join(a.card, "config.json")
    try:
        cfg = json.load(open(cfg_path, encoding="utf-8"))
    except Exception as e:
        err(f"config.json: {e}"); cfg = {}
    if cfg:
        if cfg.get("schema") != 1: err(f"schema: expected 1, got {cfg.get('schema')!r}")
        for path, v in flatten(cfg):
            if path in STRUCTURED: continue
            if path not in table: err(f"{path}: not a setting the firmware knows"); continue
            check_scalar(path, v, table[path], err)
        for name, cue in (cfg.get("audio", {}).get("cues", {}) or {}).items():
            if cue and cue not in present: err(f"audio.cues.{name}: {cue} is not in /sounds")
        levels = cfg.get("levels", [])
        if not isinstance(levels, list) or not 1 <= len(levels) <= MAX_LEVELS:
            err(f"levels: expected 1..{MAX_LEVELS} levels")
            levels = levels if isinstance(levels, list) else []
        for i, lv in enumerate(levels, 1):
            if not isinstance(lv, dict): err(f"level {i}: not an object"); continue
            if len(lv.get("name", "")) > 16: err(f"level {i}: name longer than 16")
            buttons = lv.get("buttons", [])
            if not 1 <= len(buttons) <= MAX_SOUND_PADS: err(f"level {i}: expected 1..{MAX_SOUND_PADS} buttons")
            for j, b in enumerate(buttons, 1):
                who = f"level {i} button {j}"
                if not isinstance(b, dict): err(f"{who}: not an object"); continue
                snd = b.get("sound", "")
                if snd and snd not in present: err(f"{who}: {snd} is not in /sounds")
                if len(b.get("label", "")) > 24: err(f"{who}: label longer than 24")
                if len(b.get("type", "")) > 64: err(f"{who}: typed text longer than 64")
                if b.get("action", "none") not in ACTIONS: err(f"{who}: unknown action {b.get('action')!r}")
                if b.get("action") == "goToLevel" and not 1 <= int(b.get("level", 0) or b.get("goToLevel", 0) or 0) <= len(levels):
                    err(f"{who}: goToLevel needs a level number in 1..{len(levels)}")
                if b.get("keyMode", "type") not in KEYMODES: err(f"{who}: unknown keyMode {b.get('keyMode')!r}")
                if b.get("jack", "follow") not in JACKS: err(f"{who}: unknown jack mode {b.get('jack')!r}")
                if not (snd or b.get("action", "none") != "none" or b.get("key") or b.get("type")):
                    err(f"{who}: nothing to do (no sound, action, key or text)")
    for p in problems:
        print("PROBLEM:", p)
    print(f"{a.card}: {len(present)} sound(s), {len(cfg.get('levels', [])) if cfg else 0} level(s), {len(problems)} problem(s)")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
