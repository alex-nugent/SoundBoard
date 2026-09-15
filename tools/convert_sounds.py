#!/usr/bin/env python3
"""Convert a folder of recordings into a SoundBoard card folder (FirmwareSpec §20.1).

  python3 tools/convert_sounds.py <source folder> <card folder> [options]

Every audio file ffmpeg can read becomes /sounds/<name>.wav in the canonical
format: mono, 44.1 kHz, 16-bit PCM, peak normalised to -1 dBFS, silence below
-50 dBFS trimmed at the two edges only (20 ms of it kept at each edge). The
browser converter on the settings page follows the same rule.

Options:
  --config <V3 config.txt>   one-time import of the V3 levels into config.json
  --skip-level N             leave V3 level N out (may repeat)
  --volume-level N           insert the VOLUME level (Quieter, Mute, Louder) at position N
  --template <file>          the config template (default tools/config.template.jsonc)
  --loudness                 loudnorm two-pass (I=-16 LUFS, TP=-1.5 dB) instead of peak normalisation
  --no-trim                  keep the edges as recorded
  --self-test                convert two synthetic files and check the result, then exit

Needs ffmpeg and ffprobe on the PATH (ffmpeg 8 tested).
"""
import argparse, json, os, re, shutil, struct, subprocess, sys, tempfile, wave

AUDIO_EXT = {".ogg", ".oga", ".wav", ".mp3", ".m4a", ".aac", ".flac", ".opus", ".wma", ".aiff", ".aif", ".caf", ".mp4", ".webm"}
CUE_MAP = {"_startup": "_startup", "_click": "_click", "_onsave": "_saved"}   # V3 cue files → V4 names
TRIM = ("silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.02,areverse,"
        "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.02,areverse")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def need_tools():
    for t in ("ffmpeg", "ffprobe"):
        if not shutil.which(t):
            sys.exit(f"{t} is not on the PATH")


def sanitise(stem):
    """Lower-case, spaces and odd characters to '_', the V3 cue names mapped; at most 36 characters."""
    stem = CUE_MAP.get(stem.lower(), stem)
    s = re.sub(r"[^a-z0-9_-]+", "_", stem.lower()).strip("_")
    if stem.startswith("_"):
        s = "_" + s.lstrip("_")
    return (s or "sound")[:36]


def peak_db(path):
    r = run(["ffmpeg", "-i", path, "-af", "volumedetect", "-f", "null", "-"])
    m = re.search(r"max_volume:\s*(-?[\d.]+) dB", r.stderr)
    return float(m.group(1)) if m else None


def loudnorm_measure(path):
    r = run(["ffmpeg", "-i", path, "-af", "loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"])
    m = re.search(r"\{.*\}", r.stderr, re.S)
    return json.loads(m.group(0)) if m else None


def convert(src, dst, loudness=False, trim=True):
    """One file → canonical WAV. Returns (ok, note)."""
    filters, silent = [], False
    if loudness:
        m = loudnorm_measure(src)
        if not m:
            return False, "loudnorm measurement failed"
        filters.append("loudnorm=I=-16:TP=-1.5:LRA=11:measured_I=%s:measured_TP=%s:measured_LRA=%s:measured_thresh=%s:offset=%s:linear=true"
                       % (m["input_i"], m["input_tp"], m["input_lra"], m["input_thresh"], m["target_offset"]))
    else:
        peak = peak_db(src)
        if peak is None:
            return False, "could not measure the peak"
        silent = peak <= -90                             # an all-silent file measures -91 dB
        if not silent:
            filters.append("volume=%.2fdB" % (-1.0 - peak))
    if trim and not silent:                              # trimming a silent file would leave nothing: keep 40 ms of it
        filters.append(TRIM)
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", src]
    if silent:
        cmd += ["-t", "0.04"]
    if filters:
        cmd += ["-af", ",".join(filters)]
    cmd += ["-ac", "1", "-ar", "44100", "-sample_fmt", "s16", "-c:a", "pcm_s16le", "-fflags", "+bitexact", "-map_metadata", "-1", dst]
    r = run(cmd)
    if r.returncode != 0:
        return False, r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "ffmpeg failed"
    ok, note = check_wav(dst)
    return ok, note


def check_wav(path):
    """Canonical? mono, 44.1 kHz, 16-bit PCM, some samples. Returns (ok, description)."""
    try:
        with wave.open(path, "rb") as w:
            ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    except Exception as e:
        return False, f"not a readable WAV ({e})"
    if (ch, sw, sr) != (1, 2, 44100):
        return False, f"not canonical: {ch} ch, {sw * 8}-bit, {sr} Hz"
    if n == 0:
        return False, "empty"
    return True, "%.2f s" % (n / sr)


def wav_seconds(path):
    with wave.open(path, "rb") as w:
        return w.getnframes() / w.getframerate()


# --- V3 config import -----------------------------------------------------------------------------
def parse_v3(path):
    """`mode=NAME` blocks of `file,label,typed` lines → [{name, entries:[(file, label, typed)]}]."""
    levels, cur = [], None
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.strip()
        if not line:
            continue
        if line.lower().startswith("mode="):
            cur = {"name": line[5:].strip(), "entries": []}
            levels.append(cur)
            continue
        if cur is None:
            continue
        parts = [p.strip() for p in line.split(",")]
        while len(parts) < 3:
            parts.append("")
        cur["entries"].append((parts[0], parts[1], ",".join(parts[2:])))
    return levels


