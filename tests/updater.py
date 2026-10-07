"""Test matching-release selection and Unix installation/rollback in temporary roots."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

binary = Path(sys.argv[1]).resolve()
package_root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
system = {"Darwin": "macOS", "Windows": "Windows", "Linux": "Linux"}[platform.system()]
arch = {"x86_64": "x64", "AMD64": "x64", "arm64": "arm64", "aarch64": "arm64"}[platform.machine()]
suffix = {"macOS": ".zip", "Windows": ".exe", "Linux": ".tar.gz"}[system]
release = {}


with tempfile.TemporaryDirectory(prefix="athanor-updater-") as folder:
    root = Path(folder).resolve()
    qpa = {"macOS": "cocoa", "Windows": "windows", "Linux": "xcb"}[system] if package_root else "offscreen"
    env = dict(os.environ, ATHANOR_TEST="1", QT_QPA_PLATFORM=qpa, ATHANOR_SETTINGS_DIR=str(root / "settings"))
    if not package_root:
        env["QT_QUICK_BACKEND"] = "software"
    release_file = root / "release.json"
    env["ATHANOR_TEST_UPDATE_API"] = release_file.as_uri()
    env["ATHANOR_LAUNCHER_PATH"] = str(root / "Athanor.exe")
    expected = f"Athanor-Alpha-9.9-{system}-{arch}{suffix}"
    asset = {"name": expected, "state": "uploaded", "size": 100,
             "digest": "sha256:" + "a" * 64,
             "browser_download_url": "https://github.com/SixFawn253/Athanor/releases/download/alpha-9.9/" + expected}

    def check_release(available):
        release_file.write_text(json.dumps(release), encoding="utf-8")
        result = subprocess.run([str(binary), "--update-test", str(root)], env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert result.returncode == 0, (result.stdout, result.stderr)
        state = json.loads(result.stdout.strip().splitlines()[-1])
        assert bool(state["version"]) == available, state
        return state

    release.update(tag_name="alpha-9.9", name="Athanor 9.9", draft=False, prerelease=False, assets=[asset])
    check_release(True)
    asset["name"] = "Athanor-Alpha-9.9-OtherOS-x64.exe"
    check_release(False)
    asset["name"] = expected
    asset["digest"] = "sha256:invalid"
    check_release(False)
    asset["digest"] = "sha256:" + "a" * 64
    release["prerelease"] = True
    check_release(False)
    release["prerelease"] = False
    release["tag_name"] = "alpha-1.0"
    check_release(False)
    print("Release OS/architecture, digest, prerelease and version checks passed", flush=True)

    if system != "Windows":
        name = "Athanor.app" if system == "macOS" else "Athanor"
        executable = Path("Contents/MacOS/Athanor") if system == "macOS" else Path("usr/bin/Athanor")
        target = root / name
        replacement = root / "new" / name
        for destination in (target, replacement):
            if package_root:
                shutil.copytree(package_root, destination, symlinks=True)
            else:
                (destination / executable).parent.mkdir(parents=True)
                shutil.copy2(binary, destination / executable)
            if system == "Linux":
                (destination / ".athanor-portable").write_text("portable\n")
        (target / "generation").write_text("old")
        (replacement / "generation").write_text("new")
        original = root / "original.png"
        original.write_bytes(b"original remains outside the installed runtime")
        archive = root / ("update" + suffix)
        def make_archive(path, app=replacement):
            if system == "macOS" and package_root:
                # Match the release packager, including Qt framework symlinks.
                subprocess.run(["/usr/bin/ditto", "-c", "-k", "--keepParent", str(app), str(path)], check=True)
            elif system == "macOS":
                with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zip:
                    for entry in app.rglob("*"):
                        if entry.is_file():
                            zip.write(entry, entry.relative_to(app.parent))
            else:
                with tarfile.open(path, "w:gz") as tar:
                    tar.add(app, arcname=name)
        make_archive(archive)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        parent = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(.5)"])
        args = [str(target / executable), "--apply-update", str(archive), str(target), str(parent.pid)]
        rejected = subprocess.run([*args, "b" * 64], env=env, capture_output=True, text=True, encoding="utf-8", timeout=10)
        assert rejected.returncode != 0 and (target / "generation").read_text() == "old", rejected.stdout
        parent.wait(timeout=5)
        installed = subprocess.run([*args, digest], env=env, capture_output=True, text=True, encoding="utf-8", timeout=60)
        assert installed.returncode == 0 and (target / "generation").read_text() == "new", (installed.stdout, installed.stderr)
        assert not archive.exists() and not list(root.glob("*.athanor-backup-*"))

        def reject_update(app, label, expected_error, test_env=None):
            failed_archive = root / (label + suffix)
            make_archive(failed_archive, app)
            failed_digest = hashlib.sha256(failed_archive.read_bytes()).hexdigest()
            result = subprocess.run(
                [str(target / executable), "--apply-update", str(failed_archive), str(target), str(parent.pid), failed_digest],
                env=test_env or env, capture_output=True, text=True, encoding="utf-8", timeout=65)
            assert result.returncode != 0 and expected_error in result.stdout, (label, result.stdout, result.stderr)
            assert (target / "generation").read_text() == "new", label
            assert original.read_bytes() == b"original remains outside the installed runtime", label
            assert failed_archive.exists() and not list(root.glob("*.athanor-backup-*")), label
            return result

        # Loading the actual packaged QML modules must be checked, even when CLI
        # startup succeeds. Source builds use a small equivalent mode fixture.
        if package_root:
            controls = [entry.parent for entry in replacement.rglob("qmldir")
                        if entry.parent.name == "Controls" and entry.parent.parent.name == "QtQuick"]
            assert controls, "Packaged QtQuick.Controls module not found"
            for module in controls:
                shutil.rmtree(module)
            cli = subprocess.run([str(replacement / executable), "--cli"], env=env, capture_output=True, timeout=15)
            qml = subprocess.run([str(replacement / executable), "--qml-check"], env=env, capture_output=True, timeout=15)
            assert cli.returncode == 0 and qml.returncode != 0, (cli.stderr, qml.stderr)
            reject_update(replacement, "missing-qml", "Could not unpack or start the update")
        else:
            (replacement / executable).write_text("#!/bin/sh\n[ \"$1\" = --cli ] && exit 0\nprintf 'missing QML module' >&2\nexit 2\n")
            (replacement / executable).chmod(0o755)
            cli = subprocess.run([str(replacement / executable), "--cli"], env=env, timeout=5)
            assert cli.returncode == 0
            reject_update(replacement, "missing-qml", "missing QML module")

        # A candidate can pass QML construction and then stall before its first
        # frame. A started process is insufficient: it must send the real ACK.
        stalled = root / "stalled" / name
        (stalled / executable).parent.mkdir(parents=True)
        (stalled / "generation").write_text("stalled")
        (stalled / executable).write_text(
            "#!/bin/sh\n"
            "if [ -n \"${ATHANOR_UPDATE_READY_FILE:-}\" ]; then\n"
            "  printf started > \"$ATHANOR_SETTINGS_DIR/no-readiness-started\"\n"
            "  exec /bin/sleep 30\n"
            "fi\nexit 0\n")
        (stalled / executable).chmod(0o755)
        timeout_env = dict(env, ATHANOR_TEST_UPDATE_READY_TIMEOUT_MS="1000")
        reject_update(stalled, "no-readiness", "did not confirm startup", timeout_env)
        assert (root / "settings/no-readiness-started").read_text() == "started"

        (stalled / executable).write_text("#!/bin/sh\nexit 0\n")
        reject_update(stalled, "closed-before-readiness", "closed before its first frame", timeout_env)

        (stalled / executable).write_bytes(b"not an executable")
        reject_update(stalled, "broken-runtime", "Could not unpack or start the update")
        print("Update GUI readiness, checksum rejection, missing QML, timeout rollback and original retention passed", flush=True)
