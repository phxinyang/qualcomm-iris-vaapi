#!/bin/sh

# Validate the machine-readable production/fallback/rejected codec contract.
set -eu
root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
python3 - "$root/data/iris-codec-capabilities.json" <<'PY'
import json, pathlib, sys

path = pathlib.Path(sys.argv[1])
doc = json.loads(path.read_text())
assert doc["schema_version"] == 1
assert doc["policy"]["unknown_codec_action"] == "software-fallback"
entries = {item["codec"]: item for item in doc["codecs"]}
required = {"h264", "hevc", "vp8", "mpeg2", "vp9", "av1"}
assert set(entries) == required, (set(entries), required)
for codec, item in entries.items():
    assert item["production"] in {"supported", "fallback", "rejected"}, codec
    assert isinstance(item["va_profiles"], list), codec
    assert isinstance(item["v4l2_output_formats"], list), codec
    assert isinstance(item["capture_formats"], list), codec
    assert item["fallback"] in {"software", "native-v4l2-or-software"}, codec
    assert isinstance(item["rejected"], list) and item["rejected"], codec
    assert isinstance(item["experimental"], list), codec
assert entries["h264"]["production"] == "supported"
assert entries["hevc"]["production"] == "supported"
assert entries["vp9"]["production"] == "fallback" and not entries["vp9"]["va_profiles"]
assert entries["av1"]["production"] == "fallback" and not entries["av1"]["va_profiles"]
print(f"PASS codec capability matrix entries={len(entries)}")
PY
