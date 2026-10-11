#pragma once
#include <cstdint>
#include <string>
namespace PrismForge::Effects::Overlays::Realistic {
inline constexpr uint32_t HardModelVertices=1000000, HardModelTriangles=1000000, HardModelMaterials=16;
inline constexpr uint32_t HardModelTexture=2048, HardModelRenderEdge=1536;
inline constexpr uint64_t HardModelFileBytes=128ull*1024*1024;
enum class ModelPerformanceMode : int { Auto=0, Performance=1, Balanced=2, Quality=3, Custom=4 };
struct ModelPerformanceSettings {
 ModelPerformanceMode mode=ModelPerformanceMode::Auto;
 uint32_t customNodes=4096, customVertices=200000, customTriangles=200000, customRenderEdge=1024;
};
struct ModelHardwareResources {
 uint64_t ramBytes=0, dedicatedVideoBytes=0;
 uint32_t logicalProcessors=0;
 bool hardwareGpu=false;
 std::string adapterName;
};
struct ModelResourceBudget {
 uint32_t nodes=4096, meshes=1024, primitives=1024, vertices=60000, triangles=60000, renderEdge=640;
 uint32_t materials=16, images=64, textures=64, accessorElements=3000000;
 uint64_t fileBytes=32ull*1024*1024, parseBytes=64ull*1024*1024;
 ModelPerformanceMode resolvedMode=ModelPerformanceMode::Balanced;
};
// Pure policy helpers. Hardware recommendations are estimates, not a measured FPS guarantee.
inline bool ValidateModelPerformanceSettings(const ModelPerformanceSettings& s,std::string& error) {
 error.clear();if(int(s.mode)<0||int(s.mode)>4){error="Unknown model performance mode";return false;}
 if(s.customNodes<512||s.customNodes>16384||s.customVertices<1000||s.customVertices>HardModelVertices||s.customTriangles<1000||s.customTriangles>HardModelTriangles||s.customRenderEdge<256||s.customRenderEdge>HardModelRenderEdge){error="Custom model budgets are outside supported bounds";return false;}return true;
}
inline ModelResourceBudget ResolveModelResourceBudget(const ModelPerformanceSettings& settings,const ModelHardwareResources& hardware={}) {
 ModelPerformanceSettings s=settings;std::string error;if(!ValidateModelPerformanceSettings(s,error))s={};
 auto mode=s.mode;if(mode==ModelPerformanceMode::Auto){
  if(!hardware.hardwareGpu||hardware.ramBytes<8ull*1024*1024*1024||hardware.logicalProcessors<4)mode=ModelPerformanceMode::Performance;
  else if(hardware.ramBytes>=16ull*1024*1024*1024&&hardware.dedicatedVideoBytes>=4ull*1024*1024*1024&&hardware.logicalProcessors>=8)mode=ModelPerformanceMode::Quality;
  else mode=ModelPerformanceMode::Balanced;
 }
 ModelResourceBudget b;b.resolvedMode=mode;
 if(mode==ModelPerformanceMode::Performance){b.nodes=2048;b.meshes=b.primitives=512;b.vertices=b.triangles=30000;b.renderEdge=320;}
 else if(mode==ModelPerformanceMode::Quality){b.nodes=8192;b.meshes=b.primitives=2048;b.vertices=b.triangles=200000;b.renderEdge=1024;b.fileBytes=64ull*1024*1024;b.parseBytes=128ull*1024*1024;}
 else if(mode==ModelPerformanceMode::Custom){b.nodes=s.customNodes;b.meshes=b.primitives=4096;b.vertices=s.customVertices;b.triangles=s.customTriangles;b.renderEdge=s.customRenderEdge;b.fileBytes=HardModelFileBytes;b.parseBytes=256ull*1024*1024;}
 return b;
}
inline const char* ModelPerformanceModeName(ModelPerformanceMode mode){switch(mode){case ModelPerformanceMode::Auto:return "Auto";case ModelPerformanceMode::Performance:return "Performance";case ModelPerformanceMode::Balanced:return "Balanced";case ModelPerformanceMode::Quality:return "Quality";case ModelPerformanceMode::Custom:return "Custom";}return "Auto";}
}
