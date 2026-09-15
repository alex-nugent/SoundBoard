#!/usr/bin/env python3
# A desktop stand-in for the board (FirmwareSpec.md §15.3: "the page is
# developed in a desktop browser against a mock /api/* JSON"). Serves
# data/portal/ unbuilt and answers every endpoint of §15.4 (Phase 10) from
# memory, starting from examples/config.example.json.
#
#   python3 tools/mock_portal.py [port]     then open http://localhost:8080/
import json, os, re, sys, time, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PORTAL = os.path.join(ROOT, "data", "portal")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

# The descriptor table, scraped from settings_table.cpp so the mock's schema is the board's.
def load_schema():
    src = open(os.path.join(ROOT, "lib", "sbcore", "src", "config", "settings_table.cpp")).read()
    rows = []
    for m in re.finditer(r'\{\s*"([^"]+)",\s*SType::(\w+),\s*FIELD\(([^)]+)\),\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*"([^"]*)",\s*(nullptr|"[^"]*"),\s*(nullptr|"[^"]*"),\s*"([^"]*)",\s*"([^"]*)",\s*([A-Z_| ]+?),\s*(\d+)\s*\}', src):
        path, t, _f, mn, mx, st, d, en, ch, label, group, flags, order = m.groups()
        f = set(x.strip() for x in flags.split("|"))
        fl = {"P": {"PORTAL"}, "PL": {"PORTAL", "LIVE"}, "PLA": {"PORTAL", "LIVE", "ADVANCED"}, "PA": {"PORTAL", "ADVANCED"}, "PLM": {"PORTAL", "LIVE", "MENU"}}
        ff = set()
        for x in f: ff |= fl.get(x, {x})
        num = lambda s: float(s.rstrip("f"))
        row = {"path": path, "type": t.lower(), "label": label, "group": group, "min": num(mn), "max": num(mx), "step": num(st), "def": d,
               "live": "LIVE" in ff, "advanced": "ADVANCED" in ff, "reboot": "REBOOT" in ff, "zeroOff": "ZERO_OFF" in ff, "menu": "MENU" in ff}
        if en != "nullptr": row["enums"] = en.strip('"').split("|")
        if ch != "nullptr": row["choices"] = ch.strip('"')
        if t == "String": row["maxLen"] = int(num(mx))
        rows.append(row)
    return rows

SCHEMA = {"version": "mock-1234abc", "settings": load_schema(),
          "keys": "SPACE ENTER TAB ESC BKSP UP DOWN LEFT RIGHT 0 1 2 3 4 5 6 7 8 9 A B C D E F G H I J K L M N O P Q R S T U V W X Y Z F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12 NEXT PREV STOP PLAY MUTE VOL+ VOL- HOME".split(),
          "actions": ["none", "volumeUp", "volumeDown", "mute", "nextLevel", "previousLevel", "goToLevel"], "keyModes": ["type", "hold", "tap"],
          "jackModes": ["follow", "pulse", "off"], "roles": ["sound", "level", "none"],
          "limits": {"maxLevels": 20, "maxSoundPads": 4, "maxPattern": 16, "levelName": 16, "label": 24, "typed": 64, "soundName": 40, "ownerLine": 20, "ownerLines": 3, "soundUpload": 8388608, "recordingS": 30}}

def strip_comments(s):
    s = re.sub(r"//[^\n]*", "", s)
    return re.sub(r"/\*.*?\*/", "", s, flags=re.S)

CFG = json.loads(strip_comments(open(os.path.join(ROOT, "examples", "config.example.json")).read()))
CFG.setdefault("revision", 26)
CFG.setdefault("setup", {"password": "soundboard", "idleOffMin": 10, "pauseKeyboard": False})
CFG.setdefault("wifi", {"ssid": "", "password": ""})
CFG.setdefault("bluetoothSpeaker", {"enabled": False})
CFG.setdefault("keyboard", {"enabled": True})
SOUNDS = [{"name": n, "bytes": 88200 * k, "rate": 44100, "channels": 1, "seconds": round(k, 2), "state": "cached", "reason": "", "usedBy": []}
          for n, k in [("yes.wav", 0.6), ("no.wav", 0.5), ("maybe.wav", 0.8), ("_click.wav", 0.05), ("_saved.wav", 0.3), ("hello.wav", 1.2), ("music.wav", 12.4)]]
