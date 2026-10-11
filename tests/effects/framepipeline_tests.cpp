#include "effects/framepipeline.h"
#include "video/guipreviewscaler.h"
#include "video/directshowscaler.h"
#include "video/1_1scaler.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace PrismForge::Effects;

namespace {

// Helper to allocate and populate test AVFrames
AVFrame* AllocTestFrame(int width, int height, AVPixelFormat format, uint8_t r, uint8_t g, uint8_t b, int align = 32) {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->width = width;
    f->height = height;
    f->format = format;
    if (av_frame_get_buffer(f, align) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    f->pts = 100000;
    f->pkt_dts = 99000;
    f->duration = 33333;
    f->time_base = AVRational{1, 1000000};

    if (format == AV_PIX_FMT_RGB24) {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = f->data[0] + y * f->linesize[0];
            for (int x = 0; x < width; ++x) {
                row[x * 3 + 0] = r;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = b;
            }
        }
    } else if (format == AV_PIX_FMT_BGR24) {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = f->data[0] + y * f->linesize[0];
            for (int x = 0; x < width; ++x) {
                row[x * 3 + 0] = b;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = r;
            }
        }
    } else if (format == AV_PIX_FMT_YUV420P) {
        // Standard Rec. 601 RGB -> YUV conversion
        const uint8_t y_val = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
        const uint8_t u_val = static_cast<uint8_t>(((-43 * (int)r - 84 * (int)g + 128 * (int)b) >> 8) + 128);
        const uint8_t v_val = static_cast<uint8_t>(((128 * (int)r - 107 * (int)g - 21 * (int)b) >> 8) + 128);

        for (int y = 0; y < height; ++y) {
            std::memset(f->data[0] + y * f->linesize[0], y_val, width);
        }
        const int uv_w = (width + 1) / 2;
        const int uv_h = (height + 1) / 2;
        for (int y = 0; y < uv_h; ++y) {
            std::memset(f->data[1] + y * f->linesize[1], u_val, uv_w);
            std::memset(f->data[2] + y * f->linesize[2], v_val, uv_w);
        }
    }
    return f;
}

AVFrame* AllocRawYuvFrame(int width, int height, uint8_t y_val, uint8_t u_val, uint8_t v_val, int align = 32) {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->width = width;
    f->height = height;
    f->format = AV_PIX_FMT_YUV420P;
    if (av_frame_get_buffer(f, align) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    f->pts = 100000;
    f->pkt_dts = 99000;
    f->duration = 33333;
    f->time_base = AVRational{1, 1000000};
    for (int y = 0; y < height; ++y) {
        std::memset(f->data[0] + y * f->linesize[0], y_val, width);
    }
    const int uv_w = (width + 1) / 2;
    const int uv_h = (height + 1) / 2;
    for (int y = 0; y < uv_h; ++y) {
        std::memset(f->data[1] + y * f->linesize[1], u_val, uv_w);
        std::memset(f->data[2] + y * f->linesize[2], v_val, uv_w);
    }
    return f;
}

std::vector<uint8_t> SnapshotFrameData(const AVFrame* f) {
    std::vector<uint8_t> buffer;
    if (!f) return buffer;
    for (int p = 0; p < AV_NUM_DATA_POINTERS && f->data[p]; ++p) {
        const int h = (p == 0) ? f->height : (f->height + 1) / 2;
        const int size = f->linesize[p] * h;
        if (size > 0) {
            const size_t prev = buffer.size();
            buffer.resize(prev + size);
            std::memcpy(buffer.data() + prev, f->data[p], size);
        }
    }
    return buffer;
}

void PrintTestHeader(const char* name) {
    std::cout << "[TEST] Running " << name << " ... ";
}

void PrintTestPassed() {
    std::cout << "PASSED" << std::endl;
}

} // anonymous namespace

// 1. Exact-pointer bypass & input immutability
void TestExactPointerBypass() {
    PrintTestHeader("TestExactPointerBypass");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Original);
    assert(pipeline.GetLook() == Look::Original);

    AVFrame* input = AllocTestFrame(640, 480, AV_PIX_FMT_YUV420P, 180, 100, 50);
    assert(input != nullptr);
    const auto snapshotBefore = SnapshotFrameData(input);

    AVFrame* output = pipeline.Process(input);

    // Exact pointer identity must be preserved in bypass mode
    assert(output == input);

    // Input data must remain completely untouched
    const auto snapshotAfter = SnapshotFrameData(input);
    assert(snapshotBefore == snapshotAfter);

    av_frame_free(&input);
    PrintTestPassed();
}

