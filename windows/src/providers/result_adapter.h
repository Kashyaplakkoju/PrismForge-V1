#pragma once

#include "model_manifest.h"
#include "frame_request.h"
#include "frame_preprocessor.h"
#include "effects/analysisbridge/analysis_types.h"
#include <memory>
#include <vector>

namespace PrismForge::Providers {

/// <summary>
/// Result adapter converting raw tensor outputs from the inference runner into
/// validated, immutable AnalysisBundle instances ready for publication to the AnalysisMailbox.
/// Strictly enforces truthful capabilities and exact timestamp/epoch provenance.
/// </summary>
class ResultAdapter {
public:
    ResultAdapter();
    ~ResultAdapter();

    ResultAdapter(const ResultAdapter&) = delete;
    ResultAdapter& operator=(const ResultAdapter&) = delete;

    /// <summary>
    /// Adapts exact-shape finite float outputs using the independently declared output layout.
    /// Raw probabilities must be strictly in [0,1]. Softmax honors the selected person class;
    /// ArgMax ties select the lowest class index. Binary thresholding occurs after projection.
    /// Only the person capability is available. Failure clears outBundle; no partial publication.
    /// </summary>
    bool AdaptResult(
        const std::vector<float>& rawModelOutput,
        const ModelManifest& manifest,
        const GeometryTransform& transform,
        const FrameRequest& request,
        std::shared_ptr<Effects::AnalysisBridge::AnalysisBundle>& outBundle
    );

private:
    FramePreprocessor m_preprocessor;
};

} // namespace PrismForge::Providers
