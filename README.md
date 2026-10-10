# nui-provider

Generates Xbox 360 NUI (Kinect) skeleton frames on the host and publishes them
where the xenia guest-side NUI plugin can read them.

This is the host half of the out-of-tree Kinect pipeline designed in
[tameid/xenia-canary `docs/kinect_support.md`](https://github.com/tameid/xenia-canary/blob/nui-spike/docs/kinect_support.md).
It shares no code with the emulator: the only contract between the two is the
wire format in `include/nui/frame.h`.

## Status — Phase 1

| deliverable | state |
|---|---|
| wire format v1, frozen | done — `include/nui/frame.h` |
| IPC writer, double-buffered | done — `src/ipc/file_writer.cc` |
| `synthetic` backend | done — deterministic, CI-safe, no hardware |
| `replay` backend | next |
| pose front-end / mapper / smoother | Phase 4+ (`webcam`, `phone`, `hardware`) |

The point of doing the format and the IPC before any pose code is that a title
can be driven with a *known* skeleton. If a title misbehaves, you can tell
whether the provider sent something wrong or the title is doing something you
do not understand yet.

## Build

Needs CMake ≥ 3.20, a C++17 compiler and Catch2 (vendored as a submodule):

```sh
git clone --recurse-submodules https://github.com/tameid/nui-provider
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, `ninja` plus `VsDevCmd.bat` in the environment works:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

```sh
# 120 frames of two skeletons into ./frames, then exit
./build/bin/nui-provider --dir frames --skeletons 2 --frames 120

# 30 Hz forever
./build/bin/nui-provider --dir frames --interval-ms 33
```

`--dir` is where the guest plugin looks (see below). `--interval-ms 0` emits as
fast as the disk allows, which is what a burst/replay consumer wants.

## The IPC protocol

```
<dir>/frame.idx      one line: which slot the guest should read next
<dir>/frame.0.bin    encoded frame
<dir>/frame.1.bin
```

The writer never mutates the slot the guest may be reading. It fills the other
one, `fflush`es it, and only then republishes `frame.idx`. The index is
replaced by write-temp-then-`rename`, because a plain rewrite can be observed
half-written and the guest would read a torn index.

**Latest-frame-wins.** If the guest falls behind, frames are overwritten rather
than queued. A Kinect consumer fed a queue is a consumer with growing latency,
which is worse than one that skips frames.

The guest side reads this over xenia's host path device, so in practice the
directory is mounted into the guest under a `plugins:`-style mount and the
plugin reads `frame.idx` / `frame.N.bin`.

## Wire format

`include/nui/frame.h` is **frozen**. Both the provider and the guest plugin
compile against it; changing it is a protocol change and needs a version bump,
not an edit.

Things that are easy to get wrong when touching it:

- **Packed and little-endian.** The guest is a big-endian PowerPC, so the plugin
  byte-swaps. Do not reorder fields to be host-friendlier.
- **Confidence is the most important field.** Titles gate hard on it, and a
  confidently-wrong joint is worse than a low-confidence one: it produces
  gestures and scores the title acts on. Emit 0 when you do not know. Never
  guess.
- **Two timestamps.** `capture_timestamp_us` is when the sensor sampled;
  `host_timestamp_us` is when the frame was written. The difference is pipeline
  latency and titles care about it.

Sizes are pinned by `static_assert` in the header *and* by tests in
`tests/codec_test.cc`, because a silent layout change would show up in the
guest as garbage poses rather than as a build error.

## Tests

`tests/codec_test.cc` pins the on-disk layout and round-trips frames, including
the failure paths that matter: a truncated frame, a bad magic or version, and a
header lying about its skeleton count.

`tests/ipc_test.cc` checks the handoff invariants — `frame.idx` alternates, the
slot it names is always complete, and no temp file survives.

## Licence

Apache-2.0, matching the emulator fork it feeds.