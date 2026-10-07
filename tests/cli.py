"""GUI action aliases and folder selection must also work from the CLI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from smoke import png, pdf

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="athanor-cli-") as folder:
    root = Path(folder)
    source = root / "input"
    nested = source / "nested"
    nested.mkdir(parents=True)
    png(source / "first.png")
    png(nested / "second.png", phase=20)
    (source / "readme.txt").write_text("Unsupported folder files must be filtered")
    document = root / "source.pdf"
    pdf(document)
    env = dict(os.environ, ATHANOR_TEST="1", ATHANOR_SETTINGS_DIR=str(root / "settings"))

    def run(label, *args, ok=True):
        result = subprocess.run([str(binary), "--cli", "--output", str(root / label), *map(str, args)],
                                env=env, capture_output=True, text=True, encoding="utf-8", timeout=60)
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        finals = [r for r in records if "ok" in r]
        assert result.returncode == (0 if ok else 1) and finals, (result.stdout, result.stderr)
        assert all(r["ok"] == ok for r in finals), finals
        return finals

    plain = run("flat", "--target", "png", source)
    assert len(plain) == 1 and Path(plain[0]["output"]).suffix == ".png"
    recursive = run("recursive", "--target", "png", "--include-subfolders", source)
    assert len(recursive) == 2 and all(Path(r["output"]).suffix == ".png" for r in recursive)
    deduplicated = run("duplicate", "--target", "png", source, source / "first.png")
    assert len(deduplicated) == 1
    grouped = run("grouped", "--target", "images-pdf", "--include-subfolders", source)
    assert len(grouped) == 1 and Path(grouped[0]["output"]).suffix == ".pdf"
    pages = run("pages", "--target", "pdf-jpg", document)
    assert len(pages) == 1 and len(list(Path(pages[0]["output"]).glob("*.jpg"))) == 2
    compressed = run("compress", "--pdf-output", "jpg", "--target", "pdf", document)
    assert len(compressed) == 1 and Path(compressed[0]["output"]).suffix == ".pdf"
    run("invalid", "--target", "unknown", source / "first.png", ok=False)
    run("missing", "--target", "png", root / "missing.png", ok=False)
print("CLI aliases, PDF grouping, recursive folders, deduplication and explicit errors passed")