SOUNDS.append({"name": "broken.wav", "bytes": 1000, "rate": 0, "channels": 0, "seconds": 0, "state": "bad", "reason": "NotPcm", "usedBy": []})
FW = {"job": "idle", "pct": 0, "error": "", "text": "", "available": None, "sta": False, "t0": 0, "kind": ""}
STATE = {"level": 1, "volume": 60, "muted": False, "pairing": "none", "pairUntil": 0, "padSeq": 0, "padCh": 0, "padPos": 0, "recovery": "--recovery" in sys.argv, "lastInput": time.time(), "started": time.time()}
LOCK = threading.Lock()

def used_by():
    for s in SOUNDS: s["usedBy"] = []
    roles = CFG.get("pads", {}).get("roles", ["level", "sound", "sound", "sound"])
    pos = [i for i, r in enumerate(roles) if r == "sound"]
    for li, l in enumerate(CFG.get("levels", [])):
        for bi, b in enumerate(l.get("buttons", [])):
            for s in SOUNDS:
                if b.get("sound", "").lower() == s["name"].lower() and bi < len(pos): s["usedBy"].append("L%d P%d" % (li + 1, pos[bi] + 1))
    for k, v in CFG.get("audio", {}).get("cues", {}).items():
        for s in SOUNDS:
            if v and v.lower() == s["name"].lower(): s["usedBy"].append("cue " + k)

def merge(t, p):
    for k, v in p.items():
        if v is None: t.pop(k, None)
        elif isinstance(v, dict) and isinstance(t.get(k), dict): merge(t[k], v)
        else: t[k] = v

def status():
    now = time.time()
    if STATE["pairing"] in ("searching",) and now > STATE["pairUntil"]: STATE["pairing"] = "none"
    lv = CFG["levels"][STATE["level"] - 1] if CFG.get("levels") else {}
    return {"mode": "ACTIVE", "version": SCHEMA["version"], "uptimeS": int(now - STATE["started"]) + 4000, "reset": "power-on / EN pin", "name": CFG["device"]["name"],
            "battery": {"pct": 82, "mv": 3987, "usb": True, "valid": True, "full": False},
            "level": {"index": STATE["level"], "count": len(CFG.get("levels", [])), "name": lv.get("name", "")},
            "volume": {"pct": STATE["volume"], "muted": STATE["muted"]},
            "card": {"mounted": True, "info": "16 GB SDHC", "files": len(SOUNDS), "source": "card", "revision": CFG["revision"], "lastSaveOk": True, "cardLess": False, "schemaTooNew": False, "parseError": ""},
            "bt": {"enabled": CFG["bluetoothSpeaker"]["enabled"], "linked": CFG["bluetoothSpeaker"]["enabled"] and STATE["pairing"] == "none", "peer": "ECOXGEAR", "module": "alive", "pairing": STATE["pairing"], "pairLeftS": max(0, int(STATE["pairUntil"] - now))},
            "keyboard": {"enabled": CFG["keyboard"]["enabled"], "paused": False, "link": "advertising", "bonds": 1, "initOk": True},
            "speakers": CFG["audio"]["outputs"]["speakers"], "playing": False,
            "faults": [] if STATE["recovery"] else [{"token": "!BT", "text": "Bluetooth speaker module not answering"}] if not CFG["bluetoothSpeaker"]["enabled"] and False else [],
            "warnings": [], "setup": {"clients": 1, "recovery": STATE["recovery"], "ssid": "SoundBoard-1A2B", "ip": "192.168.4.1", "idleS": int(now - STATE["lastInput"]), "idleOffMin": CFG["setup"]["idleOffMin"], "requests": 12, "sinceS": int(now - STATE["started"])},
            "pad": {"seq": STATE["padSeq"], "ch": STATE["padCh"], "pos": STATE["padPos"]}, "heap": 150 * 1024, "psram": 6 * 1024 * 1024, "calibrating": False}

