#pragma once
#include "face_filter_asset.h"
#include "overlay_validation.h"
#include <array>
#include <memory>
#include <string>
struct AVFrame;
namespace PrismForge::Effects::Overlays::Realistic {
struct ProjectedVertex {float x,y,depth,distance,r,g,b,a;float u=0,v=0,textured=0;float nx=0,ny=0,nz=1,metallic=0,roughness=1;};
struct CapFitDiagnostics {
 bool automatic=false,headProxy=false;uint32_t templePairs=0;
 float headWidthCm=0,foreheadHeightCm=0,horizontalScaleCm=0,verticalScaleCm=0;
 float crownHeightCm=0,pupilClearanceCm=0;
};
enum class FilterBuildFailure {None,InvalidRequest,AssetUnavailable};
struct ProjectedScene {
 int left=0,top=0,width=0,height=0,rasterWidth=0,rasterHeight=0;
 float focalPixels=0,faceDistance=0,noseClearance=0;
 std::vector<ProjectedVertex> faces,opaque,lenses;
 std::shared_ptr<const TextureRGBA> capTexture,filterBaseTexture,filterMetalRoughTexture;
 float filterMetricScale=0;std::array<float,3> filterAnchorCm{};
 float filterForwardClearanceCm=0;uint32_t filterSurfaceSamples=0;bool filterClearanceLimited=false;
 FilterBuildFailure filterFailure=FilterBuildFailure::None;
 CapFitDiagnostics capFit;
};
// Style0 is the legacy cap;1 is White tennis;2 is Pokemon. Assets are exe-relative.
// Shared CPU/GPU snapshots are immutable, bounded to2048x2048RGBA and retained safely.
std::shared_ptr<const TextureRGBA> LoadCapTexture(int style,std::string& error);
// Compatibility sampler: bilinear encoded RGB and linear alpha.
std::array<float,4> SampleCapTexture(const TextureRGBA&,float u,float v);
// Cap lighting sampler: decode each RGB texel to linear light before filtering.
// Alpha and clamp/orientation semantics match the compatibility sampler.
std::array<float,4> SampleCapTextureLinear(const TextureRGBA&,float u,float v);
// Explicit test harness seam only. Empty restores executable-relative asset lookup.
// Absolute existing directories only; changing it clears positive/negative caches.
bool SetCapAssetRootForTesting(const std::wstring& root);
// Explicit spatial-index verification seam; -infinity denotes an open portal.
bool SampleFaceFilterFrontDepthForTesting(int style,float x,float y,float& depth);
bool RasterizeProjectedSceneForTesting(const ProjectedScene&,std::vector<float>& rgba);
bool BuildProjectedScene(const AnalysisBridge::FaceLandmarkBundle*,int width,int height,
 bool glasses,bool cap,float size,ProjectedScene&,std::string& error,int capStyle=0,int glassesStyle=0,float capScale=1,float capVerticalOffset=0,int faceFilterStyle=0,float faceFilterScale=1,float faceFilterVerticalOffset=0,const std::shared_ptr<const NativeFaceFilter>& customModel={},const CustomFaceFilterFit& customFit={},uint32_t renderEdge=640);
class FaceAccessory3DRenderer {
public:
 FaceAccessory3DRenderer()=default;~FaceAccessory3DRenderer();
 FaceAccessory3DRenderer(const FaceAccessory3DRenderer&)=delete;
 FaceAccessory3DRenderer& operator=(const FaceAccessory3DRenderer&)=delete;
 AVFrame* ProcessFrame(AVFrame*,const AnalysisBridge::FaceLandmarkBundle*,bool glasses,bool cap,
 float size,OverlayOutcome*,LayerReport*,int capStyle=0,int glassesStyle=0,float capScale=1,float capVerticalOffset=0,int faceFilterStyle=0,float faceFilterScale=1,float faceFilterVerticalOffset=0,const std::shared_ptr<const NativeFaceFilter>& customModel={},const CustomFaceFilterFit& customFit={},uint32_t renderEdge=640);
 void Reset();
 const std::string& GetLastError() const {return lastError;}
private:
 AVFrame* output=nullptr;
 std::string lastError;
 std::vector<float> depth,rgba;
};
}