def v3_to_levels(v3, sounds_present, skip, volume_at, warn):
    out = []
    for i, lv in enumerate(v3, 1):
        if i in skip:
            continue
        buttons = []
        for f, label, typed in lv["entries"][:3]:
            stem = sanitise(os.path.splitext(f)[0])
            name = stem + ".wav"
            if name not in sounds_present:
                warn(f"level {lv['name']}: {f} → {name} is missing from /sounds")
            b = {"sound": name, "label": label or stem}
            if typed:
                b["type"] = typed
            buttons.append(b)
        out.append({"name": lv["name"], "buttons": buttons})
    if volume_at:
        vol = {"name": "VOLUME", "buttons": [{"action": "volumeDown", "label": "Quieter"},
                                             {"action": "mute", "label": "Mute"},
                                             {"action": "volumeUp", "label": "Louder"}]}
        out.insert(max(0, min(len(out), volume_at - 1)), vol)
    return out


def strip_jsonc(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(^|[^:\"'])//[^\n]*", r"\1", text)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return text


# --- self-test ------------------------------------------------------------------------------------
def make_tone_file(path, spec, sr=44100):
    """spec: list of (seconds, frequency or 0 for silence) → 16-bit mono WAV at -6 dBFS."""
    import math
    frames = bytearray()
    for secs, hz in spec:
        n = int(secs * sr)
        for i in range(n):
            v = int(16000 * math.sin(2 * math.pi * hz * i / sr)) if hz else 0
            frames += struct.pack("<h", v)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(bytes(frames))


def self_test():
    need_tools()
    d = tempfile.mkdtemp(prefix="sb-convert-")
    two = os.path.join(d, "two.wav"); silent = os.path.join(d, "silent.wav")
    make_tone_file(two, [(0.5, 0), (0.3, 440), (0.3, 0), (0.3, 880), (0.5, 0)])   # silence, tone, 300 ms gap, tone, silence
    make_tone_file(silent, [(1.0, 0)])
    fails = []
    ok, note = convert(two, os.path.join(d, "two.out.wav"))
    secs = wav_seconds(os.path.join(d, "two.out.wav")) if ok else 0
    # both tones and the 300 ms gap survive (0.9 s) plus 20 ms kept at each edge, within ffmpeg's window slop
    if not ok or not (0.90 <= secs <= 1.02):
        fails.append(f"two tones: {note}, {secs:.3f} s (expected ~0.94 s: both tones and the gap kept)")
    peak = peak_db(os.path.join(d, "two.out.wav")) if ok else None
    if peak is None or not (-1.3 <= peak <= -0.7):
        fails.append(f"two tones: peak {peak} dB, expected -1 dB")
    ok, note = convert(silent, os.path.join(d, "silent.out.wav"))
    secs = wav_seconds(os.path.join(d, "silent.out.wav")) if ok else 0
    if not ok or not (0.02 <= secs <= 0.06):
        fails.append(f"silent: {note}, {secs:.3f} s (expected ~0.04 s, not an error)")
    shutil.rmtree(d, ignore_errors=True)
    for f in fails:
        print("FAIL", f)
    print("self-test", "FAILED" if fails else "PASSED")
    return not fails


# --- main -----------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?"); ap.add_argument("out", nargs="?")
    ap.add_argument("--config"); ap.add_argument("--skip-level", type=int, action="append", default=[])
    ap.add_argument("--volume-level", type=int, default=0)
    ap.add_argument("--template", default=os.path.join(os.path.dirname(__file__), "config.template.jsonc"))
    ap.add_argument("--loudness", action="store_true"); ap.add_argument("--no-trim", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(0 if self_test() else 1)
    if not a.src or not a.out:
        ap.error("source and card folders are required")
    need_tools()
    sounds_dir = os.path.join(a.out, "sounds"); os.makedirs(sounds_dir, exist_ok=True)
    warnings = []
    warn = lambda m: (warnings.append(m), print("WARNING:", m))
    present = set()
    for f in sorted(os.listdir(a.src)):
        stem, ext = os.path.splitext(f)
        if ext.lower() not in AUDIO_EXT or f.startswith("."):
            continue
        name = sanitise(stem) + ".wav"
        if name in present:
            warn(f"{f}: name clash on {name}, skipped")
            continue
        ok, note = convert(os.path.join(a.src, f), os.path.join(sounds_dir, name), a.loudness, not a.no_trim)
        print(f"{'ok ' if ok else 'ERR'} {f:32s} → sounds/{name:28s} {note}")
        if ok:
            present.add(name)
        else:
            try: os.remove(os.path.join(sounds_dir, name))
            except OSError: pass
    cfg = json.loads(strip_jsonc(open(a.template, encoding="utf-8").read()))
    if a.config:
        cfg["levels"] = v3_to_levels(parse_v3(a.config), present, set(a.skip_level), a.volume_level, warn)
    for cue in cfg.get("audio", {}).get("cues", {}).values():
        if cue and cue not in present:
            warn(f"cue {cue} is missing from /sounds")
    with open(os.path.join(a.out, "config.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False); f.write("\n")
    print(f"\n{len(present)} sound(s) in {sounds_dir}; config.json with {len(cfg.get('levels', []))} level(s); {len(warnings)} warning(s)")
    sys.exit(1 if warnings else 0)


if __name__ == "__main__":
    main()