// 2. Input immutability under all active looks
void TestInputImmutability() {
    PrintTestHeader("TestInputImmutability");
    FramePipeline pipeline;

    const Look activeLooks[] = { Look::Warm, Look::Cool, Look::Mono };
    for (Look look : activeLooks) {
        pipeline.SetLook(look);
        AVFrame* input = AllocTestFrame(320, 240, AV_PIX_FMT_YUV420P, 120, 200, 80);
        assert(input != nullptr);
        const auto snapshotBefore = SnapshotFrameData(input);

        AVFrame* output = pipeline.Process(input);
        assert(output != nullptr);
        assert(output != input);

        // Caller-owned input must not be altered, freed, or retained
        const auto snapshotAfter = SnapshotFrameData(input);
        assert(snapshotBefore == snapshotAfter);

        av_frame_free(&input);
    }
    PrintTestPassed();
}

// 3. Active output ownership, format, dimensions & timing metadata
void TestActiveOutputOwnership() {
    PrintTestHeader("TestActiveOutputOwnership");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Mono);

    AVFrame* input = AllocTestFrame(800, 600, AV_PIX_FMT_YUV420P, 100, 150, 200);
    input->pts = 424242;
    input->pkt_dts = 424000;
    input->duration = 16667;
    input->time_base = AVRational{1, 90000};

    AVFrame* output = pipeline.Process(input);
    assert(output != nullptr);
    assert(output != input);
    assert(output->format == AV_PIX_FMT_BGR24);
    assert(output->width == 800);
    assert(output->height == 600);
    assert(output->pts == 424242);
    assert(output->pkt_dts == 424000);
    assert(output->duration == 16667);
    assert(output->time_base.num == 1 && output->time_base.den == 90000);

    // Output colour metadata must be explicitly set for downstream consumers
    assert(output->color_range == AVCOL_RANGE_JPEG);
    assert(output->colorspace == AVCOL_SPC_RGB);
    assert(output->color_primaries == input->color_primaries);
    assert(output->color_trc == input->color_trc);

    // Output pointer remains valid until the next Process call or destruction
    assert(output->data[0] != nullptr);
    assert(output->linesize[0] >= 800 * 3);

    av_frame_free(&input);
    PrintTestPassed();
}

// 4. Odd dimensions and padded row strides
void TestOddDimensionsAndPaddedStrides() {
    PrintTestHeader("TestOddDimensionsAndPaddedStrides");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // Width 319, Height 241 (odd dimensions with 64-byte alignment padding)
    // Input R=160, G=140, B=120
    AVFrame* input = AllocTestFrame(319, 241, AV_PIX_FMT_YUV420P, 160, 140, 120, 64);
    assert(input != nullptr);
    assert(input->linesize[0] >= 319);

    AVFrame* output = pipeline.Process(input);
    assert(output != nullptr);
    assert(output->width == 319);
    assert(output->height == 241);
    assert(output->format == AV_PIX_FMT_BGR24);
    assert(output->linesize[0] >= 319 * 3);

    // Warm grade on (B=120, G=140, R=160) yields expected values:
    // B' = max(0, 120 - 12) = 108; G' = min(255, 140 + 4) = 144; R' = min(255, 160 + 14) = 174
    // Assert first row pixel (0, 0)
    const uint8_t* firstRow = output->data[0];
    assert(firstRow[0] == 110); // Blue: 122 - 12 = 110
    assert(firstRow[1] == 148); // Green: 144 + 4 = 148
    assert(firstRow[2] == 179); // Red: 165 + 14 = 179

    // Assert boundary pixel (318, 240) on padded last row
    const uint8_t* lastRow = output->data[0] + (241 - 1) * output->linesize[0];
    const uint8_t b = lastRow[(319 - 1) * 3 + 0];
    const uint8_t g = lastRow[(319 - 1) * 3 + 1];
    const uint8_t r = lastRow[(319 - 1) * 3 + 2];
    assert(b == 110);
    assert(g == 148);
    assert(r == 179);

    av_frame_free(&input);

    // Also assert exact pixel values on an odd-dimension BGR24 input with padded stride
    AVFrame* bgrOdd = AllocTestFrame(319, 241, AV_PIX_FMT_BGR24, 160, 140, 120, 64);
    AVFrame* bgrOddOut = pipeline.Process(bgrOdd);
    assert(bgrOddOut != nullptr);
    const uint8_t* bgrLastRow = bgrOddOut->data[0] + (241 - 1) * bgrOddOut->linesize[0];
    assert(bgrLastRow[(319 - 1) * 3 + 0] == 108); // B: 120 - 12 = 108
    assert(bgrLastRow[(319 - 1) * 3 + 1] == 144); // G: 140 + 4 = 144
    assert(bgrLastRow[(319 - 1) * 3 + 2] == 174); // R: 160 + 14 = 174
    av_frame_free(&bgrOdd);
    PrintTestPassed();
}

