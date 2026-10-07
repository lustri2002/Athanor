"""Keep an actual in-flight update download alive across a second app instance."""
import http.server
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time

binary = Path(sys.argv[1]).resolve()
package_root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
system = platform.system()
os_name = {"Darwin": "macOS", "Windows": "Windows", "Linux": "Linux"}[system]
arch = {"x86_64": "x64", "AMD64": "x64", "arm64": "arm64", "aarch64": "arm64"}[platform.machine()]
suffix = {"Darwin": ".zip", "Windows": ".exe", "Linux": ".tar.gz"}[system]
started = threading.Event()
resume = threading.Event()
payload = b"download regression fixture" * 32768

class Download(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload[:65536])
        self.wfile.flush()
        started.set()
        if resume.wait(25):
            self.wfile.write(payload[65536:])

    def log_message(self, *args):
        pass

with tempfile.TemporaryDirectory(prefix="athanor-download-") as folder:
    root = Path(folder).resolve()
    relative = Path("Contents/MacOS/Athanor") if system == "Darwin" else Path("usr/bin/Athanor") if system == "Linux" else Path("Athanor.exe")
    app = root / ("Athanor.app" if system == "Darwin" else "Athanor")
    if package_root:
        shutil.copytree(package_root, app, symlinks=True)
    else:
        (app / relative).parent.mkdir(parents=True)
        shutil.copy2(binary, app / relative)
    if system == "Linux":
        (app / ".athanor-portable").write_text("portable\n")
    qpa = {"Darwin": "cocoa", "Windows": "windows", "Linux": "xcb"}[system] if package_root else "offscreen"
    env = dict(os.environ, ATHANOR_TEST="1", ATHANOR_SETTINGS_DIR=str(root / "settings"),
               QT_QPA_PLATFORM=qpa, ATHANOR_LAUNCHER_PATH=str(root / "launcher.exe"))
    release = root / "release.json"
    env["ATHANOR_TEST_UPDATE_API"] = release.as_uri()
    name = f"Athanor-Alpha-9.9-{os_name}-{arch}{suffix}"
    release.write_text(json.dumps({"tag_name": "alpha-9.9", "assets": [{"name": name, "state": "uploaded",
        "size": len(payload), "digest": "sha256:" + "0" * 64,
        "browser_download_url": "https://github.com/SixFawn253/Athanor/releases/download/alpha-9.9/" + name}]}))

    def check():
        result = subprocess.run([str(binary), "--update-test", str(root)], env=env, capture_output=True, timeout=20)
        assert result.returncode == 0, result.stderr

    downloads = root / "settings/updates"
    downloads.mkdir(parents=True)
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    dead_pid = child.pid
    child.wait(timeout=5)

    def fixture(index, manifest):
        file = downloads / ("update-" + f"{index:032x}" + suffix)
        file.write_bytes(b"must preserve active/unowned archives")
        if manifest is not None:
            Path(str(file) + ".owner.json").write_text(json.dumps(manifest))
        return file

    active = fixture(1, {"pids": [dead_pid, os.getpid()]})
    stale = fixture(2, {"pids": [dead_pid]})
    legacy = fixture(3, None)
    pending = fixture(4, {"pids": [dead_pid], "handoff_pending": True})
    check()
    assert active.exists() and legacy.exists() and pending.exists()
    assert not stale.exists() and not Path(str(stale) + ".owner.json").exists()

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Download) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        install_env = dict(env, ATHANOR_TEST_UPDATE_INSTALL="1",
            ATHANOR_TEST_UPDATE_DOWNLOAD=f"http://127.0.0.1:{server.server_port}/download")
        process = subprocess.Popen([str(app / relative), "--update-test", str(root)], env=install_env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert started.wait(15), "The first instance did not start its download"
            deadline = time.monotonic() + 5
            live = []
            while time.monotonic() < deadline:
                live = [p for p in downloads.glob("update-*" + suffix) if p not in (active, legacy, pending) and p.stat().st_size]
                if live:
                    break
                time.sleep(.02)
            assert len(live) == 1, list(downloads.iterdir())
            archive = live[0]
            owner = json.loads(Path(str(archive) + ".owner.json").read_text())
            assert process.pid in owner["pids"], owner
            check()
            assert archive.exists() and archive.stat().st_size > 0, "Second instance removed the active download"
            resume.set()
            out, err = process.communicate(timeout=20)
            assert process.returncode == 0 and b"could not be verified" in out, (out, err)
            assert not archive.exists() and not Path(str(archive) + ".owner.json").exists()
        finally:
            resume.set()
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            server.shutdown()
print("Active download, helper ownership, abandoned cleanup and failed-download cleanup passed")
