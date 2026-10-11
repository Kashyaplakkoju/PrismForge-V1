#include "result_adapter.h"
#include <chrono>
#include <cmath>
#include <algorithm>

namespace PrismForge::Providers {
namespace {
float StableSigmoid(double x) {
    if (x >= 0) { const double z = std::exp(-x); return static_cast<float>(1 / (1 + z)); }
    const double z = std::exp(x); return static_cast<float>(z / (1 + z));
}
}
ResultAdapter::ResultAdapter() = default;
ResultAdapter::~ResultAdapter() = default;

bool ResultAdapter::AdaptResult(const std::vector<float>& raw, const ModelManifest& m,
    const GeometryTransform& t, const FrameRequest& request,
    std::shared_ptr<Effects::AnalysisBridge::AnalysisBundle>& outBundle) {
    outBundle.reset();
    if (!m.Validate() || !request.IsValid() || t.srcWidth != request.width || t.srcHeight != request.height ||
        t.orientation != request.orientation || t.mirror != request.mirror || t.aspectMode != m.aspectMode) return false;
    int iw, ih, w, h, classes;
    if (!m.InputGeometry(iw, ih) || !m.OutputGeometry(w, h, classes) || t.targetWidth != iw || t.targetHeight != ih) return false;
    const size_t spatial = static_cast<size_t>(w) * h;
    if (raw.size() != spatial * classes) return false;
    for (float f : raw) if (!std::isfinite(f) ||
        (m.outputInterpretation == ModelOutputInterpretation::RawProbability && (f < 0 || f > 1))) return false;
    try {
        std::vector<float> probability(spatial);
        auto at = [&](size_t i, int c) -> float { return raw[m.outputLayout == TensorLayout::NCHW ? c * spatial + i : i * classes + c]; };
        for (size_t i = 0; i < spatial; ++i) {
            float p;
            switch (m.outputInterpretation) {
            case ModelOutputInterpretation::RawProbability: p = at(i, m.personClassIndex); break;
            case ModelOutputInterpretation::LogitsSigmoid: p = StableSigmoid(at(i, m.personClassIndex)); break;
            case ModelOutputInterpretation::SoftmaxTwoClass:
                // Subtract in double to avoid overflow when opposite extreme finite float logits differ.
                p = StableSigmoid(static_cast<double>(at(i, m.personClassIndex)) - at(i, 1 - m.personClassIndex)); break;
            case ModelOutputInterpretation::ArgMax: {
                int winner = 0; for (int c = 1; c < classes; ++c) if (at(i, c) > at(i, winner)) winner = c;
                p = winner == m.personClassIndex ? 1.0f : 0.0f; break; // Ties deterministically choose first class.
            }
            default: return false;
            }
            probability[i] = p;
        }
        auto bundle = std::make_shared<Effects::AnalysisBridge::AnalysisBundle>();
        bundle->sourceGeneration = request.sourceGeneration; bundle->sequenceId = request.sequenceId;
        bundle->timestampPts = request.timestampPts;
        bundle->timestampCompleted = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!m_preprocessor.MapMaskBack(probability.data(), w, h, t, bundle->personMask) || !bundle->HasPersonMask()) return false;
        // Binarize after geometric projection: bilinear interpolation can otherwise
        // reintroduce fractional values despite an explicitly binary output contract.
        if (!m.retainSoftProbability) for (float& p : bundle->personMask.data)
            p = p >= m.probabilityThreshold ? 1.0f : 0.0f;
        outBundle = std::move(bundle); return true;
    } catch (...) { return false; }
}
} // namespace PrismForge::Providers