// 5. RGB vs BGR channel order verification
void TestRgbBgrChannelOrder() {
    PrintTestHeader("TestRgbBgrChannelOrder");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // Pure Red frame in RGB24: R=255, G=0, B=0
    AVFrame* redInput = AllocTestFrame(64, 64, AV_PIX_FMT_RGB24, 255, 0, 0);
    AVFrame* redOutput = pipeline.Process(redInput);
    assert(redOutput != nullptr);
    // In BGR24: pixel[0]=Blue, pixel[1]=Green, pixel[2]=Red
    const uint8_t* redPx = redOutput->data[0];
    assert(redPx[0] == 0);          // Blue must be 0
    assert(redPx[2] == 255);        // Red must be dominant (clamped to 255)
    assert(redPx[2] > redPx[0]);

    // Pure Blue frame in RGB24: R=0, G=0, B=255
    AVFrame* blueInput = AllocTestFrame(64, 64, AV_PIX_FMT_RGB24, 0, 0, 255);
    AVFrame* blueOutput = pipeline.Process(blueInput);
    assert(blueOutput != nullptr);
    const uint8_t* bluePx = blueOutput->data[0];
    assert(bluePx[0] > bluePx[2]);  // Blue must be dominant in index 0
    assert(bluePx[0] >= 200);       // Blue channel is strongly active
    assert(bluePx[2] <= 20);        // Red channel remains bounded near 0

    av_frame_free(&redInput);
    av_frame_free(&blueInput);
    PrintTestPassed();
}

// 6. Mono channel equality verification
void TestMonoChannelEquality() {
    PrintTestHeader("TestMonoChannelEquality");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Mono);

    AVFrame* input = AllocTestFrame(128, 128, AV_PIX_FMT_RGB24, 75, 150, 225);
    AVFrame* output = pipeline.Process(input);
    assert(output != nullptr);
    assert(output->format == AV_PIX_FMT_BGR24);

    const int w = output->width;
    const int h = output->height;
    const int stride = output->linesize[0];
    const uint8_t* base = output->data[0];

    for (int y = 0; y < h; ++y) {
        const uint8_t* row = base + y * stride;
        for (int x = 0; x < w; ++x) {
            const uint8_t b = row[x * 3 + 0];
            const uint8_t g = row[x * 3 + 1];
            const uint8_t r = row[x * 3 + 2];
            // Mono requires strict channel equality across all three channels
            assert(b == g);
            assert(g == r);
        }
    }

    av_frame_free(&input);
    PrintTestPassed();
}

// 7. Bounded Warm / Cool changes and clamping
void TestBoundedWarmCoolChanges() {
    PrintTestHeader("TestBoundedWarmCoolChanges");
    FramePipeline pipeline;

    // Neutral gray: 128, 128, 128 in BGR24
    AVFrame* grayInput = AllocTestFrame(64, 64, AV_PIX_FMT_BGR24, 128, 128, 128);

    // Warm grade
    pipeline.SetLook(Look::Warm);
    AVFrame* warmOutput = pipeline.Process(grayInput);
    assert(warmOutput != nullptr);
    const uint8_t* warmPx = warmOutput->data[0];
    assert(warmPx[0] == 116); // Expected Blue
    assert(warmPx[1] == 132); // Expected Green
    assert(warmPx[2] == 142); // Expected Red

    // Cool grade
    pipeline.SetLook(Look::Cool);
    AVFrame* coolOutput = pipeline.Process(grayInput);
    assert(coolOutput != nullptr);
    const uint8_t* coolPx = coolOutput->data[0];
    assert(coolPx[0] == 142); // Expected Blue
    assert(coolPx[1] == 130); // Expected Green
    assert(coolPx[2] == 116); // Expected Red

    // Clamping boundary tests: pure black (0,0,0) and pure white (255,255,255)
    AVFrame* blackInput = AllocTestFrame(32, 32, AV_PIX_FMT_BGR24, 0, 0, 0);
    pipeline.SetLook(Look::Warm);
    AVFrame* blackWarm = pipeline.Process(blackInput);
    assert(blackWarm != nullptr);
    assert(blackWarm->data[0][0] == 0);  // B: max(0, 0 - 12) = 0
    assert(blackWarm->data[0][1] == 4);  // G: min(255, 0 + 4) = 4
    assert(blackWarm->data[0][2] == 14); // R: min(255, 0 + 14) = 14

    pipeline.SetLook(Look::Cool);
    AVFrame* blackCool = pipeline.Process(blackInput);
    assert(blackCool != nullptr);
    assert(blackCool->data[0][0] == 14); // B: min(255, 0 + 14) = 14
    assert(blackCool->data[0][1] == 2);  // G: min(255, 0 + 2) = 2
    assert(blackCool->data[0][2] == 0);  // R: max(0, 0 - 12) = 0

    AVFrame* whiteInput = AllocTestFrame(32, 32, AV_PIX_FMT_BGR24, 255, 255, 255);
    pipeline.SetLook(Look::Warm);
    AVFrame* whiteWarm = pipeline.Process(whiteInput);
    assert(whiteWarm != nullptr);
    assert(whiteWarm->data[0][0] == 243); // B: max(0, 255 - 12) = 243
    assert(whiteWarm->data[0][1] == 255); // G: min(255, 255 + 4) = 255
    assert(whiteWarm->data[0][2] == 255); // R: min(255, 255 + 14) = 255

    pipeline.SetLook(Look::Cool);
    AVFrame* whiteCool = pipeline.Process(whiteInput);
    assert(whiteCool != nullptr);
    assert(whiteCool->data[0][0] == 255); // B: min(255, 255 + 14) = 255
    assert(whiteCool->data[0][1] == 255); // G: min(255, 255 + 2) = 255
    assert(whiteCool->data[0][2] == 243); // R: max(0, 255 - 12) = 243

    av_frame_free(&grayInput);
    av_frame_free(&blackInput);
    av_frame_free(&whiteInput);
    PrintTestPassed();
}

