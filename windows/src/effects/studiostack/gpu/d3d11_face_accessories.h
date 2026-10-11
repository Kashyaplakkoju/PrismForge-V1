#pragma once
#include "d3d11_studiostack_overlay.h"
#include "effects/overlays/face_accessory_3d.h"
namespace PrismForge::Effects::StudioStack::Gpu {
class D3D11FaceAccessories {
public:
 D3D11FaceAccessories(ID3D11Device*,ID3D11DeviceContext*);~D3D11FaceAccessories();
 GpuOverlayOutcome Process(ID3D11ShaderResourceView*,ID3D11UnorderedAccessView*,int width,int height,
 const AnalysisBridge::FaceLandmarkBundle*,bool glasses,bool cap,float size,Overlays::LayerReport*,std::string*,int capStyle=0,int glassesStyle=0,float capScale=1.f,float capVerticalOffset=0.f,int faceFilterStyle=0,float faceFilterScale=1.f,float faceFilterVerticalOffset=0.f,const std::shared_ptr<const Overlays::Realistic::NativeFaceFilter>& customModel={},const Overlays::Realistic::CustomFaceFilterFit& customFit={},uint32_t renderEdge=640);
 Overlays::Realistic::FilterBuildFailure GetLastFilterBuildFailure() const;
 // Read-only allocation evidence: zero until the first valid render, then bounded lazy capacity.
 uint32_t GetVertexCapacityForTesting() const;
private:
 struct Impl;std::unique_ptr<Impl> impl;
};
}
