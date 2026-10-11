#pragma once
#include "framepipeline.h"
#include "cubelut.h"
#include <filesystem>
#include <mutex>

namespace PrismForge::Effects {
struct DesktopColorSettings {
    Look look = Look::Original;
    bool bypassed = false;
    std::shared_ptr<const CubeLut> lut;
    float intensity = 1.0f;
};
// Load only a user-selected local asset. A rejected asset never changes app settings.
std::shared_ptr<const CubeLut> LoadCubeLutFile(const std::filesystem::path& path, std::string& error);

// One serialized processing caller (Application's SessionGate). Configure is safe
// on the GUI thread; each frame uses one immutable settings snapshot. Consumers
// must finish using the borrowed result before the next Process call. Retaining a
// frame with av_frame_clone is supported by the LUT/conversion buffer's AVBufferRef.
class DesktopColorPipeline {
public:
    DesktopColorPipeline();
    ~DesktopColorPipeline();
    DesktopColorPipeline(const DesktopColorPipeline&) = delete;
    DesktopColorPipeline& operator=(const DesktopColorPipeline&) = delete;
    void Configure(DesktopColorSettings settings);
    DesktopColorSettings GetSettings() const;
    Look GetLook() const;
    AVFrame* Process(AVFrame* input);
    // Serialized caller only; returns a reusable SDR BGR24 frame or nullptr.
    AVFrame* ToBgr(AVFrame* input);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
    DesktopColorSettings m_settings;
};
}