def diag():
    import random
    ch = CFG["hardware"]["padChannels"]
    pads = [{"pos": i + 1, "ch": ch[i], "delta": round(random.uniform(-0.3, 0.3), 2), "raw": 25000 + i * 100, "baseline": 25000 + i * 100, "pressed": False, "stuck": False, "role": CFG["pads"]["roles"][i]} for i in range(4)]
    return {"pads": pads, "touch": "ready", "cache": {"cached": 7, "count": 8, "usedKB": 1300, "budgetKB": 6144, "loader": False},
            "audio": {"rail": "up", "ready": True, "playing": False, "underruns": 0, "stalls": 3, "starved": 0},
            "kcx": {"last": "OK+STATUS:1", "module": "alive", "link": "linked", "connects": 2},
            "battery": {"method": "pulldown", "k": 6.395, "tcal": 31.2, "mv": 3987, "pct": 82, "valid": True, "usb": True},
            "reset": "power-on / EN pin", "bootCount": 41, "crashes": 0, "heap": 150 * 1024, "heapMin": 120 * 1024, "psram": 6 * 1024 * 1024, "appStackMin": 6000, "netStackMin": 5000,
            "uptimeS": 4000, "pad": {"seq": STATE["padSeq"], "ch": STATE["padCh"], "pos": STATE["padPos"]}, "padChannels": ch}

