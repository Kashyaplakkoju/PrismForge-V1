#include "effects/desktopcolorpipeline.h"
#include "capture/inputsession.h"
#include "video/directshowscaler.h"
#include "video/guipreviewscaler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

using namespace PrismForge::Effects;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; return false; } } while (false)

struct Frame {
    AVFrame* value = av_frame_alloc();
    Frame(int width = 5, int height = 3, AVPixelFormat format = AV_PIX_FMT_BGR24) {
        if (!value) throw std::bad_alloc();
        value->format = format; value->width = width; value->height = height;
        if (av_frame_get_buffer(value, 64) < 0) { av_frame_free(&value); throw std::bad_alloc(); }
        value->color_range = AVCOL_RANGE_JPEG; value->colorspace = AVCOL_SPC_RGB;
        value->pts = 819; value->pkt_dts = 810; value->duration = 33; value->time_base = {1, 1000};
        value->best_effort_timestamp = 815; value->sample_aspect_ratio = {4, 3};
        value->color_primaries = AVCOL_PRI_BT709; value->color_trc = AVCOL_TRC_BT709;
    }
    ~Frame() { av_frame_free(&value); }
    void Fill(uint8_t b, uint8_t g, uint8_t r) {
        for (int y = 0; y < value->height; ++y) {
            auto* row = value->data[0] + y * value->linesize[0];
            std::memset(row, 0xcd, value->linesize[0]);
            for (int x = 0; x < value->width; ++x) { row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r; }
        }
    }
};
std::string Cube(int mode = 0, std::string header = "LUT_3D_SIZE 2\n") {
    for (int b = 0; b < 2; ++b) for (int g = 0; g < 2; ++g) for (int r = 0; r < 2; ++r) {
        if (mode == 1) header += std::to_string(b) + " " + std::to_string(g) + " " + std::to_string(r) + "\n";
        else if (mode == 2) header += "0 0 0\n";
        else if (mode == 3) header += "1 1 1\n";
        else if (mode == 4) header += std::to_string(r * g) + " " + std::to_string(g * b) + " " + std::to_string(b * r) + "\n";
        else header += std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + "\n";
    }
    return header;
}
std::shared_ptr<const CubeLut> Lut(int mode = 0) { return CubeLut::Parse(Cube(mode)); }
std::shared_ptr<const CubeLut> SizedSwap(int size) {
    std::string text = "LUT_3D_SIZE " + std::to_string(size) + "\n";
    for (int b = 0; b < size; ++b) for (int g = 0; g < size; ++g) for (int r = 0; r < size; ++r)
        text += std::to_string(static_cast<float>(b) / (size - 1)) + " " +
            std::to_string(static_cast<float>(g) / (size - 1)) + " " + std::to_string(static_cast<float>(r) / (size - 1)) + "\n";
    return CubeLut::Parse(text);
}
bool LargerLattices() {
    for (int size : {33, 65}) {
        auto lut = SizedSwap(size); CHECK(lut && lut->GetSize() == size);
        for (int i : {0, 1, 9, 40, 127, 222, 254, 255}) {
            const auto sampled = lut->SampleBytes(static_cast<uint8_t>(i), 93, static_cast<uint8_t>(255 - i));
            CHECK(std::abs(sampled.r - (255 - i) / 255.0f) < 1e-6);
            CHECK(std::abs(sampled.g - 93 / 255.0f) < 1e-6 && std::abs(sampled.b - i / 255.0f) < 1e-6);
        }
    }
    return true;
}
bool Pixels(const AVFrame* frame, int b, int g, int r, int tolerance = 0, bool diagnostic = false) {
    if (!frame || frame->format != AV_PIX_FMT_BGR24) return false;
    for (int y = 0; y < frame->height; ++y) for (int x = 0; x < frame->width; ++x) {
        const auto* p = frame->data[0] + y * frame->linesize[0] + x * 3;
        if (std::abs(p[0] - b) > tolerance || std::abs(p[1] - g) > tolerance || std::abs(p[2] - r) > tolerance) {
            if (diagnostic) std::cerr << "Pixel " << x << "," << y << " BGR " << int(p[0]) << "," << int(p[1]) << "," << int(p[2])
                << " expected " << b << "," << g << "," << r << " tolerance " << tolerance << "\n";
            return false;
        }
    }
    return true;
}

