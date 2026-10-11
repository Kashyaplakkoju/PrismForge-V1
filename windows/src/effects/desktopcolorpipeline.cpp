#include "desktopcolorpipeline.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace PrismForge::Effects {
std::shared_ptr<const CubeLut> LoadCubeLutFile(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) { error = "Could not open the LUT file."; return {}; }
        const auto length = file.tellg();
        if (length <= 0 || length > static_cast<std::streamoff>(CubeLut::MaxPayloadBytes)) {
            error = "Choose a nonempty 3D .cube LUT up to 16 MiB."; return {};
        }
        std::string text(static_cast<size_t>(length) + 1, '\0');
        file.seekg(0);
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (file.bad() || file.gcount() != length) { error = "The LUT could not be read completely or changed while loading."; return {}; }
        text.resize(static_cast<size_t>(length));
        return CubeLut::Parse(text, &error);
    } catch (const std::exception&) { error = "Could not load the LUT file."; return {}; }
}
namespace {
bool ValidSdr(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384 ||
        static_cast<int64_t>(frame->width) * frame->height > 64LL * 1024 * 1024 ||
        frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        frame->colorspace == AVCOL_SPC_BT2020_CL) return false;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    switch (format) {
    case AV_PIX_FMT_BGR24: case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGRA: case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGR0: case AV_PIX_FMT_RGB0: case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_NV12: case AV_PIX_FMT_NV21: case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422:
    case AV_PIX_FMT_GRAY8: break;
    default: return false;
    }
    int minimum[4]{};
    if (av_image_fill_linesizes(minimum, format, frame->width) < 0) return false;
    const int planes = av_pix_fmt_count_planes(format);
    int64_t size = 0;
    for (int p = 0; p < planes; ++p) {
        if (!frame->data[p] || frame->linesize[p] <= 0 || frame->linesize[p] < minimum[p]) return false;
        size += static_cast<int64_t>(frame->linesize[p]) * frame->height;
    }
    return planes > 0 && planes <= 4 && size <= (1LL << 30);
}
}
struct DesktopColorPipeline::Impl {
    FramePipeline look;
    CubeLutProcessor lut;
    SwsContext* conversion = nullptr;
    AVFrame* bgr = nullptr;
    ~Impl() { sws_freeContext(conversion); av_frame_free(&bgr); }
    AVFrame* Convert(AVFrame* input) {
        if (input->format == AV_PIX_FMT_BGR24) return input;
        if (!bgr || bgr->width != input->width || bgr->height != input->height) {
            auto* next = av_frame_alloc();
            if (!next) return nullptr;
            next->width = input->width; next->height = input->height; next->format = AV_PIX_FMT_BGR24;
            if (av_frame_get_buffer(next, 64) < 0) { av_frame_free(&next); return nullptr; }
            av_frame_free(&bgr); bgr = next;
        }
        conversion = sws_getCachedContext(conversion, input->width, input->height, static_cast<AVPixelFormat>(input->format),
            input->width, input->height, AV_PIX_FMT_BGR24, SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT,
            nullptr, nullptr, nullptr);
        if (!conversion || av_frame_make_writable(bgr) < 0) return nullptr;
        const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
        int matrix = SWS_CS_ITU601;
        switch (input->colorspace) {
        case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
        case AVCOL_SPC_FCC: matrix = SWS_CS_FCC; break;
        case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_BT2020_NCL: matrix = SWS_CS_BT2020; break;
        default: break;
        }
        const bool full = input->color_range == AVCOL_RANGE_JPEG || (desc->flags & AV_PIX_FMT_FLAG_RGB) ||
            input->format == AV_PIX_FMT_YUVJ420P || input->format == AV_PIX_FMT_YUVJ422P || input->format == AV_PIX_FMT_YUVJ444P;
        const int* coefficients = sws_getCoefficients(matrix);
        if (sws_setColorspaceDetails(conversion, coefficients, full ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16) < 0)
            return nullptr;
        if (sws_scale(conversion, input->data, input->linesize, 0, input->height, bgr->data, bgr->linesize) != input->height)
            return nullptr;
        bgr->pts = input->pts; bgr->pkt_dts = input->pkt_dts; bgr->duration = input->duration;
        bgr->time_base = input->time_base; bgr->sample_aspect_ratio = input->sample_aspect_ratio;
        bgr->best_effort_timestamp = input->best_effort_timestamp;
        bgr->color_range = AVCOL_RANGE_JPEG; bgr->colorspace = AVCOL_SPC_RGB;
        bgr->color_primaries = input->color_primaries; bgr->color_trc = input->color_trc;
        return bgr;
    }
};
DesktopColorPipeline::DesktopColorPipeline() : m_impl(std::make_unique<Impl>()) {}
DesktopColorPipeline::~DesktopColorPipeline() = default;
void DesktopColorPipeline::Configure(DesktopColorSettings settings) {
    if (settings.look < Look::Original || settings.look > Look::Mono) settings.look = Look::Original;
    settings.intensity = std::isfinite(settings.intensity) ? std::clamp(settings.intensity, 0.0f, 1.0f) : 0.0f;
    std::lock_guard<std::mutex> lock(m_mutex); m_settings = std::move(settings);
}
DesktopColorSettings DesktopColorPipeline::GetSettings() const {
    std::lock_guard<std::mutex> lock(m_mutex); return m_settings;
}
Look DesktopColorPipeline::GetLook() const {
    const auto settings = GetSettings(); return settings.bypassed ? Look::Original : settings.look;
}
AVFrame* DesktopColorPipeline::ToBgr(AVFrame* input) {
    return ValidSdr(input) ? m_impl->Convert(input) : nullptr;
}
AVFrame* DesktopColorPipeline::Process(AVFrame* input) {
    const auto settings = GetSettings();
    if (!input || settings.bypassed) return input;
    const bool hasLut = settings.lut && settings.intensity > 0;
    if (settings.look == Look::Original && !hasLut) return input;
    if (!ValidSdr(input)) return input;
    m_impl->look.SetLook(settings.look);
    auto* looked = m_impl->look.Process(input);
    if (!hasLut) return looked;
    // If a requested built-in stage failed, keep the untouched current frame.
    if (settings.look != Look::Original && looked == input) return input;
    auto* bgr = m_impl->Convert(looked);
    if (!bgr) return input;
    auto* result = m_impl->lut.Process(bgr, settings.lut.get(), settings.intensity);
    return result == bgr ? input : result;
}
}
