# Writes the release manifest (FirmwareSpec.md §16) for a built image:
#   python3 tools/manifest.py v1.2.0 soundboard-v1.2.0.bin > manifest.json
# `schema` is the configuration schema this image writes and `minSchema` the
# oldest it reads (both from lib/sbcore/src/config/config.h); the board refuses
# an install whose card file lies outside that window. The app slot is
# 0x640000 bytes (default_16MB.csv); a bigger image is refused here already.
import hashlib, json, os, re, sys

APP_SLOT = 0x640000
version, path = sys.argv[1], sys.argv[2]
src = open(os.path.join(os.path.dirname(__file__), "..", "lib", "sbcore", "src", "config", "config.h")).read()
schema = int(re.search(r"SCHEMA_VERSION\s*=\s*(\d+)", src).group(1))
data = open(path, "rb").read()
if len(data) > APP_SLOT:
    sys.exit("image %d bytes does not fit the app slot (%d)" % (len(data), APP_SLOT))
notes_path = os.path.join(os.path.dirname(__file__), "..", "RELEASE_NOTES.md")
notes = open(notes_path).read().strip() if os.path.exists(notes_path) else ""
print(json.dumps({
    "version": version,
    "file": os.path.basename(path),
    "size": len(data),
    "sha256": hashlib.sha256(data).hexdigest(),
    "schema": schema,
    "minSchema": 1,
    "notes": notes,
}, indent=2))
