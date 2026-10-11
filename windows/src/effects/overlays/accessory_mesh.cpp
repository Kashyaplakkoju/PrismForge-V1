#include "accessory_mesh.h"
#include <cmath>
#include <array>
#include <algorithm>
namespace PrismForge::Effects::Overlays::Realistic {
namespace {
constexpr float pi=3.14159265358979323846f;
struct V {float x,y,z;};
V operator+(V a,V b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
V operator-(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
V operator*(V a,float s){return {a.x*s,a.y*s,a.z*s};}
V Cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V Unit(V a){float d=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);return d>1e-6f?a*(1/d):V{0,0,1};}
uint32_t Vertex(Mesh& m,V p,V n,float u=0,float v=0){m.vertices.push_back({p.x,p.y,p.z,n.x,n.y,n.z,u,v});return uint32_t(m.vertices.size()-1);}
void Quad(Mesh& m,uint32_t a,uint32_t b,uint32_t c,uint32_t d,Material mat){m.triangles.push_back({a,b,c,mat});m.triangles.push_back({a,c,d,mat});}
void Tube(Mesh& m,const std::vector<V>& path,float radius,Material mat,bool closed=false){
 constexpr int sides=8;const auto first=uint32_t(m.vertices.size());
 for(size_t i=0;i<path.size();++i){
  V tangent=Unit(path[std::min(i+1,path.size()-1)]-path[i?i-1:0]);
  if(closed&&(i==0||i==path.size()-1))tangent=Unit(path[1]-path[path.size()-2]);
  V n=Unit(Cross(tangent,{0,0,1}));if(std::abs(tangent.z)>.98f)n=Unit(Cross(tangent,{0,1,0}));V b=Unit(Cross(tangent,n));
  for(int j=0;j<sides;++j){float a=2*pi*j/sides;V norm=n*std::cos(a)+b*std::sin(a);Vertex(m,path[i]+norm*radius,norm,float(i)/path.size(),float(j)/sides);}
 }
 for(uint32_t i=0;i+1<path.size();++i)for(uint32_t j=0;j<sides;++j){uint32_t a=first+i*sides+j,b=first+i*sides+(j+1)%sides;Quad(m,a,b,b+sides,a+sides,mat);}
}
std::vector<V> Curve(std::initializer_list<V> points,int count){std::vector<V> p(points),out;for(int k=0;k<=count;++k){float t=float(k)*float(p.size()-1)/count;int i=std::min(int(t),int(p.size()-2));t-=i;V a=p[i?i-1:i],b=p[i],c=p[i+1],d=p[std::min(i+2,int(p.size()-1))];out.push_back((b*2+(c-a)*t+(a*2-b*5+c*4-d)*(t*t)+(b*3-a-c*3+d)*(t*t*t))*.5f);}return out;}
// Geometry exported from the user's actual glasses-tryon-v2 Classic Wayfarer.
// Regenerate with tools/accessories/export_reference_glasses.mjs; provenance is
// recorded beside that tool. No Three.js runtime is required in the app.
#include "reference_wayfarer_mesh.inc"
#include "reference_glasses_collection_mesh.inc"
#include "reference_caps_mesh.inc"
Mesh MakeGlasses(){
 Mesh m;m.vertices.assign(std::begin(kReferenceWayfarerVertices),std::end(kReferenceWayfarerVertices));
 m.triangles.assign(std::begin(kReferenceWayfarerTriangles),std::end(kReferenceWayfarerTriangles));return m;
}
Mesh MakeCap(){Mesh m;constexpr int levels=20,segments=64;
 // Low baseball-cap crown with curved fabric panels. Base is the forehead anchor.
 for(int r=0;r<=levels;++r)for(int i=0;i<=segments;++i){float a=2*pi*i/segments,t=(pi*.5f)*r/levels;V p{.55f*std::sin(t)*std::cos(a),.43f*std::cos(t),.60f*std::sin(t)*std::sin(a)-.10f};V n=Unit({p.x/(.55f*.55f),p.y/(.43f*.43f),(p.z+.1f)/(.6f*.6f)});Vertex(m,p,n,float(i)/segments,float(r)/levels);}
 for(int r=0;r<levels;++r)for(int i=0;i<segments;++i){uint32_t a=r*(segments+1)+i;Quad(m,a,a+1,a+segments+2,a+segments+1,Material::CapFabric);}
 for(int panel=0;panel<6;++panel){std::vector<V> line;float a=2*pi*panel/6;for(int i=1;i<=levels;++i){float t=pi*.5f*i/levels;line.push_back({.552f*std::sin(t)*std::cos(a),.432f*std::cos(t),.602f*std::sin(t)*std::sin(a)-.1f});}Tube(m,line,.002f,Material::CapSeam);}
 // Curved front visor with thickness and underside, not a flat rectangular sticker.
 for(int underside=0;underside<2;++underside){uint32_t first=uint32_t(m.vertices.size());constexpr int along=32,deep=8;
  for(int r=0;r<=deep;++r)for(int i=0;i<=along;++i){float x=-.56f+1.12f*i/along,q=float(r)/deep,k=std::sqrt(std::max(0.f,1-(x/.57f)*(x/.57f)));float z=.23f+.25f*k+q*.44f*k,y=-.018f-.12f*q+.06f*(x/.56f)*(x/.56f)-(underside?.012f:0);Vertex(m,{x,y,z},Unit({-.15f*x,underside?-1.f:1.f,-.27f}),float(i)/along,q);}
  for(int r=0;r<deep;++r)for(int i=0;i<along;++i){uint32_t a=first+r*(along+1)+i;Quad(m,a,a+1,a+along+2,a+along+1,underside?Material::CapUnderbrim:Material::CapFabric);}
 }
 return m;
}
}
const Mesh& GlassesMesh(){static const Mesh m=MakeGlasses();return m;}
const Mesh& ReferenceGlassesMesh(int style){
 static const Mesh aviator{std::vector<MeshVertex>(std::begin(kReferenceAviatorVertices),std::end(kReferenceAviatorVertices)),std::vector<MeshTriangle>(std::begin(kReferenceAviatorTriangles),std::end(kReferenceAviatorTriangles))};
 static const Mesh round{std::vector<MeshVertex>(std::begin(kReferenceRoundTortoiseVertices),std::end(kReferenceRoundTortoiseVertices)),std::vector<MeshTriangle>(std::begin(kReferenceRoundTortoiseTriangles),std::end(kReferenceRoundTortoiseTriangles))};
 static const Mesh catEye{std::vector<MeshVertex>(std::begin(kReferenceCatEyeVertices),std::end(kReferenceCatEyeVertices)),std::vector<MeshTriangle>(std::begin(kReferenceCatEyeTriangles),std::end(kReferenceCatEyeTriangles))};
 static const Mesh empty{};return style==0?GlassesMesh():style==1?aviator:style==2?round:style==3?catEye:empty;
}
const GlassesMaterialSet& ReferenceGlassesMaterials(int style){
 static constexpr GlassesMaterialSet classic{{24.f/255,25.f/255,28.f/255,1},{28.f/255,38.f/255,48.f/255,.8f},{228.f/255,212.f/255,196.f/255,.55f},.24f,.05f,.08f,1};
 return style==1?kReferenceAviatorMaterials:style==2?kReferenceRoundTortoiseMaterials:style==3?kReferenceCatEyeMaterials:classic;
}
const Mesh& CapMesh(){static const Mesh m=MakeCap();return m;}
const Mesh& ReferenceCapMesh(int style){
 static const Mesh pokemon{std::vector<MeshVertex>(std::begin(kPokemonCapVertices),std::end(kPokemonCapVertices)),std::vector<MeshTriangle>(std::begin(kPokemonCapTriangles),std::end(kPokemonCapTriangles))};
 static const Mesh empty{};return style==2?pokemon:style==0?CapMesh():empty;
}
bool ValidateAccessoryMesh(const Mesh& mesh){
 if(mesh.vertices.empty()||mesh.vertices.size()>60000||mesh.triangles.empty()||mesh.triangles.size()>60000)return false;
 for(const auto& v:mesh.vertices){
  for(float x:{v.x,v.y,v.z,v.nx,v.ny,v.nz,v.u,v.v})if(!std::isfinite(x))return false;
  const float n=v.nx*v.nx+v.ny*v.ny+v.nz*v.nz;
  if(n<.5f||n>1.5f||std::abs(v.x)>4||std::abs(v.y)>4||std::abs(v.z)>4||std::abs(v.u)>16||std::abs(v.v)>16)return false;
 }
 for(const auto& t:mesh.triangles)if(t.a>=mesh.vertices.size()||t.b>=mesh.vertices.size()||t.c>=mesh.vertices.size()||static_cast<uint32_t>(t.material)>5)return false;
 return true;
}
}