// 8. Colour-range conversion (full vs limited range)
void TestColourRangeConversion() {
    PrintTestHeader("TestColourRangeConversion");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // Test case 1: Identical raw YUV values (Y=64, U=128, V=128)
    // Full range (JPEG / PC range [0, 255])
    AVFrame* fullRangeFrame = AllocRawYuvFrame(160, 120, 64, 128, 128);
    fullRangeFrame->color_range = AVCOL_RANGE_JPEG;
    fullRangeFrame->colorspace = AVCOL_SPC_BT709;
    AVFrame* outFull = pipeline.Process(fullRangeFrame);
    assert(outFull != nullptr);
    assert(outFull->format == AV_PIX_FMT_BGR24);
    assert(outFull->color_range == AVCOL_RANGE_JPEG);

    // Save full range pixels before next Process call reuses internal buffer
    const uint8_t fullB = outFull->data[0][0];
    const uint8_t fullG = outFull->data[0][1];
    const uint8_t fullR = outFull->data[0][2];

    // Limited range (MPEG / TV range [16, 235])
    AVFrame* limitedRangeFrame = AllocRawYuvFrame(160, 120, 64, 128, 128);
    limitedRangeFrame->color_range = AVCOL_RANGE_MPEG;
    limitedRangeFrame->colorspace = AVCOL_SPC_BT709;
    AVFrame* outLimited = pipeline.Process(limitedRangeFrame);
    assert(outLimited != nullptr);
    assert(outLimited->format == AV_PIX_FMT_BGR24);
    assert(outLimited->color_range == AVCOL_RANGE_JPEG);

    const uint8_t limB = outLimited->data[0][0];
    const uint8_t limG = outLimited->data[0][1];
    const uint8_t limR = outLimited->data[0][2];

    // Assert actual pixel differences: full range pixel values are strictly higher than limited range
    assert(fullB > limB); // Blue
    assert(fullG > limG); // Green
    assert(fullR > limR); // Red

    const int diffB = static_cast<int>(fullB) - static_cast<int>(limB);
    const int diffG = static_cast<int>(fullG) - static_cast<int>(limG);
    const int diffR = static_cast<int>(fullR) - static_cast<int>(limR);
    assert(diffB >= 5 && diffB <= 12);
    assert(diffG >= 5 && diffG <= 12);
    assert(diffR >= 5 && diffR <= 12);

    // Assert expected pixel values within integer rounding tolerance
    assert(std::abs(static_cast<int>(fullB) - 52) <= 2);
    assert(std::abs(static_cast<int>(fullG) - 68) <= 2);
    assert(std::abs(static_cast<int>(fullR) - 78) <= 2);

    assert(std::abs(static_cast<int>(limB) - 44) <= 2);
    assert(std::abs(static_cast<int>(limG) - 60) <= 2);
    assert(std::abs(static_cast<int>(limR) - 70) <= 2);

    // Test case 2: Nominal black (Y=16, U=128, V=128)
    AVFrame* fullBlack = AllocRawYuvFrame(160, 120, 16, 128, 128);
    fullBlack->color_range = AVCOL_RANGE_JPEG;
    fullBlack->colorspace = AVCOL_SPC_BT709;
    AVFrame* outFullBlack = pipeline.Process(fullBlack);
    assert(outFullBlack != nullptr);
    const uint8_t fullBlackB = outFullBlack->data[0][0];
    const uint8_t fullBlackG = outFullBlack->data[0][1];
    const uint8_t fullBlackR = outFullBlack->data[0][2];

    AVFrame* limitedBlack = AllocRawYuvFrame(160, 120, 16, 128, 128);
    limitedBlack->color_range = AVCOL_RANGE_MPEG;
    limitedBlack->colorspace = AVCOL_SPC_BT709;
    AVFrame* outLimitedBlack = pipeline.Process(limitedBlack);
    assert(outLimitedBlack != nullptr);
    const uint8_t limBlackB = outLimitedBlack->data[0][0];
    const uint8_t limBlackG = outLimitedBlack->data[0][1];
    const uint8_t limBlackR = outLimitedBlack->data[0][2];

    // In MPEG limited range, Y=16 is absolute black (0,0,0) -> Warm gives B=0, G=4, R=14
    // In JPEG full range, Y=16 is dark gray (~16,16,16) -> Warm gives B=4, G=20, R=30
    assert(limBlackB == 0);
    assert(limBlackG == 4);
    assert(limBlackR == 14);

    assert(fullBlackR > limBlackR);
    assert(fullBlackG > limBlackG);
    assert(fullBlackB > limBlackB);

    av_frame_free(&fullRangeFrame);
    av_frame_free(&limitedRangeFrame);
    av_frame_free(&fullBlack);
    av_frame_free(&limitedBlack);
    PrintTestPassed();
}

