#!/bin/sh

# Validate a report captured by electron-video-acceptance.html. This is a
# hardware-independent schema/claim gate; it never treats a missing decoder
# field or a shell/GPU startup log as a hardware-decode pass.
set -eu
root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
report=${2:-"$root/test/electron-video-report.example.json"}
expect_hardware=${IRIS_ELECTRON_EXPECT_HARDWARE:-0}

python3 - "$report" "$expect_hardware" <<'PY'
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
expect_hardware = sys.argv[2] == "1"
doc = json.loads(path.read_text())
assert doc["schema_version"] == 1
assert doc["result"] in {"pass", "unqualified", "fail"}
for group in ("runtime", "display", "launch", "target", "media"):
    assert isinstance(doc[group], dict), group
for key in ("application", "application_version", "electron_version", "chrome_version", "user_agent"):
    assert isinstance(doc["runtime"].get(key), str) and doc["runtime"][key], key
assert doc["display"].get("server") in {"wayland", "x11", "unknown"}
assert isinstance(doc["launch"].get("sandbox_flags"), list)
for key in ("resolved_iris_node", "boot_id", "source_commit", "driver_sha256"):
    assert isinstance(doc["target"].get(key), str) and doc["target"][key], key
media = doc["media"]
assert isinstance(media.get("decoder_name"), str) and media["decoder_name"]
assert media.get("is_platform_video_decoder") in {True, False, None}
for key in ("decoded_frames", "dropped_frames"):
    value = media.get(key)
    assert value is None or (isinstance(value, int) and value >= 0), key
assert isinstance(doc.get("limitations"), list) and doc["limitations"]

# A hardware pass is a release claim, so its provenance must be usable to
# reproduce the run. Keep the schema permissive for unqualified exploratory
# reports, but reject placeholders and abbreviated hashes when pass evidence is
# requested.
provenance = doc["target"]
if doc["result"] == "pass" or expect_hardware:
    assert re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", provenance["source_commit"]), \
        "pass report requires a full source commit"
    assert re.fullmatch(r"[0-9a-fA-F]{64}", provenance["driver_sha256"]), \
        "pass report requires a driver SHA-256"
    assert provenance["boot_id"] not in {"unknown", "paste boot id before and after playback"}, \
        "pass report requires boot-id evidence"
    assert provenance["resolved_iris_node"] not in {"unknown", "/dev/videoX"}, \
        "pass report requires the resolved Iris node"

hardware = (media["decoder_name"] == "VaapiVideoDecoder" and
            media["is_platform_video_decoder"] is True and
            isinstance(media.get("decoded_frames"), int) and media["decoded_frames"] > 0 and
            isinstance(media.get("dropped_frames"), int) and media["dropped_frames"] >= 0)
if doc["result"] == "pass":
    assert hardware, "pass report lacks VaapiVideoDecoder/platform/frame evidence"
if expect_hardware:
    assert hardware, "hardware acceptance failed: require VaapiVideoDecoder, platform=true, decoded_frames>0"
if re.search(r"/dev/video[0-9]+$", doc["target"]["resolved_iris_node"]):
    print("WARN resolved node is boot-specific; retain it as evidence, never as launcher configuration", file=sys.stderr)
print(f"PASS electron report schema result={doc['result']} hardware_evidence={str(hardware).lower()} file={path}")
PY
