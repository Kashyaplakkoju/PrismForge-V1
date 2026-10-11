#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include "../depth_types.h"
#include "effects/analysisbridge/analysis_types.h"

namespace PrismForge::Effects::Relighting::Gpu {

/// <summary>
/// GPU (Direct3D 11) compute shader implementation of depth-to-normal reconstruction.
/// Uploads single-channel float depth buffers to native R32_FLOAT textures and executes
/// CS_DepthToNormals compute shader to produce unit-length surface normals in R32G32B32A32_FLOAT.
/// </summary>
class D3D11DepthToNormal {
public:
    D3D11DepthToNormal();
    ~D3D11DepthToNormal();

    // Non-copyable, movable
    D3D11DepthToNormal(const D3D11DepthToNormal&) = delete;
    D3D11DepthToNormal& operator=(const D3D11DepthToNormal&) = delete;
    D3D11DepthToNormal(D3D11DepthToNormal&&) noexcept = default;
    D3D11DepthToNormal& operator=(D3D11DepthToNormal&&) noexcept = default;

    /// <summary>
    /// Reconstructs surface normals from the supplied DepthBuffer on the GPU.
    /// </summary>
    GpuDepthToNormalOutcome ReconstructNormals(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const DepthBuffer& depth,
        const DepthToNormalConfig& config = DepthToNormalConfig{},
        GpuDepthToNormalFaultSeam faultSeam = GpuDepthToNormalFaultSeam::None,
        HRESULT* outHresult = nullptr,
        std::string* errorDescription = nullptr
    );

    /// <summary>
    /// True if an active reconstructed normal texture is available.
    /// </summary>
    bool HasOutput() const { return m_hasOutput && m_outputSRV != nullptr; }

    /// <summary>
    /// SRV for downstream lighting shaders (DXGI_FORMAT_R32G32B32A32_FLOAT).
    /// </summary>
    ID3D11ShaderResourceView* GetOutputSRV() const { return HasOutput() ? m_outputSRV.Get() : nullptr; }
    ID3D11Texture2D* GetOutputTexture() const { return HasOutput() ? m_outputTex.Get() : nullptr; }

    int GetWidth() const { return HasOutput() ? m_currentWidth : 0; }
    int GetHeight() const { return HasOutput() ? m_currentHeight : 0; }

    /// <summary>
    /// Clears active outputs.
    /// </summary>
    void Reset();

    /// <summary>
    /// Releases all GPU resources.
    /// </summary>
    void ReleaseResources();

    /// <summary>
    /// Reads back active GPU normals into host memory for parity/correctness verification.
    /// </summary>
    bool ReadbackNormals(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AnalysisBridge::NormalBuffer& outNormals
    ) const;

private:
    struct DepthToNormalCB {
        int width;
        int height;
        int format;       // 0 = RelativeDepth, 1 = InverseDepth
        int useSobel;     // 1 = Sobel 3x3, 0 = Central diff
        float depthScale;
        float invalidMin;
        float invalidMax;
        float aspect;
        int flipX;
        int flipY;
        float fallbackZ;
        float padding;
    };

    bool EnsureShaders(ID3D11Device* device, std::string* errorDescription);
    bool EnsureTextures(ID3D11Device* device, int width, int height, std::string* errorDescription);

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_computeShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_inputTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_inputSRV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_outputTex;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_outputSRV;

    mutable Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTex;

    Microsoft::WRL::ComPtr<ID3D11Device> m_resourceDevice;
    bool m_hasOutput = false;
    int m_inputCapacityW = 0;
    int m_inputCapacityH = 0;
    int m_currentWidth = 0;
    int m_currentHeight = 0;
};

} // namespace PrismForge::Effects::Relighting::Gpu
