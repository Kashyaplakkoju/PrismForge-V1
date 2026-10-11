#include "effects/overlays/accessory_mesh.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace R=PrismForge::Effects::Overlays::Realistic;
namespace {
int assertions=0;
void Need(bool ok,const char* message){++assertions;if(!ok)throw std::runtime_error(message);}
bool Same(float a,float b){return std::abs(a-b)<1e-6f;}
void Meshes(){
 Need(&R::ReferenceGlassesMesh(0)==&R::GlassesMesh(),"Classic Wayfarer identity preserved");
 const size_t vertices[]={2833,3058,2833,2833},triangles[]={4710,5094,4710,4710};
 for(int style=0;style<=3;++style){const auto& mesh=R::ReferenceGlassesMesh(style);
  Need(R::ValidateAccessoryMesh(mesh),"selected mesh validates against production bounds");Need(mesh.vertices.size()==vertices[style]&&mesh.triangles.size()==triangles[style],"complete original model geometry retained");Need(&mesh==&R::ReferenceGlassesMesh(style),"immutable cached geometry identity");
  size_t lenses=0,pads=0;for(const auto& triangle:mesh.triangles){Need(triangle.material==R::Material::Frame||triangle.material==R::Material::Lens||triangle.material==R::Material::NosePad,"no cap material or unrepresented material in glasses");if(triangle.material==R::Material::Lens)++lenses;if(triangle.material==R::Material::NosePad)++pads;}
  Need(lenses==126&&pads==336,"both complete curved lenses and nose pads preserved");
  for(const auto& vertex:mesh.vertices)Need(std::abs(vertex.nx*vertex.nx+vertex.ny*vertex.ny+vertex.nz*vertex.nz-1)<2e-6f,"unit source or repaired normals");
 }
 Need(R::ReferenceGlassesMesh(-1).vertices.empty()&&R::ReferenceGlassesMesh(4).vertices.empty()&&R::ReferenceGlassesMesh(std::numeric_limits<int>::max()).vertices.empty(),"invalid mesh selector yields no geometry");
 Need(&R::ReferenceGlassesMesh(1)!=&R::ReferenceGlassesMesh(2)&&&R::ReferenceGlassesMesh(2)!=&R::ReferenceGlassesMesh(3),"individual style geometry snapshots");
}
void Materials(){
 const int frame[][3]={{24,25,28},{226,183,85},{90,39,8},{25,21,28}};
 const int lens[][3]={{28,38,48},{110,69,27},{168,230,207},{0,205,172}};
 const float opacity[]={.8f,.65f,.25f,.8f},scale[]={1,1.05f,.98f,1.04f};
 for(int style=0;style<=3;++style){const auto& m=R::ReferenceGlassesMaterials(style);for(int c=0;c<3;++c){Need(Same(m.frame[c],frame[style][c]/255.f),"actual source frame RGB");Need(Same(m.lens[c],lens[style][c]/255.f),"actual source lens RGB");}
  Need(m.frame[3]==1&&Same(m.lens[3],opacity[style])&&Same(m.nosePad[3],.55f),"actual wired source opacity includes clamp");Need(Same(m.scaleMultiplier,scale[style]),"source pose scale multiplier");Need(Same(m.frameRoughness,.24f)&&Same(m.lensRoughness,.08f)&&Same(m.frameMetalness,style==1?.8f:.05f),"actual wired renderer material parameters, not unused factory");
 }
 Need(&R::ReferenceGlassesMaterials(-1)==&R::ReferenceGlassesMaterials(0),"invalid descriptor fallback to inert legacy material configuration");
}
}
int main(){try{Meshes();Materials();std::cout<<"REFERENCE_GLASSES_NATIVE_GROUPS 2 ASSERTIONS "<<assertions<<" PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<"\n";return 1;}}