class H(BaseHTTPRequestHandler):
    def log_message(self, f, *a): sys.stderr.write("%s %s\n" % (self.command, self.path))
    def send(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        elif isinstance(body, str): body = body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def args(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        ct = self.headers.get("Content-Type", "")
        if "json" in ct: return body.decode(), {}
        if "multipart" in ct:
            m = re.search(rb'filename="([^"]*)"', body); name = m.group(1).decode() if m else ""
            fb = re.search(rb"\r\n\r\n(.*?)\r\n--", body, re.S)
            fields = {k: v[0] for k, v in parse_qs(b"".join(re.findall(rb'name="(\w+)"\r\n\r\n([^\r]*)', body)).decode(errors="ignore")).items()} if False else {}
            for fm in re.finditer(rb'name="(\w+)"\r\n\r\n([^\r]*)\r\n', body): fields[fm.group(1).decode()] = fm.group(2).decode(errors="ignore")
            return (fb.group(1) if fb else b""), dict(fields, filename=name)
        return body.decode(), {k: v[0] for k, v in parse_qs(body.decode()).items()}
    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        with LOCK:
            if u.path in ("/", "/index.html"): return self.send(200, open(os.path.join(PORTAL, "index.html"), "rb").read(), "text/html; charset=utf-8")
            if u.path in ("/style.css", "/app.js"):
                return self.send(200, open(os.path.join(PORTAL, u.path[1:]), "rb").read(), "text/css" if u.path.endswith("css") else "application/javascript")
            if u.path == "/api/schema": return self.send(200, SCHEMA)
            if u.path == "/api/config": return self.send(200, CFG)
            if u.path == "/api/config/download":
                c = json.loads(json.dumps(CFG))
                if q.get("support") == "1": c["setup"].pop("password", None); c["wifi"].pop("password", None)
                return self.send(200, json.dumps(c, indent=2), extra={"Content-Disposition": "attachment; filename=config.json"})
            if u.path == "/api/status": return self.send(200, status())
            if u.path == "/api/diag":
                if "--pads" in sys.argv and int(time.time()) % 3 == 0 and STATE.get("padAt", 0) != int(time.time()):   # a fake press every ~3 s for Identify
                    STATE["padAt"] = int(time.time())
                    STATE["padSeq"] += 1; STATE["padCh"] = [2, 3, 4, 5][(STATE["padSeq"] - 1) % 4]; STATE["padPos"] = STATE["padSeq"]
                return self.send(200, diag())
            if u.path == "/api/sounds": used_by(); return self.send(200, {"cardMounted": True, "count": len(SOUNDS), "loader": False, "sounds": SOUNDS})
            if u.path == "/api/log": return self.send(200, "\n".join("[%9d] I app     mock log line %d" % (i * 1000, i) for i in range(int(q.get("tail", "50")))), "text/plain")
            if u.path == "/api/coredump": return self.send(404, {"error": "no crash dump in flash"})
            if u.path == "/api/firmware/status":
                dt = time.time() - FW["t0"]
                if FW["job"] == "connecting" and dt > 2: FW["job"] = "checking" if FW["kind"] else "done"; FW["sta"] = True; FW["text"] = "checking for updates" if FW["kind"] else "joined HomeNet"; FW["t0"] = time.time()
                elif FW["job"] == "checking" and dt > 2:
                    FW["available"] = {"version": "v0.11.0", "size": 1657584, "notes": "Mock release notes.", "schema": 1, "minSchema": 1, "same": False}
                    if FW["kind"] == "install": FW["job"] = "downloading"; FW["text"] = "downloading v0.11.0"; FW["t0"] = time.time()
                    else: FW["job"] = "done"; FW["text"] = "v0.11.0 is available"
                elif FW["job"] == "downloading":
                    FW["pct"] = min(100, int(dt * 20)); FW["text"] = "downloading v0.11.0 %d %%" % FW["pct"]
                    if FW["pct"] >= 100: FW["job"] = "rebooting"; FW["text"] = "restarting with v0.11.0"
                return self.send(200, {"version": SCHEMA["version"], "slot": "app0", "state": "flashed over USB", "other": {"present": True, "version": "v0.9.0", "state": "valid", "eligible": True},
                                       "usb": True, "job": FW["job"], "pct": FW["pct"], "error": FW["error"], "text": FW["text"],
                                       "sta": {"connected": FW["sta"], "ssid": "HomeNet", "ip": "192.168.1.42" if FW["sta"] else "", "rssi": -61, "configured": True},
                                       "available": FW["available"], "repo": "alex-nugent/SoundBoard", "channel": "latest", "schema": 1, "heap": 60000})
        self.send(404, {"error": "no such endpoint"})
    def do_PUT(self): self.do_POST()
    def do_POST(self):
        u = urlparse(self.path); body, f = self.args()
        with LOCK:
            STATE["lastInput"] = time.time()
            if u.path == "/api/config":
                try: p = json.loads(body)
                except Exception as e: return self.send(400, {"ok": False, "error": "not valid JSON: %s" % e, "revision": CFG["revision"], "warnings": []})
                warn = []
                if "levels" in p and not p["levels"]: return self.send(400, {"ok": False, "error": "levels: at least one level", "revision": CFG["revision"], "warnings": []})
                merge(CFG, p); CFG["revision"] += 1
                if STATE["level"] > len(CFG["levels"]): STATE["level"] = 1
                return self.send(200, {"ok": True, "revision": CFG["revision"], "warnings": warn})
            if u.path == "/api/config/upload":
                try: doc = json.loads(body.decode() if isinstance(body, bytes) else body)
                except Exception as e: return self.send(400, {"ok": False, "error": "not valid JSON: %s" % e, "revision": CFG["revision"], "warnings": []})
                if f.get("fromThisBoard") != "1": doc["hardware"] = CFG["hardware"]
                CFG.clear(); CFG.update(doc); CFG["revision"] = CFG.get("revision", 0) + 1
                return self.send(200, {"ok": True, "revision": CFG["revision"], "warnings": ["mock: uploaded as given"]})
            if u.path == "/api/sounds/upload":
                name = f.get("filename", "")
                if any(s["name"].lower() == name.lower() for s in SOUNDS): return self.send(400, {"error": "a sound with that name exists: rename or delete it first"})
                SOUNDS.append({"name": name, "bytes": len(body), "rate": 44100, "channels": 1, "seconds": round(max(0, len(body) - 44) / 88200, 2), "state": "cached", "reason": "", "usedBy": []})
                return self.send(200, {"ok": True, "name": name, "info": "%.2f s, 1 ch, 44100 Hz, %d KB" % (max(0, len(body) - 44) / 88200, len(body) // 1024)})
            if u.path == "/api/sounds/delete":
                n = f.get("name", ""); SOUNDS[:] = [s for s in SOUNDS if s["name"] != n]
                for l in CFG["levels"]:
                    for b in l["buttons"]:
                        if b.get("sound", "").lower() == n.lower(): b["sound"] = ""
                return self.send(200, {"ok": True})
            if u.path == "/api/sounds/rename":
                n, to = f.get("name", ""), f.get("to", "")
                for s in SOUNDS:
                    if s["name"] == n: s["name"] = to
                for l in CFG["levels"]:
                    for b in l["buttons"]:
                        if b.get("sound", "").lower() == n.lower(): b["sound"] = to
                return self.send(200, {"ok": True})
            if u.path == "/api/play": return self.send(200, {"ok": True})
            if u.path == "/api/firmware/wifi/scan": return self.send(200, {"networks": [{"ssid": "HomeNet", "rssi": -61, "open": False}, {"ssid": "Neighbour", "rssi": -80, "open": False}]})
            if u.path == "/api/firmware/wifi/join": FW.update(job="connecting", kind="", text="joining " + f.get("ssid", ""), t0=time.time(), error=""); return self.send(200, {"ok": True, "job": "connecting"})
            if u.path in ("/api/firmware/check", "/api/firmware/install"):
                FW.update(job="connecting" if not FW["sta"] else "checking", kind="install" if u.path.endswith("install") else "check", text="checking", pct=0, t0=time.time(), error="")
                return self.send(200, {"ok": True, "job": FW["job"]})
            if u.path == "/api/firmware/cancel": FW.update(job="idle", pct=0, text="cancelled"); return self.send(200, {"ok": True, "job": "idle"})
            if u.path == "/api/firmware/rollback": FW.update(job="rebooting", pct=100, text="restarting with v0.9.0"); return self.send(200, {"ok": True, "job": "rebooting"})
            if u.path == "/api/firmware/upload": FW.update(job="rebooting", pct=100, text="restarting with the uploaded file"); return self.send(200, {"ok": True, "job": "rebooting"})
            if u.path == "/api/action":
                n = f.get("name")
                if n == "setLevel": STATE["level"] = int(f.get("level", 1))
                elif n in ("pairSpeaker", "forgetSpeakers"):
                    if not CFG["bluetoothSpeaker"]["enabled"]: return self.send(400, {"error": "enable the Bluetooth speaker first"})
                    STATE["pairing"] = "searching"; STATE["pairUntil"] = time.time() + 60
                elif n == "setBatteryK": return self.send(200, {"ok": True, "message": "K 6.395 from %s V (mock)" % f.get("volts")})
                elif n == "resetSettings":
                    for r in SCHEMA["settings"]:
                        if r["path"].startswith(("hardware.", "wifi.")): continue
                        d = r["def"]; v = d == "true" if r["type"] == "bool" else d if r["type"] in ("string", "enum") else float(d) if "." in d else int(d)
                        o = CFG; ks = r["path"].split(".")
                        for k in ks[:-1]: o = o.setdefault(k, {})
                        o[ks[-1]] = v
                    CFG["revision"] += 1
                elif n == "wifiOff": pass
                elif n not in ("buzz", "forgetHosts", "recalibrate", "closeJack", "preview", "mute"): return self.send(400, {"error": "unknown action \"%s\"" % n})
                return self.send(200, {"ok": True})
        self.send(404, {"error": "no such endpoint"})

if __name__ == "__main__":
    used_by()
    print("mock board at http://localhost:%d/  (--recovery for the banner, --pads for fake presses)" % PORT)
    ThreadingHTTPServer(("", PORT), H).serve_forever()
