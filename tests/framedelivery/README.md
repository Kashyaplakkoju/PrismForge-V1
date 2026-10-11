# Latest frame delivery regressions

`windows/src/video/latestframedelivery.h` separates capture/decode from the existing shared effects/output owner. It owns one active software AVFrame and one pending AVFrame. Each accepted submission replaces the pending frame instead of accumulating a FIFO. The callback receives the current source token and a monotonic queue wait in milliseconds.

## API and ownership

- `bool Start(uint64_t generation, Callback callback)` starts one worker, joining a previous worker first. Callback is `std::function<void(uint64_t, AVFrame*, double)>`.
- `bool Submit(uint64_t generation, const AVFrame*)` validates software geometry/planes and takes FFmpeg references. Completely borrowed software frames are copied into owned buffers by FFmpeg. The producer can unref/reuse the frame after Submit; referenced pixels require normal FFmpeg copy-on-write before mutation.
- `void SetGeneration(uint64_t)` purges pending frames and rejects cloning submissions begun before the switch. An already executing callback must use the application's SessionGate before publication.
- `void Stop()` prevents acceptance, purges pending frames and externally joins the worker. No callback is running when external Stop returns. A callback may request Stop; its join is deferred to external Stop/destruction. Start and owner destruction must never run on the callback thread. The callback may read stats, switch generation, submit or request Stop without taking a lock held across user work.
- The callback borrows its owned AVFrame until it returns; retain with `av_frame_clone` if needed afterward. Exceptions are counted and contained so a subsequent frame can run.
- `Stats GetStats() const` reports submitted, replaced, delivered, rejected, callbackErrors, purged, pending, inFlight and maxOutstanding. Counters reset on Start. Delivered counts callback attempts after return, including exceptions. Queue storage is bounded at two frames; temporary references taken by concurrent callers and frames deliberately retained by consumers are outside that queue bound.

Frames with unsupported hardware/palette/bitstream formats, unsafe owned plane spans or mixed owned/borrowed planes are rejected. Negative-stride owned and completely borrowed software video frames are supported. No per-pixel validation is performed.

## Run

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/framedelivery/run-tests.ps1
```

The runner uses the existing MSVC x64 toolchain with `/O2`, FFmpeg/vcpkg avutil and a 30-second process watchdog. Compilation or test failure propagates as a nonzero runner result. Logs are under `build/frame-delivery/`.

18 tests cover owned pixels/PTS/metadata/side data, borrowed ownership, 5,000 submissions behind a held consumer, exact latest sequence and drop counters, old-source purge, external and callback stop races, restart, callback exception recovery, malformed/hardware frame rejection, negative strides, eight concurrent producers, destructor draining, reentrant callbacks and a retained consumer clone. The actual SessionGate integration test holds old publication, switches through Advance then SetGeneration, rejects late/dequeued old input and verifies current new-source output pixels.

The verification receipt records the observed successful run and its source/log hashes. Separate intentional failure (child exit 19) and one-second watchdog probes were rejected with runner exit 1. These probes intentionally fail and must not be treated as regressions:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/framedelivery/run-tests.ps1 -ProbeFailure
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/framedelivery/run-tests.ps1 -WatchdogSeconds 1 -ProbeWatchdog
```

This proves bounded software frame delivery and lifecycle behavior. It does not measure phone capture-to-display latency or imply a live FPS guarantee.