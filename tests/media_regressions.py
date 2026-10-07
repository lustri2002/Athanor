"""Independent media fidelity regressions; all inputs and settings are temporary."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


def executable(name, binary):
    filename = name + (".exe" if os.name == "nt" else "")
    folders = [os.environ.get("ATHANOR_TOOLS_DIR"), binary.parent / "tools", binary.parent,
               binary.parent / "../Resources/tools", binary.parent / "../lib/athanor/tools"]
    for folder in folders:
        if folder and (Path(folder) / filename).is_file():
            return str((Path(folder) / filename).resolve())
    result = shutil.which(name)
    assert result, f"{name} is required for independent media validation"
    return result


def command(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, timeout=180)
    assert result.returncode == 0, (args, result.stderr.decode("utf-8", "replace"))
    return result.stdout


def convert(binary, root, source, *args, ok=True):
    env = dict(os.environ, ATHANOR_TEST="1", ATHANOR_SETTINGS_DIR=str(root / "settings"))
    result = subprocess.run([str(binary), "--cli", "--speed", "Fast", "--threads", "2",
                             "--acceleration", "CPU", "--output", str(root / "output"),
                             *map(str, args), str(source)], env=env, capture_output=True,
                            text=True, encoding="utf-8", timeout=180)
    records = []
    for line in result.stdout.splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "ok" in record:
            records.append(record)
    assert records and records[-1]["ok"] == ok, (result.returncode, result.stdout, result.stderr)
    assert result.returncode == (0 if ok else 1), (result.returncode, result.stdout, result.stderr)
    return records[-1]


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def apng(path, depth=8, phases=(0, 0, 31), durations=(250, 375, 500), loop=3):
    width = height = 64
    data = bytearray(b"\x89PNG\r\n\x1a\n")
    data += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, 6, 0, 0, 0))
    data += png_chunk(b"acTL", struct.pack(">II", len(phases), loop))
    sequence = 0
    for i, (phase, duration) in enumerate(zip(phases, durations)):
        control = struct.pack(">IIIIIHHBB", sequence, width, height, 0, 0, duration, 1000, 0, 0)
        data += png_chunk(b"fcTL", control)
        sequence += 1
        rows = bytearray()
        for y in range(height):
            rows.append(0)
            for x in range(width):
                if depth == 16:
                    rows += struct.pack(">4H", (x * 997 + phase) % 65536, y * 911,
                                        (x * 193 + y * 131 + phase) % 65536, 65535)
                else:
                    rows += bytes(((x * 3 + phase) % 256, y * 3, (x + y + phase) % 256,
                                   0 if x == 0 else x * 4))
        encoded = zlib.compress(rows)
        if i == 0:
            data += png_chunk(b"IDAT", encoded)
        else:
            data += png_chunk(b"fdAT", struct.pack(">I", sequence) + encoded)
            sequence += 1
    data += png_chunk(b"IEND", b"")
    path.write_bytes(data)


def probe(ffprobe, path):
    return json.loads(command(ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", path))


def raw_video(ffmpeg, path, first=False):
    args = [ffmpeg, "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0"]
    if first:
        args += ["-frames:v", "1"]
    return command(*args, "-fps_mode", "passthrough", "-pix_fmt", "rgba", "-f", "rawvideo", "-")


def webp_frames(ffmpeg, path, root):
    """Parse timing/loop independently, then decode each full WebP frame with FFmpeg.

    This also works with FFmpeg versions that cannot demux animated WebP.
    """
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WEBP"
    offset, loop, frames, durations = 12, None, [], []
    while offset + 8 <= len(data):
        kind, size = data[offset:offset + 4], struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8:offset + 8 + size]
        assert len(chunk) == size
        if kind == b"ANIM":
            loop = struct.unpack_from("<H", chunk, 4)[0]
        elif kind == b"ANMF":
            number = lambda start: int.from_bytes(chunk[start:start + 3], "little")
            assert number(0) == number(3) == 0, "Regression fixture expects full, unshifted frames"
            assert number(6) + 1 == number(9) + 1 == 64
            assert chunk[15] & 2, "Full frames must replace the previous canvas without blending"
            durations.append(number(12))
            frame = root / f"independent-frame-{len(frames)}.webp"
            contents = b"WEBP" + chunk[16:]
            frame.write_bytes(b"RIFF" + struct.pack("<I", len(contents)) + contents)
            frames.append(raw_video(ffmpeg, frame, first=True))
        offset += 8 + size + (size & 1)
    return b"".join(frames), durations, loop


def main():
    binary = Path(sys.argv[1]).resolve()
    ffmpeg, ffprobe = executable("ffmpeg", binary), executable("ffprobe", binary)
    with tempfile.TemporaryDirectory(prefix="athanor-media-regressions-") as directory:
        root = Path(directory)
        (root / "output").mkdir()

        deep = root / "precision16.apng"
        apng(deep, depth=16, phases=(0, 19), durations=(250, 375))
        assert probe(ffprobe, deep)["streams"][0]["pix_fmt"] == "rgba64be"
        original = deep.read_bytes()
        result = convert(binary, root, deep, "--image", "webp", "--lossless", "--delete-originals", ok=False)
        assert "8-bit" in result["error"] and deep.read_bytes() == original
        assert not list((root / "output").iterdir())
        print("APNG16 lossless rejection preserves original", flush=True)

        video = root / "lossless-source.mkv"
        command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "testsrc2=size=128x96:rate=10:duration=1", "-c:v", "ffv1", video)
        original = video.read_bytes()
        result = convert(binary, root, video, "--video", "mp4", "--lossless", "--delete-originals", ok=False)
        assert "Lossless MP4" in result["error"] and video.read_bytes() == original
        assert not list((root / "output").iterdir())
        print("Explicit lossless MP4 rejection preserves original", flush=True)

        base = root / "landscape.mp4"
        command(ffmpeg, "-nostdin", "-v", "error", "-i", video, "-c:v", "libx264", "-crf", "0", base)
        for rotation in (90, 180, 270):
            source = root / f"rotation-{rotation}.mp4"
            command(ffmpeg, "-nostdin", "-v", "error", "-display_rotation", rotation, "-i", base, "-c", "copy", source)
            info = probe(ffprobe, source)["streams"][0]
            assert any(side.get("rotation", 0) % 360 == rotation % 360
                       for side in info.get("side_data_list", [])), info
            expected = (96, 128) if rotation in (90, 270) else (128, 96)
            original_frames = raw_video(ffmpeg, source)
            result = convert(binary, root, source, "--video", "mkv", "--lossless")
            output = Path(result["output"])
            stream = probe(ffprobe, output)["streams"][0]
            assert (stream["width"], stream["height"]) == expected
            assert hashlib.sha256(raw_video(ffmpeg, output)).digest() == hashlib.sha256(original_frames).digest()
            result = convert(binary, root, source, "--video", "mp4")
            output = Path(result["output"])
            stream = probe(ffprobe, output)["streams"][0]
            assert (stream["width"], stream["height"]) == expected
            before, after = raw_video(ffmpeg, source, first=True), raw_video(ffmpeg, output, first=True)
            assert len(before) == len(after)
            error = sum(abs(a - b) for i, (a, b) in enumerate(zip(before, after)) if i % 4 != 3)
            assert error / (len(before) * 3 / 4) < 20, "MP4 display orientation changed"
            print("Rotation", rotation, "geometry and decoded fidelity", flush=True)

        short_audio = root / "video-long-audio-short.mp4"
        command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "testsrc2=size=128x128:rate=10:duration=2", "-f", "lavfi", "-i",
                "sine=frequency=440:duration=1", "-c:v", "libx264", "-c:a", "aac", short_audio)
        info = probe(ffprobe, short_audio)
        audio_stream = next(stream for stream in info["streams"] if stream["codec_type"] == "audio")
        assert float(info["format"]["duration"]) > float(audio_stream["duration"]) + 0.5
        result = convert(binary, root, short_audio, "--video", "opus")
        out_audio = probe(ffprobe, result["output"])["streams"][0]
        assert abs(float(out_audio["duration"]) - float(audio_stream["duration"])) < 0.1
        result = convert(binary, root, short_audio, "--video", "wav")
        pcm = lambda path: command(ffmpeg, "-nostdin", "-v", "error", "-i", path, "-map", "0:a:0",
                                   "-c:a", "pcm_s24le", "-f", "s24le", "-")
        assert hashlib.sha256(pcm(short_audio)).digest() == hashlib.sha256(pcm(result["output"])).digest()
        print("Audio duration and independent selected-stream PCM hash", flush=True)

        # Live Matroska omits explicit track duration tags. FFprobe may report a
        # container estimate for every stream, which is not the mapped audio's length.
        untagged = root / "audio-without-track-duration.mkv"
        command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "testsrc2=size=64x64:rate=10:duration=2", "-f", "lavfi", "-i",
                "sine=frequency=440:duration=1", "-c:v", "ffv1", "-c:a", "pcm_s16le", "-live", "1", untagged)
        track = next(s for s in probe(ffprobe, untagged)["streams"] if s["codec_type"] == "audio")
        assert not any(key.upper() == "DURATION" for key in track.get("tags", {})), track
        result = convert(binary, root, untagged, "--video", "opus", "--target-size", "0.015")
        output = Path(result["output"])
        assert output.stat().st_size <= 15000
        assert abs(float(probe(ffprobe, output)["streams"][0]["duration"]) - 1) < .1
        result = convert(binary, root, untagged, "--video", "wav")
        assert hashlib.sha256(pcm(untagged)).digest() == hashlib.sha256(pcm(result["output"])).digest()
        print("Untagged audio: packet duration, target size and PCM hash", flush=True)

        for index, (phases, durations, loop) in enumerate([
                ((0, 0, 31), (250, 375, 500), 3),
                ((0, 0), (250, 375), 1),
                ((0, 0), (250, 375), 0)]):
            source = root / f"duplicate-{index}.apng"
            apng(source, phases=phases, durations=durations, loop=loop)
            expected_pixels = raw_video(ffmpeg, source)
            result = convert(binary, root, source, "--image", "webp", "--lossless")
            actual_pixels, actual_durations, actual_loop = webp_frames(ffmpeg, Path(result["output"]), root)
            assert actual_durations == list(durations) and actual_loop == loop
            assert hashlib.sha256(actual_pixels).digest() == hashlib.sha256(expected_pixels).digest()
            print("Repeated animation frames", index, "pixel/alpha hash, timing and loop", flush=True)

        assert not list((root / "output").glob(".athanor-*")), "Staging workspaces were left behind"
        print("All independent media regressions passed", flush=True)


if __name__ == "__main__":
    main()
