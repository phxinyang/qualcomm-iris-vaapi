#!/usr/bin/env python3
"""Generate and verify the provenance record for a deployed VA driver."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
from typing import Any


SCHEMA = "qualcomm-iris-vaapi/provenance-v1"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL {message}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_output(command: list[str], default: str = "unavailable") -> str:
    try:
        result = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return default
    return result.stdout.strip() or default


def compiler_inventory(build_dir: Path) -> Any:
    output = command_output(
        ["meson", "introspect", "--compilers", str(build_dir)], default=""
    )
    if output:
        try:
            return json.loads(output)
        except json.JSONDecodeError:
            pass
    return {"fallback": command_output([os.environ.get("CXX", "c++"), "--version"]).splitlines()[0]}


def validate_source_identity(commit: str, digest: str, dirty: str) -> bool:
    if len(commit) not in (40, 64) or any(c not in "0123456789abcdefABCDEF" for c in commit):
        fail("source commit must be a full hexadecimal object id")
    if len(digest) != 64 or any(c not in "0123456789abcdefABCDEF" for c in digest):
        fail("tracked source digest must be a SHA-256 hexadecimal string")
    if dirty not in ("0", "1"):
        fail("source dirty flag must be 0 or 1")
    return dirty == "1"


def iris_module_identity() -> dict:
    """SHA-256 and path of the installed qcom-iris module, if any."""
    root = Path("/lib/modules") / platform.release()
    for candidate in sorted(root.glob("**/qcom-iris.ko*")) if root.is_dir() else []:
        try:
            return {"path": str(candidate), "sha256": sha256(candidate)}
        except OSError:
            continue
    return {}


def generate(args: argparse.Namespace) -> None:
    artifact = args.artifact.resolve()
    build_dir = args.build_dir.resolve()
    output = args.output.resolve()
    if not artifact.is_file():
        fail(f"driver artifact does not exist: {artifact}")
    dirty = validate_source_identity(args.source_commit, args.tracked_source_sha256, args.source_dirty)

    record = {
        "schema": SCHEMA,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source": {
            "commit": args.source_commit.lower(),
            "tracked_sha256": args.tracked_source_sha256.lower(),
            "dirty": dirty,
        },
        "build": {
            "compiler": compiler_inventory(build_dir),
            "libva_version": command_output(["pkg-config", "--modversion", "libva"]),
            "meson_version": command_output(["meson", "--version"]),
        },
        "runtime": {
            "kernel_release": platform.release(),
            "kernel": command_output(["uname", "-srvm"]),
            "machine": platform.machine(),
            # The Iris kernel module is part of the qualified contract (the
            # decode-order patch lives there), so its identity travels with
            # every hardware result. Absent on hosts without the module.
            "iris_module": iris_module_identity(),
        },
        "artifact": {
            "path": str(artifact),
            "size": artifact.stat().st_size,
            "sha256": sha256(artifact),
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".new")
    temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(output)
    print(f"PASS provenance generated manifest={output} sha256={record['artifact']['sha256']}")


def require_mapping(parent: dict[str, Any], key: str) -> dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        fail(f"manifest field {key} is missing or not an object")
    return value


def verify(args: argparse.Namespace) -> None:
    manifest = args.manifest.resolve()
    artifact = args.artifact.resolve()
    try:
        record = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read manifest {manifest}: {error}")
    if not isinstance(record, dict) or record.get("schema") != SCHEMA:
        fail(f"unsupported provenance schema in {manifest}")

    source = require_mapping(record, "source")
    build = require_mapping(record, "build")
    runtime = require_mapping(record, "runtime")
    recorded_artifact = require_mapping(record, "artifact")

    commit = source.get("commit")
    digest = source.get("tracked_sha256")
    dirty_value = source.get("dirty")
    if not isinstance(commit, str) or not isinstance(digest, str) or not isinstance(dirty_value, bool):
        fail("manifest source identity is incomplete")
    validate_source_identity(commit, digest, "1" if dirty_value else "0")
    for section, key in (
        (build, "compiler"),
        (build, "libva_version"),
        (runtime, "kernel_release"),
        (runtime, "kernel"),
    ):
        if section.get(key) in (None, "", {}, []):
            fail(f"manifest field {key} is empty")

    if not artifact.is_file():
        fail(f"driver artifact does not exist: {artifact}")
    actual_sha = sha256(artifact)
    actual_size = artifact.stat().st_size
    if recorded_artifact.get("sha256") != actual_sha:
        fail("driver SHA-256 does not match manifest")
    if recorded_artifact.get("size") != actual_size:
        fail("driver size does not match manifest")
    if Path(str(recorded_artifact.get("path", ""))).resolve() != artifact:
        fail("driver path does not match manifest")

    if args.expected_source_commit and commit != args.expected_source_commit.lower():
        fail("source commit does not match deployment identity")
    if args.expected_tracked_source_sha256 and digest != args.expected_tracked_source_sha256.lower():
        fail("tracked source digest does not match deployment identity")
    if args.expected_source_dirty is not None:
        expected_dirty = args.expected_source_dirty == "1"
        if dirty_value != expected_dirty:
            fail("source dirty flag does not match deployment identity")

    print(
        "PASS provenance verified "
        f"commit={commit} tracked_sha256={digest} artifact_sha256={actual_sha}"
    )


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    generate_parser = commands.add_parser("generate")
    generate_parser.add_argument("--source-commit", required=True)
    generate_parser.add_argument("--tracked-source-sha256", required=True)
    generate_parser.add_argument("--source-dirty", choices=("0", "1"), required=True)
    generate_parser.add_argument("--build-dir", type=Path, required=True)
    generate_parser.add_argument("--artifact", type=Path, required=True)
    generate_parser.add_argument("--output", type=Path, required=True)
    generate_parser.set_defaults(handler=generate)

    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser.add_argument("--artifact", type=Path, required=True)
    verify_parser.add_argument("--expected-source-commit")
    verify_parser.add_argument("--expected-tracked-source-sha256")
    verify_parser.add_argument("--expected-source-dirty", choices=("0", "1"))
    verify_parser.set_defaults(handler=verify)
    return root


def main() -> None:
    args = parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