// 9. Format and resolution transitions
void TestFormatAndResolutionTransitions() {
    PrintTestHeader("TestFormatAndResolutionTransitions");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Mono);

    struct Step {
        int w;
        int h;
        AVPixelFormat fmt;
    } steps[] = {
        { 640, 480, AV_PIX_FMT_YUV420P },
        { 1280, 720, AV_PIX_FMT_YUV420P },
        { 1920, 1080, AV_PIX_FMT_RGB24 },
        { 320, 240, AV_PIX_FMT_BGR24 },
        { 640, 480, AV_PIX_FMT_YUV420P }
    };

    for (const auto& step : steps) {
        AVFrame* f = AllocTestFrame(step.w, step.h, step.fmt, 100, 100, 100);
        AVFrame* out = pipeline.Process(f);
        assert(out != nullptr);
        assert(out->width == step.w);
        assert(out->height == step.h);
        assert(out->format == AV_PIX_FMT_BGR24);
        av_frame_free(&f);
    }
    PrintTestPassed();
}

// 10. Null and invalid frames handling
void TestNullAndInvalidFrames() {
    PrintTestHeader("TestNullAndInvalidFrames");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // Null input returns null
    assert(pipeline.Process(nullptr) == nullptr);

    // Invalid dimensions return input safely without crash
    AVFrame* invalidDim = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 0, 0, 0);
    invalidDim->width = -1;
    assert(pipeline.Process(invalidDim) == invalidDim);
    invalidDim->width = 64; // restore for safe free
    av_frame_free(&invalidDim);

    // Invalid format returns input safely
    AVFrame* invalidFmt = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 0, 0, 0);
    invalidFmt->format = AV_PIX_FMT_NONE;
    assert(pipeline.Process(invalidFmt) == invalidFmt);
    invalidFmt->format = AV_PIX_FMT_YUV420P; // restore for safe free
    av_frame_free(&invalidFmt);

    PrintTestPassed();
}

// 11. Reject unsupported pixel format before swscale
void TestRejectUnsupportedPixelFormat() {
    PrintTestHeader("TestRejectUnsupportedPixelFormat");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // Format AV_PIX_FMT_NONE
    AVFrame* fNone = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 100, 100, 100);
    fNone->format = AV_PIX_FMT_NONE;
    AVFrame* outNone = pipeline.Process(fNone);
    assert(outNone == fNone); // Safely returns original input
    fNone->format = AV_PIX_FMT_YUV420P;
    av_frame_free(&fNone);

    // Arbitrary unsupported format ID (99999)
    AVFrame* fInvalid = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 100, 100, 100);
    fInvalid->format = 99999;
    AVFrame* outInvalid = pipeline.Process(fInvalid);
    assert(outInvalid == fInvalid); // Safely returns original input
    fInvalid->format = AV_PIX_FMT_YUV420P;
    av_frame_free(&fInvalid);

    PrintTestPassed();
}

