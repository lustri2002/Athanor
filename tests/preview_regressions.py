"""Selective previews and streamed alpha validation, using independent FFmpeg output."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import zlib

from media_regressions import apng, command, convert, executable, png_chunk, probe, raw_video


def read_image(binary, root, source, frame):
    output = root / "selected.png"
    job = root / "read-image.json"
    job.write_text(json.dumps({"source": str(source), "frame": frame, "output": str(output)}))
    env = dict(os.environ, ATHANOR_TEST="1", ATHANOR_SETTINGS_DIR=str(root / "settings"))
    result = subprocess.run([str(binary), "--read-image-test", str(job)], env=env, capture_output=True,
                            text=True, encoding="utf-8", timeout=180)
    records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    assert result.returncode == 0 and records and records[-1]["ok"], (result.stdout, result.stderr)
    return output


def digest(data):
    return hashlib.sha256(data).digest()


def png_profile(path):
    data, offset = path.read_bytes(), 8
    while offset + 8 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind, chunk = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + size]
        if kind == b"iCCP":
            name_end = chunk.index(b"\0")
            assert chunk[name_end + 1] == 0
            return zlib.decompress(chunk[name_end + 2:])
        offset += size + 12
    return None


def main():
    binary = Path(sys.argv[1]).resolve()
    ffmpeg, ffprobe = executable("ffmpeg", binary), executable("ffprobe", binary)
    avifenc = executable("avifenc", binary)
    with tempfile.TemporaryDirectory(prefix="athanor-preview-regressions-") as directory:
        root = Path(directory)
        (root / "output").mkdir()
        source = root / "animation.apng"
        apng(source)
        frame_bytes = 64 * 64 * 4
        expected = raw_video(ffmpeg, source)
        assert len(expected) == frame_bytes * 3
        webp = Path(convert(binary, root, source, "--image", "webp", "--lossless")["output"])
        # Use the independent encoder and the APNG's known RGBA frames as oracle.
        pngs = []
        for index in range(3):
            png = root / f"frame-{index}.png"
            command(ffmpeg, "-nostdin", "-v", "error", "-i", source, "-vf", f"select=eq(n\\,{index})",
                    "-frames:v", "1", "-fps_mode", "passthrough", "-pix_fmt", "rgba", png)
            pngs.append(png)
        avif = root / "animation.avif"
        command(avifenc, "--lossless", "--speed", "8", "--jobs", "2", "--timescale", "1000",
                "--duration", "250", pngs[0], "--duration", "375", pngs[1], "--duration", "500", pngs[2], avif)
        heif = Path(convert(binary, root, pngs[0], "--image", "heic", "--lossless")["output"])
        for path, count, pixels in [(source, 3, expected), (webp, 3, expected), (avif, 3, expected),
                                     (heif, 1, expected[:frame_bytes])]:
            for requested in (-1, 0, 1, count - 1, count + 20):
                index = max(0, min(requested, count - 1))
                output = read_image(binary, root, path, requested)
                actual = raw_video(ffmpeg, output, first=True)
                assert digest(actual) == digest(pixels[index * frame_bytes:(index + 1) * frame_bytes]), (path, requested)
            print(path.suffix, "selected/clamped frame RGB and alpha hashes", flush=True)

        gif = root / "animation.gif"
        command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "testsrc2=size=64x64:rate=10:duration=0.5", "-filter_complex",
                "split[a][b];[a]palettegen[p];[b][p]paletteuse", gif)
        expected_gif = raw_video(ffmpeg, gif)
        count = len(expected_gif) // frame_bytes
        for requested in (0, 2, count + 10):
            output = read_image(binary, root, gif, requested)
            index = min(requested, count - 1)
            assert digest(raw_video(ffmpeg, output, first=True)) == digest(expected_gif[index * frame_bytes:(index + 1) * frame_bytes])
        print("GIF selective frames match independent decoder", flush=True)

        tagged = root / "srgb.png"
        data = pngs[0].read_bytes()
        tagged.write_bytes(data[:33] + png_chunk(b"sRGB", b"\0") + data[33:])
        profiled = root / "profiled.png"
        profiled.write_bytes(read_image(binary, root, tagged, 0).read_bytes())
        profile = png_profile(profiled)
        assert profile and profile[36:40] == b"acsp"
        for fmt in ("png", "webp", "avif", "heic"):
            path = profiled if fmt == "png" else Path(convert(binary, root, profiled, "--image", fmt, "--lossless")["output"])
            output = read_image(binary, root, path, 0)
            assert png_profile(output) == profile, (fmt, "ICC profile changed")
            assert digest(raw_video(ffmpeg, output, first=True)) == digest(expected[:frame_bytes])
        print("PNG/WebP/AVIF/HEIF ICC profiles and pixels preserved", flush=True)

        jpeg = root / "oriented.jpg"
        command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=96x64:duration=0.1",
                "-frames:v", "1", "-pix_fmt", "yuvj444p", "-q:v", "1", jpeg)
        coded = raw_video(ffmpeg, jpeg, first=True)
        # TIFF little-endian Orientation=6 means rotate 90 degrees clockwise.
        exif = b"Exif\0\0II" + struct.pack("<HIH", 42, 8, 1) + struct.pack("<HHIHHI", 0x112, 3, 1, 6, 0, 0)
        data = jpeg.read_bytes()
        jpeg.write_bytes(data[:2] + b"\xff\xe1" + struct.pack(">H", len(exif) + 2) + exif + data[2:])
        output = read_image(binary, root, jpeg, 0)
        stream = probe(ffprobe, output)["streams"][0]
        assert (stream["width"], stream["height"]) == (64, 96)
        rotated = b"".join(coded[((63 - x) * 96 + y) * 4:((63 - x) * 96 + y) * 4 + 4]
                           for y in range(96) for x in range(64))
        actual = raw_video(ffmpeg, output, first=True)
        assert sum(abs(a - b) for a, b in zip(rotated, actual)) / len(rotated) < 4
        print("JPEG EXIF orientation preserved", flush=True)

        alpha_source = root / "transparent.mkv"
        command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "testsrc=size=128x96:rate=10:duration=0.5", "-vf", "format=rgba,colorkey=0xff0000:0.1:0.0",
                "-c:v", "ffv1", "-pix_fmt", "bgra", alpha_source)
        log = root / "ffmpeg-commands.jsonl"
        original_override = os.environ.get("ATHANOR_TOOLS_DIR")
        try:
            if os.name != "nt":
                tools = root / "instrumented-tools"
                tools.mkdir()
                wrapper = tools / "ffmpeg"
                wrapper.write_text("#!/usr/bin/env python3\nimport json, os, sys\n"
                                   f"with open({str(log)!r}, 'a') as stream: stream.write(json.dumps(sys.argv[1:])+'\\n')\n"
                                   f"os.execv({ffmpeg!r}, [{ffmpeg!r}, *sys.argv[1:]])\n")
                wrapper.chmod(0o755)
                for name, target in [("ffprobe", ffprobe), ("avifenc", avifenc)]:
                    (tools / name).symlink_to(target)
                os.environ["ATHANOR_TOOLS_DIR"] = str(tools)
            result = convert(binary, root, alpha_source, "--video", "webm", "--target-size", "0.004")
        finally:
            if original_override is None:
                os.environ.pop("ATHANOR_TOOLS_DIR", None)
            else:
                os.environ["ATHANOR_TOOLS_DIR"] = original_override
        output = Path(result["output"])
        def alpha(path):
            args = [ffmpeg, "-nostdin", "-v", "error"]
            if probe(ffprobe, path)["streams"][0]["codec_name"] == "vp9":
                args += ["-c:v", "libvpx-vp9"]
            return command(*args, "-i", path, "-vf", "format=rgba,alphaextract", "-fps_mode", "passthrough",
                           "-pix_fmt", "gray", "-f", "rawvideo", "-")
        assert digest(alpha(alpha_source)) == digest(alpha(output))
        if log.exists():
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            fingerprints = [args for args in calls if any("alphaextract" in arg for arg in args)]
            assert len(fingerprints) > 3, "Fixture must exercise multiple target-size attempts"
            assert all(args[-1] == "pipe:1" for args in fingerprints), "Alpha data was written to disk"
            source_calls = [args for args in fingerprints if args[args.index("-i") + 1] == str(alpha_source)]
            assert len(source_calls) == 1, "Source alpha was decoded again during target-size trials"
            print("Alpha fingerprints stream to stdout; source decoded once across trials", flush=True)
        print("Independent output alpha hash preserved", flush=True)
        assert not list((root / "output").glob(".athanor-*")), "Staging workspaces were left behind"
        print("All selective preview and alpha regressions passed", flush=True)


if __name__ == "__main__":
    main()
