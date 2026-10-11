#pragma once
#include "rtsp/ffmpeg.h"
#include <memory>

namespace PrismForge::Effects {

enum class BackgroundMode : int {
    Original = 0,
    Blur = 1,
    Solid = 2
};

struct AlphaMask {
    const float* data = nullptr;
    int width = 0;
    int height = 0;
    int strideFloats = 0;
};

struct BackgroundSettings {
    BackgroundMode mode = BackgroundMode::Original;
    int blurRadius = 8;
    unsigned char solidB = 32, solidG = 32, solidR = 32;
};

class BackgroundCompositor {
public:
    BackgroundCompositor();
    ~BackgroundCompositor();
    BackgroundCompositor(const BackgroundCompositor&) = delete;
    BackgroundCompositor& operator=(const BackgroundCompositor&) = delete;

    AVFrame* Process(AVFrame* bgrInput, const AlphaMask* mask,
                     const BackgroundSettings& settings);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PrismForge::Effects