// 12. Reject missing required planes before swscale
void TestRejectMissingRequiredPlanes() {
    PrintTestHeader("TestRejectMissingRequiredPlanes");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // YUV420P requires 3 planes (Y=0, U=1, V=2)
    AVFrame* f = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 120, 120, 120);

    // 1. Missing plane 1 (U plane null)
    uint8_t* const origU = f->data[1];
    f->data[1] = nullptr;
    AVFrame* outU = pipeline.Process(f);
    assert(outU == f); // Safely returns original input untouched
    f->data[1] = origU;

    // 2. Missing plane 2 (V plane null)
    uint8_t* const origV = f->data[2];
    f->data[2] = nullptr;
    AVFrame* outV = pipeline.Process(f);
    assert(outV == f);
    f->data[2] = origV;

    // 3. Missing plane 0 (Y plane null)
    uint8_t* const origY = f->data[0];
    f->data[0] = nullptr;
    AVFrame* outY = pipeline.Process(f);
    assert(outY == f);
    f->data[0] = origY;

    av_frame_free(&f);
    PrintTestPassed();
}

// 13. Reject invalid strides before swscale
void TestRejectInvalidStrides() {
    PrintTestHeader("TestRejectInvalidStrides");
    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    // 1. Zero stride on YUV420P
    AVFrame* fYuv = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 100, 100, 100);
    const int origStride0 = fYuv->linesize[0];
    fYuv->linesize[0] = 0;
    AVFrame* outZero = pipeline.Process(fYuv);
    assert(outZero == fYuv); // Safely returns input
    fYuv->linesize[0] = origStride0;

    // 2. Negative stride on YUV420P
    fYuv->linesize[0] = -64;
    AVFrame* outNeg = pipeline.Process(fYuv);
    assert(outNeg == fYuv);
    fYuv->linesize[0] = origStride0;

    // 3. Insufficient stride (smaller than plane width)
    fYuv->linesize[0] = 32; // Width is 64, so 32 is insufficient
    AVFrame* outShort = pipeline.Process(fYuv);
    assert(outShort == fYuv);
    fYuv->linesize[0] = origStride0;

    // 4. Insufficient chroma stride
    const int origStride1 = fYuv->linesize[1];
    fYuv->linesize[1] = 10; // (64+1)/2 = 32 required
    AVFrame* outChromaShort = pipeline.Process(fYuv);
    assert(outChromaShort == fYuv);
    fYuv->linesize[1] = origStride1;

    av_frame_free(&fYuv);

    // 5. Insufficient stride on RGB24 (width 64 requires 64 * 3 = 192 bytes)
    AVFrame* fRgb = AllocTestFrame(64, 64, AV_PIX_FMT_RGB24, 100, 100, 100);
    const int origRgbStride = fRgb->linesize[0];
    fRgb->linesize[0] = 100; // less than 192 bytes
    AVFrame* outRgbShort = pipeline.Process(fRgb);
    assert(outRgbShort == fRgb);
    fRgb->linesize[0] = origRgbStride;
    av_frame_free(&fRgb);

    PrintTestPassed();
}

// 11. Invalid setting handling
void TestInvalidSetting() {
    PrintTestHeader("TestInvalidSetting");
    FramePipeline pipeline;
    pipeline.SetLook(static_cast<Look>(9999));

    // Invalid Look must fall back to Look::Original
    assert(pipeline.GetLook() == Look::Original);

    AVFrame* input = AllocTestFrame(64, 64, AV_PIX_FMT_YUV420P, 50, 50, 50);
    AVFrame* out = pipeline.Process(input);
    assert(out == input);

    av_frame_free(&input);
    PrintTestPassed();
}

// 12. Repeated calls and memory stability
void TestRepeatedCalls() {
    PrintTestHeader("TestRepeatedCalls");
    FramePipeline pipeline;

    const Look sequence[] = { Look::Warm, Look::Cool, Look::Mono, Look::Original };
    AVFrame* f = AllocTestFrame(320, 240, AV_PIX_FMT_YUV420P, 120, 130, 140);

    for (int i = 0; i < 160; ++i) {
        pipeline.SetLook(sequence[i % 4]);
        AVFrame* out = pipeline.Process(f);
        assert(out != nullptr);
        if (pipeline.GetLook() == Look::Original) {
            assert(out == f);
        } else {
            assert(out != f);
            assert(out->format == AV_PIX_FMT_BGR24);
        }
    }

    av_frame_free(&f);
    PrintTestPassed();
}