bool Interpolation() {
    const auto lut = Lut(4); CHECK(lut);
    const auto sample = lut->Sample(.25f, .5f, .75f);
    CHECK(std::abs(sample.r - .125f) < 1e-6 && std::abs(sample.g - .375f) < 1e-6 && std::abs(sample.b - .1875f) < 1e-6);
    for (int i = 0; i < 256; i += 7) {
        const uint8_t r = static_cast<uint8_t>(i), g = static_cast<uint8_t>(255 - i), b = 93;
        const auto sampled = lut->SampleBytes(r, g, b);
        CHECK(std::abs(sampled.r - r * g / 65025.0f) < 1e-6);
        CHECK(std::abs(sampled.g - g * b / 65025.0f) < 1e-6);
    }
    return true;
}
bool StrictParsing() {
    std::string error = "old error";
    CHECK(CubeLut::Parse(Cube(0, "\xef\xbb\xbf# comment\r\nTitle \"A # title\" # inline comment\nLUT_3D_SIZE 2 # size\nDOMAIN_MIN +0 0e0 0\n"), &error));
    CHECK(error.empty());
    for (const std::string& header : {"LUT_3D_SIZE 2 garbage\n", "LUT_3D_SIZE 2.5\n", "LUT_3D_SIZE2\n",
            "LUT_3D_SIZE \n", "LUT_3D_SIZE 1\n", "LUT_3D_SIZE 66\n", "LUT_1D_SIZE 2\n",
            "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0 garbage\n", "LUT_3D_SIZE 2\nDOMAIN_MAX 0 1 1\n",
            "TITLEbare\nLUT_3D_SIZE 2\n", "TITLE \"x\" suffix\nLUT_3D_SIZE 2\n",
            "DOMAIN_MIN NaN 0 0\nLUT_3D_SIZE 2\n", "DOMAIN_MIN +-1 0 0\nLUT_3D_SIZE 2\n",
            "DOMAIN_MAX 1e400 1 1\nLUT_3D_SIZE 2\n",
            "LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n", "DOMAIN_MIN 0 0 0\nDOMAIN_MIN 0 0 0\nLUT_3D_SIZE 2\n"}) {
        CHECK(!CubeLut::Parse(Cube(0, header), &error)); CHECK(!error.empty());
    }
    CHECK(!CubeLut::Parse(Cube() + "1 1 1\n"));
    CHECK(!CubeLut::Parse("LUT_3D_SIZE 2\n0 0 0\n"));
    CHECK(!CubeLut::Parse(Cube() + std::string(1, '\0')));
    CHECK(!CubeLut::Parse(std::string(CubeLut::MaxPayloadBytes + 1, ' ')));
    return true;
}
bool ExtremeNumbers() {
    auto wide = CubeLut::Parse(Cube(0, "DOMAIN_MIN -3e38 -3e38 -3e38\nDOMAIN_MAX 3e38 3e38 3e38\nLUT_3D_SIZE 2\n"));
    CHECK(wide);
    auto c = wide->SampleBytes(10, 100, 200);
    CHECK(std::isfinite(c.r) && std::abs(c.r - .5f) < 1e-6 && std::abs(c.g - .5f) < 1e-6);
    auto tiny = CubeLut::Parse(Cube(0, "DOMAIN_MAX 1e-40 1e-40 1e-40\nLUT_3D_SIZE 2\n")); CHECK(tiny);
    c = tiny->SampleBytes(255, 0, 128); CHECK(c.r == 1 && c.g == 0 && c.b == 1);
    const auto ordinary = Lut();
    c = ordinary->Sample(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity());
    CHECK(c.r == 0 && c.g == 1 && c.b == 0);
    std::string big = "LUT_3D_SIZE 2\n";
    for (int i = 0; i < 8; ++i) big += "3.4028234663852886e38 -3.4028234663852886e38 3.4028234663852886e38\n";
    auto extreme = CubeLut::Parse(big); CHECK(extreme);
    c = extreme->SampleBytes(40, 100, 160); CHECK(std::isfinite(c.r) && std::isfinite(c.g) && c.r > 1 && c.g < 0);
    Frame frame; frame.Fill(40, 100, 160); CubeLutProcessor processor;
    CHECK(Pixels(processor.Process(frame.value, extreme.get(), 1), 255, 0, 255));
    return true;
}
bool OddStrideAndLifetime() {
    Frame frame; frame.Fill(40, 100, 160);
    std::vector<uint8_t> original(frame.value->data[0], frame.value->data[0] + frame.value->linesize[0] * frame.value->height);
    CubeLutProcessor processor; auto swap = Lut(1);
    auto* result = processor.Process(frame.value, swap.get(), 1);
    CHECK(result != frame.value && Pixels(result, 160, 100, 40));
    CHECK(std::memcmp(frame.value->data[0], original.data(), original.size()) == 0);
    CHECK(result->pts == 819 && result->pkt_dts == 810 && result->duration == 33 && result->time_base.den == 1000);
    CHECK(result->sample_aspect_ratio.num == 4 && result->best_effort_timestamp == 815);
    CHECK(result->color_range == AVCOL_RANGE_JPEG && result->colorspace == AVCOL_SPC_RGB);
    auto* retained = av_frame_clone(result); CHECK(retained);
    frame.Fill(5, 10, 15); CHECK(Pixels(processor.Process(frame.value, swap.get(), 1), 15, 10, 5));
    const bool unchanged = Pixels(retained, 160, 100, 40); av_frame_free(&retained); CHECK(unchanged);
    Frame resized(7, 5); resized.Fill(60, 70, 80); CHECK(Pixels(processor.Process(resized.value, swap.get(), .5f), 70, 70, 70));
    return true;
}
bool CombinedOrder() {
    Frame frame; frame.Fill(40, 100, 160);
    DesktopColorPipeline pipeline;
    pipeline.Configure({Look::Warm, false, Lut(1), 1}); CHECK(Pixels(pipeline.Process(frame.value), 174, 104, 28));
    pipeline.Configure({Look::Warm, false, Lut(1), .5f}); CHECK(Pixels(pipeline.Process(frame.value), 101, 104, 101));
    pipeline.Configure({Look::Cool, false, {}, 1}); CHECK(Pixels(pipeline.Process(frame.value), 54, 102, 148));
    pipeline.Configure({Look::Mono, false, Lut(1), 1});
    const int mono = (77 * 160 + 150 * 100 + 29 * 40 + 128) >> 8;
    CHECK(Pixels(pipeline.Process(frame.value), mono, mono, mono));
    CHECK(Pixels(frame.value, 40, 100, 160)); return true;
}
bool Bypass() {
    Frame frame; frame.Fill(40, 100, 160); DesktopColorPipeline pipeline;
    auto swap = Lut(1);
    pipeline.Configure({Look::Warm, true, swap, .5f}); CHECK(pipeline.Process(frame.value) == frame.value);
    CHECK(pipeline.GetLook() == Look::Original && pipeline.GetSettings().lut == swap);
    pipeline.Configure({Look::Original, false, swap, 0}); CHECK(pipeline.Process(frame.value) == frame.value);
    pipeline.Configure({Look::Original, false, {}, 1}); CHECK(pipeline.Process(frame.value) == frame.value);
    pipeline.Configure({Look::Original, false, swap, 1}); CHECK(Pixels(pipeline.Process(frame.value), 160, 100, 40));
    pipeline.Configure({Look::Original, false, swap, std::numeric_limits<float>::quiet_NaN()}); CHECK(pipeline.Process(frame.value) == frame.value);
    pipeline.Configure({Look::Original, false, swap, 7}); CHECK(Pixels(pipeline.Process(frame.value), 160, 100, 40));
    CHECK(pipeline.Process(nullptr) == nullptr); return true;
}
bool RgbConversion() {
    Frame rgb(5, 3, AV_PIX_FMT_RGB24); rgb.Fill(160, 100, 40);
    DesktopColorPipeline pipeline; pipeline.Configure({Look::Original, false, Lut(1), 1});
    CHECK(Pixels(pipeline.Process(rgb.value), 160, 100, 40));
    auto* output = pipeline.Process(rgb.value);
    CHECK(output->pts == rgb.value->pts && output->sample_aspect_ratio.num == 4);
    CHECK(output->colorspace == AVCOL_SPC_RGB && output->color_range == AVCOL_RANGE_JPEG);
    return true;
}
void Yuv(Frame& frame, uint8_t y, uint8_t u = 128, uint8_t v = 128) {
    for (int row = 0; row < frame.value->height; ++row) std::memset(frame.value->data[0] + row * frame.value->linesize[0], y, frame.value->width);
    for (int row = 0; row < frame.value->height / 2; ++row) {
        if (frame.value->format == AV_PIX_FMT_NV12) {
            auto* p = frame.value->data[1] + row * frame.value->linesize[1];
            for (int x = 0; x < frame.value->width; x += 2) { p[x] = u; p[x + 1] = v; }
        } else for (int plane = 1; plane < 3; ++plane)
            std::memset(frame.value->data[plane] + row * frame.value->linesize[plane], plane == 1 ? u : v, frame.value->width / 2);
    }
}
bool YuvRangeAndMatrix() {
    DesktopColorPipeline pipeline; pipeline.Configure({Look::Original, false, Lut(), 1});
    for (auto format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12}) {
        Frame frame(8, 4, format); frame.value->color_range = AVCOL_RANGE_MPEG; frame.value->colorspace = AVCOL_SPC_BT709;
        Yuv(frame, 16); CHECK(Pixels(pipeline.Process(frame.value), 0, 0, 0, 1, true));
        Yuv(frame, 235); CHECK(Pixels(pipeline.Process(frame.value), 255, 255, 255, 1, true));
        frame.value->color_range = AVCOL_RANGE_JPEG; Yuv(frame, 16); CHECK(Pixels(pipeline.Process(frame.value), 16, 16, 16, 1, true));
        frame.value->color_range = AVCOL_RANGE_MPEG; Yuv(frame, 80, 90, 240);
        CHECK(Pixels(pipeline.Process(frame.value), 0, 23, 255, 2, true));
        frame.value->colorspace = AVCOL_SPC_SMPTE170M;
        CHECK(Pixels(pipeline.Process(frame.value), 0, 0, 253, 3, true));
    }
    return true;
}
bool InvalidFrames() {
    Frame frame; frame.Fill(40, 100, 160);
    DesktopColorPipeline pipeline; pipeline.Configure({Look::Warm, false, Lut(1), 1});
    frame.value->color_trc = AVCOL_TRC_SMPTE2084; CHECK(pipeline.Process(frame.value) == frame.value);
    frame.value->color_trc = AVCOL_TRC_BT709; const int stride = frame.value->linesize[0];
    frame.value->linesize[0] = 1; CHECK(pipeline.Process(frame.value) == frame.value);
    frame.value->linesize[0] = -stride; CHECK(pipeline.Process(frame.value) == frame.value);
    frame.value->linesize[0] = stride; const int format = frame.value->format;
    frame.value->format = AV_PIX_FMT_D3D11; CHECK(pipeline.Process(frame.value) == frame.value);
    frame.value->format = format; frame.value->width = std::numeric_limits<int>::max(); CHECK(pipeline.Process(frame.value) == frame.value);
    frame.value->width = 5; auto* pixels = frame.value->data[0]; frame.value->data[0] = nullptr; CHECK(pipeline.Process(frame.value) == frame.value);
    frame.value->data[0] = pixels; CHECK(Pixels(pipeline.Process(frame.value), 174, 104, 28));
    return true;
}
bool LocalFiles() {
    // Only these explicitly named fixture files are created and removed.
    const auto path = std::filesystem::current_path() / std::filesystem::path(L"LUT-\u03b1-\u6d4b\u8bd5.cube");
    const auto oversize = std::filesystem::current_path() / "oversized-test.cube";
    struct Cleanup { std::filesystem::path a, b; ~Cleanup() { std::error_code e; std::filesystem::remove(a, e); std::filesystem::remove(b, e); } } cleanup{path, oversize};
    auto save = [&](const std::string& contents) { std::ofstream file(path, std::ios::binary); file << contents; return bool(file); };
    CHECK(save(Cube(1))); std::string error; auto lut = LoadCubeLutFile(path, error); CHECK(lut && error.empty());
    CHECK(lut->SampleBytes(40, 100, 160).r > .6f);
    CHECK(save("LUT_3D_SIZE 2.5\n")); CHECK(!LoadCubeLutFile(path, error) && !error.empty());
    CHECK(save("")); CHECK(!LoadCubeLutFile(path, error));
    {
        std::ofstream file(oversize, std::ios::binary); file.seekp(CubeLut::MaxPayloadBytes); file.put('x'); CHECK(file.good());
    }
    CHECK(!LoadCubeLutFile(oversize, error)); CHECK(!LoadCubeLutFile("missing-lut-test.cube", error));
    Frame frame; frame.Fill(40, 100, 160); DesktopColorPipeline pipeline;
    pipeline.Configure({Look::Original, false, lut, 1}); CHECK(Pixels(pipeline.Process(frame.value), 160, 100, 40));
    return true;
}
bool CommonConsumersAndSourceSwitch() {
    Frame mobile(8, 4); mobile.Fill(40, 100, 160);
    Frame camera(8, 4); camera.Fill(20, 60, 100);
    DesktopColorPipeline pipeline; pipeline.Configure({Look::Original, false, Lut(1), 1});
    PrismForge::Input::SessionGate gate;
    Video::DirectShowScaler ds(8, 4); Video::GuiPreviewScaler preview(8, 4);
    bool correct = true; int deliveries = 0;
    auto deliver = [&](uint64_t token, Frame& frame, int expectedB, int expectedR) {
        return gate.Deliver(token, [&] {
            auto* result = pipeline.Process(frame.value); int width = 0, height = 0;
            const auto* packed = ds.Process(result, width, height);
            correct &= packed && width == 8 && height == 4 && packed[0] == expectedB && packed[2] == expectedR;
            const auto* shown = preview.Process(result, width, height); // Existing preview scaler consumes BGR24.
            correct &= shown && shown[0] == expectedB && shown[2] == expectedR;
            correct &= Pixels(result, expectedB, frame.value->data[0][1], expectedR); // snapshot's exact frame
            ++deliveries;
        });
    };
    const auto old = gate.Current(); CHECK(deliver(old, mobile, 160, 40));
    const auto current = gate.Advance(); CHECK(!deliver(old, mobile, 160, 40));
    CHECK(deliver(current, camera, 100, 20)); CHECK(correct && deliveries == 2); return true;
}
bool ConcurrentConfiguration() {
    DesktopColorPipeline pipeline; Frame frame(64, 16); frame.Fill(40, 100, 160);
    auto black = Lut(2), white = Lut(3);
    pipeline.Configure({Look::Original, false, black, 1});
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 1000; ++i)
            pipeline.Configure({Look::Original, i % 3 == 0, i % 2 ? white : black, 1});
    });
    start = true; bool valid = true;
    for (int i = 0; i < 1000; ++i) {
        auto* result = pipeline.Process(frame.value);
        valid &= result == frame.value || Pixels(result, 0, 0, 0) || Pixels(result, 255, 255, 255);
    }
    writer.join(); CHECK(valid);
    pipeline.Configure({Look::Warm, false, Lut(1), 1}); CHECK(Pixels(pipeline.Process(frame.value), 174, 104, 28)); return true;
}
bool CurrentFrameAndResize() {
    DesktopColorPipeline pipeline; pipeline.Configure({Look::Original, false, Lut(1), 1});
    for (int i = 0; i < 32; ++i) {
        Frame frame(i % 2 ? 7 : 11, i % 3 ? 5 : 9); frame.Fill(static_cast<uint8_t>(i), 90, static_cast<uint8_t>(200 - i));
        frame.value->pts = i;
        auto* result = pipeline.Process(frame.value); CHECK(Pixels(result, 200 - i, 90, i) && result->pts == i);
    }
    return true;
}
bool Benchmark() {
    DesktopColorPipeline pipeline;
    for (int size : {2, 33}) {
    pipeline.Configure({Look::Warm, false, size == 2 ? Lut(1) : SizedSwap(size), 1});
    for (const auto dimensions : {std::pair<int, int>{1280, 720}, {1920, 1080}}) {
        Frame frame(dimensions.first, dimensions.second);
        std::vector<double> times;
        for (int i = 0; i < 19; ++i) {
            for (int y = 0; y < frame.value->height; ++y) for (int x = 0; x < frame.value->width; ++x) {
                auto* p = frame.value->data[0] + y * frame.value->linesize[0] + x * 3;
                p[0] = static_cast<uint8_t>((i * 3 + x * 11 + y * 7) % 256);
                p[1] = static_cast<uint8_t>((53 + x * 5 + y * 3 + i * 7) % 256);
                p[2] = static_cast<uint8_t>((221 + x * 7 + y * 13 - i * 2) % 256);
            }
            frame.value->pts = i;
            const auto start = std::chrono::steady_clock::now(); auto* output = pipeline.Process(frame.value);
            const auto stop = std::chrono::steady_clock::now();
            CHECK(output && output != frame.value && output->pts == i);
            for (int y : {0, frame.value->height - 1}) for (int x : {0, frame.value->width - 1}) {
                const auto* p = output->data[0] + y * output->linesize[0] + x * 3;
                const auto* input = frame.value->data[0] + y * frame.value->linesize[0] + x * 3;
                CHECK(p[0] == std::min(input[2] + 14, 255) && p[1] == std::min(input[1] + 4, 255) &&
                    p[2] == std::max(input[0] - 12, 0));
            }
            if (i >= 3) times.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        }
        std::sort(times.begin(), times.end());
        std::cout << "BENCH CPU Warm+" << size << "^3 LUT " << dimensions.first << "x" << dimensions.second
            << ": spatially varied changing input, 3 warmup + 16 samples, expected corner pixels each frame, p50=" << times[8]
            << " ms p95=" << times[15] << " ms; processing only, excludes capture/consumers\n";
    }
    }
    return true;
}
int main() {
    const std::pair<const char*, bool(*)()> tests[]{
        {"independent trilinear/channel-order pixels", Interpolation}, {"33/65-node lattice sampling", LargerLattices},
        {"strict bounded locale-independent parser", StrictParsing},
        {"extreme domains/entries and NaN-safe indexing", ExtremeNumbers}, {"odd stride, immutability, metadata and retained clone", OddStrideAndLifetime},
        {"built-in look then LUT with intensity", CombinedOrder}, {"whole-chain bypass and restore", Bypass},
        {"Original RGB conversion and timing", RgbConversion}, {"YUV/NV12 range and matrix changes", YuvRangeAndMatrix},
        {"invalid/HDR frames and recovery", InvalidFrames}, {"bounded Unicode local-file loading", LocalFiles},
        {"mobile/system generations and shared consumers", CommonConsumersAndSourceSwitch},
        {"concurrent immutable configuration replacement", ConcurrentConfiguration}, {"changing current frames and resize", CurrentFrameAndResize}
    };
    int passed = 0;
    for (const auto& test : tests) {
        if (test.second()) { ++passed; std::cout << "PASS " << test.first << "\n"; }
        else std::cout << "FAIL " << test.first << "\n";
    }
    std::cout << "SUMMARY " << passed << "/" << std::size(tests) << " PASS\n";
    if (passed != std::size(tests)) return 1;
    return Benchmark() ? 0 : 1;
}
