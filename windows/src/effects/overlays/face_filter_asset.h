#pragma once
#include "accessory_mesh.h"
#include "model_performance.h"
#include <array>
#include <memory>
#include <string>
namespace PrismForge::Effects::Overlays::Realistic {
// RGB is original sRGB for base images; alpha and metallic/roughness images are linear.
struct TextureRGBA {uint32_t width=0,height=0;std::vector<uint8_t> pixels;};
struct FilterTriangle {uint32_t a,b,c,material;};
struct FilterMaterial {
 std::array<float,4> baseColor{1,1,1,1};
 float metallic=0,roughness=1;
 bool blend=false,textured=false;
};
struct NativeFaceFilter {
 // Metres, +Y up, +Z toward viewer, authored nose-bridge origin; never AABB-recentred.
 std::vector<MeshVertex> vertices;
 std::vector<FilterTriangle> triangles;
 std::vector<FilterMaterial> materials;
 std::shared_ptr<const TextureRGBA> baseTexture,metalRoughTexture;
 float authoredIpdMeters=.064f;
 float authoredEyeHeightMeters=.008f,authoredEyeDepthMeters=0;
};
// User fitting is a rigid transform about the authored pupil midpoint.
struct CustomFaceFilterFit {
 float xCm=0,yCm=0,zCm=0,tiltDegrees=0,turnDegrees=0,rollDegrees=0;
 bool headOcclusion=true;
};
bool ValidateCustomFaceFilterFit(const CustomFaceFilterFit&,std::string& error);
// One bounded immutable PFM snapshot; callers load only on an owned worker.
std::shared_ptr<const NativeFaceFilter> LoadFaceFilterFile(const std::wstring& path,std::string& error);
// 1 Aurora fox, 2 Nocturne owl, 3 Atlas explorer helmet, 5 private Royal crown. 4 is the local-model pointer route. 0 means no filter.
std::shared_ptr<const NativeFaceFilter> LoadFaceFilter(int style,std::string& error);
bool ValidateFaceFilter(const NativeFaceFilter&,std::string& error);
// Test seam: absolute existing non-reparse directory only. Empty restores exe-relative assets.
// Changing the root invalidates both successful and unsuccessful immutable cached snapshots.
bool SetFaceFilterAssetRootForTesting(const std::wstring& root);
}