// 13. Destruction and recreation
void TestDestructionAndRecreation() {
    PrintTestHeader("TestDestructionAndRecreation");
    for (int i = 0; i < 5; ++i) {
        auto pipeline = std::make_unique<FramePipeline>();
        pipeline->SetLook(Look::Warm);
        AVFrame* f = AllocTestFrame(160, 120, AV_PIX_FMT_YUV420P, 80, 80, 80);
        AVFrame* out = pipeline->Process(f);
        assert(out != nullptr);
        av_frame_free(&f);
        pipeline.reset(); // Destroy pipeline instance
    }
    PrintTestPassed();
}

// 14. Synthetic 1080p CPU benchmark
void BenchmarkCpuProcessingTimings() {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "[BENCHMARK] Label: CPU processing timings only" << std::endl;
    std::cout << "            (not end-to-end camera FPS or GPU benchmark)" << std::endl;
    std::cout << "========================================================" << std::endl;

    FramePipeline pipeline;
    pipeline.SetLook(Look::Warm);

    const int w = 1920;
    const int h = 1080;
    AVFrame* f1080p = AllocTestFrame(w, h, AV_PIX_FMT_YUV420P, 128, 128, 128);
    assert(f1080p != nullptr);

    // Warm-up run
    pipeline.Process(f1080p);

    const int iterations = 100;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        AVFrame* out = pipeline.Process(f1080p);
        assert(out != nullptr);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double avgMs = totalMs / iterations;
    const double estFps = 1000.0 / avgMs;

    std::cout << "1080p YUV420P -> BGR24 (Warm grade, CPU swscale + LUT):" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time per frame: " << avgMs << " ms" << std::endl;
    std::cout << "  Throughput estimate:    " << estFps << " FPS (CPU only)" << std::endl;

    av_frame_free(&f1080p);
}

// 15. Explicitly report skipped checks
void ReportSkippedChecks() {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "[INFO] Explicit report of skipped checks (per contract scope):" << std::endl;
    std::cout << "  1. Skipped: GPU acceleration / Direct3D 11 interop (initial backend is CPU-only)." << std::endl;
    std::cout << "  2. Skipped: Face-tracking bounding boxes / facial landmark relighting (Warm/Cool are global grades)." << std::endl;
    std::cout << "  3. Skipped: Background matting / neural segmentation (assigned to future stages)." << std::endl;
    std::cout << "  4. Skipped: Multi-threaded parallel Process calls (contract specifies single decode thread)." << std::endl;
    std::cout << "========================================================\n" << std::endl;
}


void TestColourMetadataTransitions() {
    PrintTestHeader("TestColourMetadataTransitions");
    FramePipeline pipeline; pipeline.SetLook(Look::Warm);
    AVFrame* input = AllocTestFrame(32, 18, AV_PIX_FMT_BGR24, 80, 120, 160);
    assert(input);
    input->color_primaries = AVCOL_PRI_BT2020;
    input->color_trc = AVCOL_TRC_BT709;
    input->sample_aspect_ratio = {4, 3};
    input->best_effort_timestamp = 123456;
    AVFrame* output = pipeline.Process(input);
    assert(output != input);
    assert(output->color_primaries == AVCOL_PRI_BT2020);
    assert(output->color_trc == AVCOL_TRC_BT709);
    assert(output->sample_aspect_ratio.num == 4 && output->sample_aspect_ratio.den == 3);
    assert(output->best_effort_timestamp == 123456);
    input->color_primaries = AVCOL_PRI_UNSPECIFIED;
    input->color_trc = AVCOL_TRC_UNSPECIFIED;
    output = pipeline.Process(input);
    assert(output != input);
    assert(output->color_primaries == AVCOL_PRI_UNSPECIFIED);
    assert(output->color_trc == AVCOL_TRC_UNSPECIFIED);
    av_frame_free(&input); PrintTestPassed();
}

void TestHdrAndPaletteBypass() {
    PrintTestHeader("TestHdrAndPaletteBypass");
    FramePipeline pipeline; pipeline.SetLook(Look::Cool);
    AVFrame* input = AllocTestFrame(16, 16, AV_PIX_FMT_BGR24, 80, 120, 160);
    const auto before = SnapshotFrameData(input);
    input->color_trc = AVCOL_TRC_SMPTE2084;
    assert(pipeline.Process(input) == input);
    input->color_trc = AVCOL_TRC_ARIB_STD_B67;
    assert(pipeline.Process(input) == input);
    assert(before == SnapshotFrameData(input));
    av_frame_free(&input);
    input = av_frame_alloc(); assert(input);
    input->width = input->height = 16; input->format = AV_PIX_FMT_PAL8;
    assert(av_frame_get_buffer(input, 32) == 0);
    // Deliberately missing palette must never reach swscale.
    input->data[1] = nullptr;
    assert(pipeline.Process(input) == input);
    av_frame_free(&input); PrintTestPassed();
}

