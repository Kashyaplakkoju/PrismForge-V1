#pragma once
#include "rtsp/ffmpeg.h"
#include <memory>

namespace PrismForge::Effects {
enum class Look : int { Original = 0, Warm = 1, Cool = 2, Mono = 3 };

class FramePipeline {
public:
    FramePipeline();
    ~FramePipeline();
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;
    void SetLook(Look look) noexcept;
    Look GetLook() const noexcept;
    AVFrame* Process(AVFrame* input);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
