#pragma once

#include "providers/model_manifest.h"
#include <vector>
#include <cstdint>
#include <fstream>
#include <string>

namespace PrismForge::Tests {

/// <summary>
/// Deterministic tiny ONNX fixture graph with complete provenance.
/// 
/// Provenance:
///   Model:      TinyGrayscaleConv
///   Format:     ONNX IR v8, Opset 17
///   Graph:      1x1 2D Convolution mapping 3-channel RGB [1, 3, 16, 16] to 1-channel Grayscale [1, 1, 16, 16]
///   Kernel:     W shape [1, 3, 1, 1], values: [0.299f, 0.587f, 0.114f] (standard Rec. 601 luma)
///   Bias:       None
///   Expected:   output[y, x] == 0.299 * R[y, x] + 0.587 * G[y, x] + 0.114 * B[y, x]
///   Generation: Pure-Python protobuf wire-format serializer (zero external weights or black-box dependencies)
///   Status:     Real ONNX test fixture (never a production detector or implicit fallback)
/// </summary>
class TinyOnnxFixture {
public:
    static constexpr size_t kModelByteSize = 176;

    static const uint8_t* GetModelBytes() {
        static const uint8_t s_bytes[kModelByteSize] = {
            0x08, 0x08, 0x12, 0x0a, 0x50, 0x72, 0x69, 0x73, 0x6d, 0x46, 0x6f, 0x72, 0x67, 0x65, 0x1a, 0x03,
            0x31, 0x2e, 0x30, 0x3a, 0x94, 0x01, 0x12, 0x11, 0x54, 0x69, 0x6e, 0x79, 0x47, 0x72, 0x61, 0x79,
            0x73, 0x63, 0x61, 0x6c, 0x65, 0x43, 0x6f, 0x6e, 0x76, 0x0a, 0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70,
            0x75, 0x74, 0x0a, 0x01, 0x57, 0x12, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x1a, 0x05, 0x63,
            0x6f, 0x6e, 0x76, 0x30, 0x22, 0x04, 0x43, 0x6f, 0x6e, 0x76, 0x2a, 0x1b, 0x08, 0x01, 0x08, 0x03,
            0x08, 0x01, 0x08, 0x01, 0x10, 0x01, 0x4a, 0x0c, 0x87, 0x16, 0x99, 0x3e, 0xa2, 0x45, 0x16, 0x3f,
            0xd5, 0x78, 0xe9, 0x3d, 0x42, 0x01, 0x57, 0x5a, 0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74,
            0x12, 0x16, 0x0a, 0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x03,
            0x0a, 0x02, 0x08, 0x10, 0x0a, 0x02, 0x08, 0x10, 0x62, 0x20, 0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70,
            0x75, 0x74, 0x12, 0x16, 0x0a, 0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02,
            0x08, 0x01, 0x0a, 0x02, 0x08, 0x10, 0x0a, 0x02, 0x08, 0x10, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11
        };
        return s_bytes;
    }

    static std::vector<uint8_t> GetModelByteVector() {
        const uint8_t* p = GetModelBytes();
        return std::vector<uint8_t>(p, p + kModelByteSize);
    }

    static bool WriteFixtureFile(const std::wstring& filePath) {
        std::ofstream ofs(filePath, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs.write(reinterpret_cast<const char*>(GetModelBytes()), kModelByteSize);
        return ofs.good();
    }

    static float ComputeExpectedLuma(float r, float g, float b) {
        return 0.299f * r + 0.587f * g + 0.114f * b;
    }

    static Providers::ModelManifest CreateManifest() {
        Providers::ModelManifest m;
        m.modelName = "TinyGrayscaleConvFixture";
        m.modelVersion = "1.0.0";
        m.inputTensorName = "input";
        m.inputDims = { 1, 3, 16, 16 };
        m.inputLayout = Providers::TensorLayout::NCHW;
        m.outputTensorName = "output";
        m.outputDims = { 1, 1, 16, 16 };
        m.outputInterpretation = Providers::ModelOutputInterpretation::RawProbability;
        m.personClassIndex = 0;
        m.retainSoftProbability = true;
        m.producesPersonMask = true;
        m.producesSkinMask = false;
        m.producesProtectionMask = false;
        m.producesNormalMap = false;
        m.producesDepthMap = false;
        m.producesFaceLandmarks = false;
        return m;
    }
};

} // namespace PrismForge::Tests