void TestLiveLookSwitching() {
    PrintTestHeader("TestLiveLookSwitching");
    FramePipeline pipeline;
    AVFrame* input = AllocTestFrame(63, 35, AV_PIX_FMT_BGR24, 128, 128, 128, 64);
    assert(input); const auto before = SnapshotFrameData(input);
    for (int n = 0; n < 20; ++n) {
        pipeline.SetLook(Look::Warm); auto* out = pipeline.Process(input);
        assert(out != input && out->data[0][0] == 116 && out->data[0][2] == 142);
        pipeline.SetLook(Look::Cool); out = pipeline.Process(input);
        assert(out != input && out->data[0][0] == 142 && out->data[0][2] == 116);
        pipeline.SetLook(Look::Mono); out = pipeline.Process(input);
        assert(out != input && out->data[0][0] == 128 && out->data[0][2] == 128);
        pipeline.SetLook(Look::Original); assert(pipeline.Process(input) == input);
    }
    assert(before == SnapshotFrameData(input));
    av_frame_free(&input); PrintTestPassed();
}


void TestConsumerScalerFanout() {
    PrintTestHeader("TestConsumerScalerFanout");
    FramePipeline pipeline;
    Video::GuiPreviewScaler preview(62, 34);
    Video::DirectShowScaler directshow(62, 34);
    Video::_1_1Scaler snapshot(AV_PIX_FMT_RGB24);
    AVFrame* input = AllocTestFrame(31, 17, AV_PIX_FMT_BGR24, 128, 128, 128, 64);
    assert(input);
    const auto before = SnapshotFrameData(input);
    const Look looks[] = {Look::Original, Look::Warm, Look::Cool, Look::Mono, Look::Original};
    const uint8_t expected[][3] = {{128,128,128},{116,132,142},{142,130,116},{128,128,128},{128,128,128}};
    for (int i=0; i<5; ++i) {
        pipeline.SetLook(looks[i]); AVFrame* processed = pipeline.Process(input);
        int w=0, h=0;
        const auto* p = preview.Process(processed, w, h); assert(p && w==62 && h==34);
        std::vector<uint8_t> previewCopy(p, p + size_t(w)*h*3);
        const auto* d = directshow.Process(processed, w, h); assert(d && w==62 && h==34);
        std::vector<uint8_t> cameraCopy(d, d + size_t(w)*h*3);
        assert(previewCopy == cameraCopy);
        const auto* s = snapshot.Process(processed, w, h); assert(s && w==31 && h==17);
        std::vector<uint8_t> snapshotCopy(s, s + size_t(w)*h*3);
        for (size_t pixel=0; pixel<previewCopy.size(); pixel+=3)
            for (int c=0;c<3;++c) assert(previewCopy[pixel+c] == expected[i][c]);
        for (size_t pixel=0; pixel<snapshotCopy.size(); pixel+=3)
            for (int c=0;c<3;++c) assert(snapshotCopy[pixel+c] == expected[i][2-c]);
        // Consumers have copied/scaled synchronously before pipeline reuse.
        pipeline.SetLook(Look::Cool); pipeline.Process(input);
        assert(previewCopy == cameraCopy);
        for (int c=0;c<3;++c) assert(snapshotCopy[c] == expected[i][2-c]);
    }
    assert(before == SnapshotFrameData(input));
    av_frame_free(&input); PrintTestPassed();
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "PrismForge Effects: FramePipeline Comprehensive Tests" << std::endl;
    std::cout << "========================================================\n" << std::endl;

    TestExactPointerBypass();
    TestInputImmutability();
    TestActiveOutputOwnership();
    TestOddDimensionsAndPaddedStrides();
    TestRgbBgrChannelOrder();
    TestMonoChannelEquality();
    TestBoundedWarmCoolChanges();
    TestColourRangeConversion();
    TestFormatAndResolutionTransitions();
    TestNullAndInvalidFrames();
    TestRejectUnsupportedPixelFormat();
    TestRejectMissingRequiredPlanes();
    TestRejectInvalidStrides();
    TestInvalidSetting();
    TestRepeatedCalls();
    TestDestructionAndRecreation();

    TestColourMetadataTransitions();
    TestHdrAndPaletteBypass();
    TestLiveLookSwitching();
    TestConsumerScalerFanout();

    BenchmarkCpuProcessingTimings();
    ReportSkippedChecks();

    std::cout << "[SUMMARY] All 20 unit tests passed successfully." << std::endl;
    return 0;
}
