"""Verify the configured version, executable and release naming contract agree."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
metadata = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
source_version = (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
assert re.fullmatch(r"\d+\.\d+\.\d+", source_version), source_version
assert metadata["version"] == source_version, metadata
release_version = source_version.removesuffix(".0")
assert metadata["release_version"] == release_version, metadata
assert metadata["application_version"] == release_version + "-alpha", metadata
assert metadata["platform"] in {"Windows", "macOS", "Linux"}, metadata
assert re.fullmatch(r"[a-z0-9_]+", metadata["architecture"]), metadata
asset = f"Athanor-Alpha-{release_version}-{metadata['platform']}-{metadata['architecture']}"
assert metadata["asset_basename"] == asset, metadata
if len(sys.argv) > 3:
    installed = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
    assert installed == metadata, (installed, metadata)
env = dict(os.environ, ATHANOR_TEST="1")
result = subprocess.run([str(binary), "--version"], env=env, capture_output=True, text=True, encoding="utf-8", timeout=15)
assert result.returncode == 0, (result.stdout, result.stderr)
assert result.stdout.strip() == "Athanor " + metadata["application_version"], (result.stdout, metadata)
print(f"Version {source_version}, application {metadata['application_version']} and assets {asset} verified", flush=True)
