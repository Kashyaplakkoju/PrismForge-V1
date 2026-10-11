#pragma once
#include <vector>
#include <cstdint>
#include <array>
namespace PrismForge::Effects::Overlays::Realistic {
struct MeshVertex { float x,y,z,nx,ny,nz,u,v; };
enum class Material:uint32_t { Frame=0,Lens=1,NosePad=2,CapFabric=3,CapSeam=4,CapUnderbrim=5 };
struct MeshTriangle {uint32_t a,b,c;Material material;};
struct Mesh {std::vector<MeshVertex> vertices;std::vector<MeshTriangle> triangles;};
// Actual local reference glasses, normalized to the source 14cm head-local frame.
// Colour channels are source sRGB values; the native renderer supplies shading.
struct GlassesMaterialSet {
 std::array<float,4> frame,lens,nosePad;
 float frameRoughness,frameMetalness,lensRoughness,scaleMultiplier;
};
// 0 Classic Wayfarer; 1 Gold Teardrop Aviator; 2 Vintage Round Tortoise;
// 3 Chic Cat-Eye Emerald. An invalid style returns an empty mesh.
const Mesh& ReferenceGlassesMesh(int style);
const GlassesMaterialSet& ReferenceGlassesMaterials(int style);
const Mesh& GlassesMesh();
const Mesh& CapMesh();
const Mesh& ReferenceCapMesh(int style);
bool ValidateAccessoryMesh(const Mesh&);
}
