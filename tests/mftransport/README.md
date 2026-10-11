# Modern camera transport regressions

Run `powershell.exe -ExecutionPolicy Bypass -File tests/mftransport/run-tests.ps1` from the repository. Requires the already installed MSVC toolchain and C:\vcpkg FFmpeg; `-VcpkgRoot` may override the latter.

The runner builds in ignored build/mf-transport, launches with a 60s watchdog, captures stdout/stderr and fails on a nonzero child exit. All named mappings/mutexes use a unique `Local\PrismForge_Test_<pid>_<counter>` prefix. The suite uses MFStartup and the actual source RequestSample implementation in process. It never installs/registers a camera, contacts a phone, opens a capture device, or touches the production PrismForge mapping names.

See docs/MODERN_CAMERA_TRANSPORT.md for the original 24 regression groups, verified scope and live-validation limits.

The suite now also verifies exact limited-BT.709 NV12 and YUV420P packing with varied non-neutral pixels, poisoned padded rows and nonzero plane offsets; input immutability and malformed planes; repeated changes of format/range/matrix; and transitions between exact copies and narrow aspect-fitted output. The fitted-output check covers SIMD writes into neighboring black bars.

Run `powershell.exe -ExecutionPolicy Bypass -File tests/mftransport/run-writer-benchmark.ps1` for the controlled writer-only comparison. It compiles the exact pre-optimization writer from commit 86726cc (override `-BaselineCommit`), the current writer, and a read-only snapshot of the working VCamdroid reference (`-ReferenceRoot`). Only the reference snapshot's IPC name expressions are substituted with unique private Local objects; original files and production mappings are untouched. Each format receives 24 warmup frames and 360 samples with a changing current-frame pixel, in forward/reverse binary order. Raw results, stderr, source hashes and scope are saved in ignored build/mf-writer-benchmark.

Timings include MF conversion and publication only; they exclude capture, decode, preview, DirectShow, and consuming applications. Exact limited-BT.709 sample identity is checked independently, and an RGB-red probe reports whether each implementation produces the advertised limited BT.709 encoding. The reference's faster generic path is not colour-correct for every input; semantic differences must accompany timing comparisons. This benchmark does not verify end-to-end frame rate or latency.
