# Port review: regression checks and measured optimizations

All fixtures are generated in temporary directories. These checks run both on
the source build and against the deployed runtime with developer search paths
removed. The shared conversion engine and UI remain the same targets on each OS.

| Scenario | Corrected behavior | Regression |
| --- | --- | --- |
| Immediate Unix cancellation before `setsid()` | Stop the root and its group; preserve the original even with deletion enabled | `processes` |
| Reusing a process instance | Reset Unix modifiers and Windows monitor/handles; ignore stale events | `processes` |
| Grouped image-to-PDF workspace failure or cancellation | Complete every row, count each cancelled row once | `processes` |
| Second app instance during an update download | Keep archives owned by live parent/helper processes; clean only confirmed abandoned downloads | `update_download` |
| Unix update with missing QML or no first frame | Preflight QML; retain backup until startup acknowledgement, then rollback on exit/timeout | `updater` |
| CLI PDF aliases and folder inputs | Use shared GUI/CLI target selection and folder expansion, including recursion and deduplication | `cli` |
| Explicit lossless APNG16 or MP4 request | Reject unsupported precision/codec combinations and preserve the original | `media_regressions` |
| Video with 90/180/270-degree display rotation | Validate displayed geometry and retain orientation | `media_regressions` |
| Audio shorter than its containing video | Validate the mapped audio stream; scan packet timestamps when track duration is absent | `media_regressions` |
| Repeated animated WebP frames | Keep pixels, alpha, frame count, durations and loop count using a mux fallback when needed | `media_regressions` |
| Preview of a long animation | Decode only the selected frame, with the existing negative/end clamping | `preview_regressions` |
| Alpha validation across target-size attempts | Hash the decoded byte stream without a raw file; cache the source fingerprint per conversion | `preview_regressions` |
| Repeated desktop integration setup | Preserve unchanged content/mtime; update changed commands and repair script permissions | `platform` |
| Application/package version | Derive application and asset names from `VERSION` and verify installed metadata | `version` |

## Diagnostic measurements

These are single-run samples on macOS arm64, Qt 6.11.2, using the native decoder
harness and identical generated inputs before/after the optimization. Pixel and
alpha hashes matched in each comparison. Memory figures are peak RSS in bytes;
the complete application's memory includes its UI and other active work.

| Operation | Before | After |
| --- | --- | --- |
| GIF 1024×1024, 120 frames: preview frame 0 | 450,756,608 RSS; 244 ms | 36,093,952 RSS; 51 ms |
| Same GIF: preview frame 80 | 533,184,512 RSS; 228 ms | 36,192,256 RSS; 142 ms |
| Alpha fingerprint, 1280×720, 30 fps, 2 seconds | 55,296,000 temporary bytes; 771 ms | 0 temporary bytes; 760 ms |
| Second identical Finder integration setup | 58 files rewritten; 29 contents changed | 0 rewritten; 0 contents changed |

The first three baselines use commit `623c594`; the optimization is in `1b79d7f`.
The Finder baseline uses the previous integration writer; `625addc` makes it
idempotent. Repeat measurements for throughput claims or different hardware.

## Remaining release checks

Public macOS signing/notarization requires the maintainer's credentials. GPU AV1
hardware and actual desktop notification delivery require the corresponding
hardware/session. The original private managed MSVC codec SDK is preserved and
is separate from the MSYS2 CI build.

Unix startup confirmation has a 20-second helper timeout and a 15-second timer
after QML load. Very slow startup restores the previous installation. Legacy
update archives with no ownership manifest, and incomplete ownership transfers,
are conservatively retained. The repeated-frame WebP fallback can increase file
size to preserve the sequence exactly.
