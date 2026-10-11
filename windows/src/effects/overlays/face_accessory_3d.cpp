#include "face_accessory_3d.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <windows.h>
#include <filesystem>
#include <mutex>
#include <array>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
namespace PrismForge::Effects::Overlays::Realistic {
namespace {
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
struct TextureCache {
 std::mutex mutex;std::wstring root;std::array<std::shared_ptr<const TextureRGBA>,2> snapshots;
 std::array<bool,2> attempted{};std::array<std::string,2> errors;
};
TextureCache& Cache(){static TextureCache cache;return cache;}
uint32_t LE32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
struct V {float x,y,z;};
V operator+(V a,V b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
V operator-(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
V operator*(V a,float s){return {a.x*s,a.y*s,a.z*s};}
float Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
V Cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V Unit(V a){float n=std::sqrt(Dot(a,a));return n>1e-6f?a*(1/n):V{};}
V Rotate(V p,V x,V y,V z){return x*p.x+y*p.y+z*p.z;}
V ToWorld(const AnalysisBridge::FaceMeshVertex3D& p,int w,int h,float focal,float distance,float bridgeZ){float z=-distance-(p.z-bridgeZ)*w*distance/focal;return {(p.x-.5f)*w*(-z)/focal,(.5f-p.y)*h*(-z)/focal,z};}
ProjectedVertex Project(V p,V normal,Material mat,float u,float v,int w,int h,float focal,bool textured=false,const GlassesMaterialSet* glassesMaterial=nullptr){
 float d=-p.z;ProjectedVertex out{w*.5f+focal*p.x/d,h*.5f-focal*p.y/d,250.f/249.f-250.f/(249.f*d),d,0,0,0,1};
 V n=Unit(normal),light=Unit({-.4f,.65f,1.f}),fill=Unit({.7f,.2f,1.f}),half=Unit(light+V{0,0,1});
 float diffuse=.36f+.50f*std::max(0.f,Dot(n,light))+.16f*std::max(0.f,Dot(n,fill));
 float spec=std::pow(std::max(0.f,Dot(n,half)),mat==Material::Lens?96.f:40.f);
 if(glassesMaterial&&(mat==Material::Frame||mat==Material::Lens||mat==Material::NosePad)){
  const auto& color=mat==Material::Frame?glassesMaterial->frame:mat==Material::Lens?glassesMaterial->lens:glassesMaterial->nosePad;
  const float rough=mat==Material::Lens?glassesMaterial->lensRoughness:glassesMaterial->frameRoughness;
  const float shine=std::pow(std::max(0.f,Dot(n,half)),std::clamp(2.f/(rough*rough)-2.f,2.f,256.f));
  const float highlight=mat==Material::Frame?(.12f+.26f*glassesMaterial->frameMetalness)*shine:mat==Material::Lens?.30f*shine:0.f;
  const float shade=mat==Material::NosePad?1.f:diffuse;
  out.r=color[0]*shade+highlight;out.g=color[1]*shade+highlight;out.b=color[2]*shade+highlight;out.a=color[3];return out;
 }
 switch(mat){
 case Material::Frame:out.r=(24.f/255)*diffuse+.26f*spec;out.g=(25.f/255)*diffuse+.26f*spec;out.b=(28.f/255)*diffuse+.26f*spec;break;
 case Material::Lens:out.r=(28.f/255)*diffuse+.30f*spec;out.g=(38.f/255)*diffuse+.30f*spec;out.b=(48.f/255)*diffuse+.30f*spec;out.a=.8f;break;
 case Material::NosePad:out.r=228.f/255;out.g=212.f/255;out.b=196.f/255;out.a=.55f;break;
 case Material::CapFabric:{if(textured){out.r=out.g=out.b=diffuse;out.u=u;out.v=v;out.textured=1;break;}float texture=.97f+.03f*std::sin(u*410.f)*std::sin(v*180.f);out.r=.16f*diffuse*texture;out.g=.19f*diffuse*texture;out.b=.27f*diffuse*texture;break;}
 case Material::CapSeam:out.r=.15f*diffuse;out.g=.17f*diffuse;out.b=.20f*diffuse;break;
 case Material::CapUnderbrim:out.r=.027f*diffuse;out.g=.034f*diffuse;out.b=.047f*diffuse;break;
 }
 return out;
}
bool ValidPlane(const AVFrame* f){
 if(!f||f->format!=AV_PIX_FMT_BGR24||f->width<=0||f->height<=0||f->width>8192||f->height>8192||!f->data[0]||f->linesize[0]<f->width*3)return false;
 uint64_t span=uint64_t(f->linesize[0])*(f->height-1)+uint64_t(f->width)*3;uintptr_t start=reinterpret_cast<uintptr_t>(f->data[0]);if(span>(1ULL<<30)||span>UINTPTR_MAX-start)return false;
 bool owned=false,inside=false;for(const auto* b:f->buf)if(b){owned=true;uintptr_t base=reinterpret_cast<uintptr_t>(b->data);if(b->data&&b->size<=UINTPTR_MAX-base&&start>=base&&start-base<=b->size&&span<=b->size-(start-base))inside=true;}return !owned||inside;
}
// Cap image RGB is sRGB; alpha stays linear. Decode bytes before filtering.
const std::array<float,256>& LinearSrgbBytes(){
 static const auto values=[](){std::array<float,256> result{};for(size_t i=0;i<result.size();++i){const float encoded=float(i)/255.f;result[i]=encoded<=.04045f?encoded/12.92f:std::pow((encoded+.055f)/1.055f,2.4f);}return result;}();
 return values;
}
float EncodeSrgb(float linear){
 if(!std::isfinite(linear))return 0;linear=std::clamp(linear,0.f,1.f);
 return linear<=.0031308f?12.92f*linear:1.055f*std::pow(linear,1.f/2.4f)-.055f;
}
// Bounded material-aware lighting shared with the GPU pixel shader. Base RGB is
// linear glTF colour; metallic/roughness maps stay linear and affect highlights.
std::array<float,3> FilterShade(V normal,const float* base,float metallic,float roughness){
 const V n=Unit(normal),key=Unit({-.4f,.65f,1.f}),fill=Unit({.7f,.2f,1.f}),half=Unit(key+V{0,0,1});
 metallic=std::clamp(metallic,0.f,1.f);roughness=std::clamp(roughness,.045f,1.f);
 const float diffuse=.30f+.55f*std::max(0.f,Dot(n,key))+.18f*std::max(0.f,Dot(n,fill));
 const float shine=std::pow(std::max(0.f,Dot(n,half)),std::clamp(2.f/(roughness*roughness)-2.f,2.f,256.f));
 std::array<float,3> result{};for(int k=0;k<3;++k){const float f0=.04f*(1-metallic)+base[k]*metallic;result[k]=EncodeSrgb(base[k]*diffuse*(1-.65f*metallic)+f0*shine*(.30f+.45f*metallic));}
 return result;
}
// An immutable bounded XY index accelerates exact front-shell queries. The
// bounds choose candidate triangles only; they never resize or place a model.
struct FilterSurfaceTriangle {uint32_t index;float minX,maxX,minY,maxY,den;};
struct FilterSurface {
 static constexpr int grid=64;
 std::shared_ptr<const NativeFaceFilter> model;
 std::vector<FilterSurfaceTriangle> triangles;
 std::array<std::vector<uint32_t>,grid*grid> bins;
 float minX=0,minY=0,stepX=1,stepY=1;bool indexed=true;
};
std::shared_ptr<const FilterSurface> FrontSurface(int style,const std::shared_ptr<const NativeFaceFilter>& model){
 struct SurfaceCache {std::mutex mutex;std::array<std::shared_ptr<const FilterSurface>,3> values;};
 static SurfaceCache cache;std::lock_guard<std::mutex> lock(cache.mutex);
 auto& existing=cache.values[size_t(style-1)];if(existing&&existing->model==model)return existing;
 auto out=std::make_shared<FilterSurface>();out->model=model;
 float minX=std::numeric_limits<float>::infinity(),minY=minX,maxX=-minX,maxY=-minX;
 for(uint32_t index=0;index<model->triangles.size();++index){const auto& t=model->triangles[index];if(model->materials[t.material].blend)continue;
  const auto& a=model->vertices[t.a];const auto& b=model->vertices[t.b];const auto& c=model->vertices[t.c];
  if(std::max({a.z,b.z,c.z})<model->authoredEyeDepthMeters-.03f)continue;
  const float den=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);if(std::abs(den)<1e-12f)continue;
  FilterSurfaceTriangle tri{index,std::min({a.x,b.x,c.x}),std::max({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::max({a.y,b.y,c.y}),den};
  out->triangles.push_back(tri);minX=std::min(minX,tri.minX);maxX=std::max(maxX,tri.maxX);minY=std::min(minY,tri.minY);maxY=std::max(maxY,tri.maxY);
 }
 if(!out->triangles.empty()){
  out->minX=minX;out->minY=minY;out->stepX=std::max((maxX-minX)/FilterSurface::grid,1e-6f);out->stepY=std::max((maxY-minY)/FilterSurface::grid,1e-6f);
  size_t references=0;for(uint32_t i=0;i<out->triangles.size();++i){const auto& t=out->triangles[i];
   const int x0=std::clamp(int((t.minX-minX)/out->stepX),0,FilterSurface::grid-1),x1=std::clamp(int((t.maxX-minX)/out->stepX),0,FilterSurface::grid-1),
     y0=std::clamp(int((t.minY-minY)/out->stepY),0,FilterSurface::grid-1),y1=std::clamp(int((t.maxY-minY)/out->stepY),0,FilterSurface::grid-1);
   references+=size_t(x1-x0+1)*size_t(y1-y0+1);
   if(references>500000){out->indexed=false;for(auto& bin:out->bins)std::vector<uint32_t>().swap(bin);break;}
   for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x)out->bins[y*FilterSurface::grid+x].push_back(i);
  }
 }
 existing=out;return existing;
}
float FrontDepth(const FilterSurface& surface,float x,float y){
 float front=-std::numeric_limits<float>::infinity();
 auto query=[&](uint32_t index){const auto& q=surface.triangles[index];if(x<q.minX||x>q.maxX||y<q.minY||y>q.maxY)return;
  const auto& t=surface.model->triangles[q.index];const auto& a=surface.model->vertices[t.a];const auto& b=surface.model->vertices[t.b];const auto& c=surface.model->vertices[t.c];
  const float wa=((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/q.den,
    wb=((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/q.den,wc=1.f-wa-wb;
  if(wa<0||wb<0||wc<0)return;const float z=wa*a.z+wb*b.z+wc*c.z;
  if(z>=surface.model->authoredEyeDepthMeters-.03f)front=std::max(front,z);
 };
 if(surface.indexed){const float px=(x-surface.minX)/surface.stepX,py=(y-surface.minY)/surface.stepY;
  if(px<0||py<0||px>FilterSurface::grid||py>FilterSurface::grid)return front;
  const int ix=std::min(int(px),FilterSurface::grid-1),iy=std::min(int(py),FilterSurface::grid-1);for(uint32_t index:surface.bins[iy*FilterSurface::grid+ix])query(index);
 }else for(uint32_t index=0;index<surface.triangles.size();++index)query(index);
 return front;
}
void Raster(const std::vector<ProjectedVertex>& tris,const ProjectedScene& s,std::vector<float>& depth,std::vector<float>& rgba,bool face,bool translucent){
 const float sx=float(s.rasterWidth)/s.width,sy=float(s.rasterHeight)/s.height;
 for(size_t i=0;i+2<tris.size();i+=3){auto a=tris[i],b=tris[i+1],c=tris[i+2];a.x=(a.x-s.left)*sx;a.y=(a.y-s.top)*sy;b.x=(b.x-s.left)*sx;b.y=(b.y-s.top)*sy;c.x=(c.x-s.left)*sx;c.y=(c.y-s.top)*sy;
  const float den=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);if(std::abs(den)<1e-7f)continue;
  int left=int(std::clamp(std::floor(std::min({a.x,b.x,c.x})),0.f,float(s.rasterWidth-1))),right=int(std::clamp(std::ceil(std::max({a.x,b.x,c.x})),0.f,float(s.rasterWidth-1)));
  int top=int(std::clamp(std::floor(std::min({a.y,b.y,c.y})),0.f,float(s.rasterHeight-1))),bottom=int(std::clamp(std::ceil(std::max({a.y,b.y,c.y})),0.f,float(s.rasterHeight-1)));
  for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){float px=x+.5f,py=y+.5f,wa=((b.y-c.y)*(px-c.x)+(c.x-b.x)*(py-c.y))/den,wb=((c.y-a.y)*(px-c.x)+(a.x-c.x)*(py-c.y))/den,wc=1-wa-wb;if(wa<0||wb<0||wc<0)continue;
   float z=wa*a.depth+wb*b.depth+wc*c.depth;size_t at=size_t(y)*s.rasterWidth+x;if(z<0||z>1||z>depth[at]+1e-7f)continue;
   if(face){depth[at]=z;continue;}
   float inv=wa/a.distance+wb/b.distance+wc/c.distance;wa=wa/a.distance/inv;wb=wb/b.distance/inv;wc=wc/c.distance/inv;
   float alpha=std::clamp(wa*a.a+wb*b.a+wc*c.a,0.f,1.f);float color[3]={wa*a.r+wb*b.r+wc*c.r,wa*a.g+wb*b.g+wc*c.g,wa*a.b+wb*b.b+wc*c.b};
   if(a.textured==1&&s.capTexture){const auto texel=SampleCapTextureLinear(*s.capTexture,wa*a.u+wb*b.u+wc*c.u,wa*a.v+wb*b.v+wc*c.v);alpha*=texel[3];for(int k=0;k<3;++k)color[k]=EncodeSrgb(color[k]*texel[k]);}
   if(a.textured>=2){
    const float u=wa*a.u+wb*b.u+wc*c.u,v=wa*a.v+wb*b.v+wc*c.v;
    float metal=wa*a.metallic+wb*b.metallic+wc*c.metallic,rough=wa*a.roughness+wb*b.roughness+wc*c.roughness;
    if(a.textured==2&&s.filterBaseTexture){const auto texel=SampleCapTextureLinear(*s.filterBaseTexture,u,v);alpha*=texel[3];for(int k=0;k<3;++k)color[k]*=texel[k];}
    if(a.textured==2&&s.filterMetalRoughTexture){const auto mr=SampleCapTexture(*s.filterMetalRoughTexture,u,v);rough*=mr[1];metal*=mr[2];}
    const auto lit=FilterShade({wa*a.nx+wb*b.nx+wc*c.nx,wa*a.ny+wb*b.ny+wc*c.ny,wa*a.nz+wb*b.nz+wc*c.nz},color,metal,rough);
    for(int k=0;k<3;++k)color[k]=lit[k];
   }
   if(alpha<=1.f/255.f)continue;if(!translucent)depth[at]=z;
   for(int k=0;k<3;++k)rgba[at*4+k]=color[k]*alpha+rgba[at*4+k]*(1-alpha);rgba[at*4+3]=alpha+rgba[at*4+3]*(1-alpha);
  }
 }
}
}
bool SampleFaceFilterFrontDepthForTesting(int style,float x,float y,float& depth){
 depth=-std::numeric_limits<float>::infinity();if(style<1||style>3||!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>2.f||std::abs(y)>2.f)return false;
 try{std::string error;auto model=LoadFaceFilter(style,error);if(!model||!ValidateFaceFilter(*model,error))return false;depth=FrontDepth(*FrontSurface(style,model),x,y);return true;}catch(...){return false;}
}
bool RasterizeProjectedSceneForTesting(const ProjectedScene& scene,std::vector<float>& rgba){
 rgba.clear();if(scene.width<=0||scene.height<=0||scene.rasterWidth<=0||scene.rasterHeight<=0||scene.rasterWidth>int(HardModelRenderEdge)||scene.rasterHeight>int(HardModelRenderEdge)||uint64_t(scene.rasterWidth)*scene.rasterHeight>uint64_t(HardModelRenderEdge)*HardModelRenderEdge)return false;
 uint64_t vertices=0;for(const auto* list:{&scene.faces,&scene.opaque,&scene.lenses}){if(uint64_t(list->size())>uint64_t(HardModelTriangles)*3+8192-vertices||list->size()%3)return false;vertices+=list->size();for(const auto& v:*list){for(float x:{v.x,v.y,v.depth,v.distance,v.r,v.g,v.b,v.a,v.u,v.v,v.textured,v.nx,v.ny,v.nz,v.metallic,v.roughness})if(!std::isfinite(x))return false;if(v.distance<=0||v.textured<0||v.textured>3)return false;}}
 try{std::vector<float> depth(size_t(scene.rasterWidth)*scene.rasterHeight,1.f);rgba.assign(depth.size()*4,0);Raster(scene.faces,scene,depth,rgba,true,false);Raster(scene.opaque,scene,depth,rgba,false,false);Raster(scene.lenses,scene,depth,rgba,false,true);return true;}catch(...){rgba.clear();return false;}
}
std::shared_ptr<const TextureRGBA> LoadCapTexture(int style,std::string& error){
 error.clear();if(style<1||style>2){error="Invalid textured cap style";return {};}
 auto& cache=Cache();std::lock_guard<std::mutex> guard(cache.mutex);const size_t index=size_t(style-1);
 if(cache.attempted[index]){error=cache.errors[index];return cache.snapshots[index];}
 cache.attempted[index]=true;
 auto fail=[&](const char* reason)->std::shared_ptr<const TextureRGBA>{cache.errors[index]=reason;error=reason;return {};};
 try{
  std::filesystem::path root(cache.root);
  if(root.empty()){std::array<wchar_t,32768> executable{};const DWORD n=GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
   if(!n||n>=executable.size())return fail("Cannot locate cap assets beside executable");root=std::filesystem::path(executable.data()).parent_path()/L"res"/L"accessories";}
  const auto path=root/(style==1?L"white_tennis_cap.pfrgba":L"pokemon_masters_cap.pfrgba");
  Handle file;file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(file.value==INVALID_HANDLE_VALUE)return fail("Selected cap texture is missing");
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
  if(!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||GetFileType(file.value)!=FILE_TYPE_DISK||!GetFileSizeEx(file.value,&length)||length.QuadPart<12||length.QuadPart>int64_t(2048)*2048*4+12)return fail("Invalid bounded cap texture file");
  std::array<uint8_t,12> header{};DWORD read=0;
  if(!ReadFile(file.value,header.data(),DWORD(header.size()),&read,nullptr)||read!=header.size()||std::memcmp(header.data(),"PFC1",4))return fail("Invalid cap texture header");
  const uint32_t w=LE32(header.data()+4),h=LE32(header.data()+8);const uint64_t bytes=uint64_t(w)*h*4;
  if(!w||!h||w>2048||h>2048||bytes>uint64_t(2048)*2048*4||uint64_t(length.QuadPart)!=bytes+12)return fail("Invalid cap texture dimensions or extent");
  auto texture=std::make_shared<TextureRGBA>();texture->width=w;texture->height=h;texture->pixels.resize(size_t(bytes));
  if(!ReadFile(file.value,texture->pixels.data(),DWORD(bytes),&read,nullptr)||read!=bytes)return fail("Cap texture could not be read completely");
  cache.snapshots[index]=std::move(texture);return cache.snapshots[index];
 }catch(...){return fail("Could not allocate bounded cap texture");}
}
bool SetCapAssetRootForTesting(const std::wstring& root){
 try{if(!root.empty()){const std::filesystem::path path(root);if(root.size()>32700||!path.is_absolute()||!std::filesystem::is_directory(path)||std::filesystem::is_symlink(path))return false;}
  auto& cache=Cache();std::lock_guard<std::mutex> guard(cache.mutex);cache.root=root;cache.snapshots={};cache.attempted={};cache.errors={};return true;
 }catch(...){return false;}
}
std::array<float,4> SampleCapTexture(const TextureRGBA& texture,float u,float v){
 std::array<float,4> out{};if(!texture.width||!texture.height||texture.width>2048||texture.height>2048||texture.pixels.size()!=size_t(texture.width)*texture.height*4||!std::isfinite(u)||!std::isfinite(v))return out;
 const float x=std::clamp(u,0.f,1.f)*texture.width-.5f,y=std::clamp(v,0.f,1.f)*texture.height-.5f;
 const int ix=int(std::floor(x)),iy=int(std::floor(y));const float tx=x-ix,ty=y-iy;
 for(int j=0;j<2;++j)for(int i=0;i<2;++i){const int px=std::clamp(ix+i,0,int(texture.width)-1),py=std::clamp(iy+j,0,int(texture.height)-1);const float weight=(i?tx:1-tx)*(j?ty:1-ty)/255.f;
  for(int k=0;k<4;++k)out[k]+=texture.pixels[(size_t(py)*texture.width+px)*4+k]*weight;}
 return out;
}
std::array<float,4> SampleCapTextureLinear(const TextureRGBA& texture,float u,float v){
 std::array<float,4> out{};if(!texture.width||!texture.height||texture.width>2048||texture.height>2048||texture.pixels.size()!=size_t(texture.width)*texture.height*4||!std::isfinite(u)||!std::isfinite(v))return out;
 const float x=std::clamp(u,0.f,1.f)*texture.width-.5f,y=std::clamp(v,0.f,1.f)*texture.height-.5f;
 const int ix=int(std::floor(x)),iy=int(std::floor(y));const float tx=x-ix,ty=y-iy;const auto& decoded=LinearSrgbBytes();
 for(int j=0;j<2;++j)for(int i=0;i<2;++i){const int px=std::clamp(ix+i,0,int(texture.width)-1),py=std::clamp(iy+j,0,int(texture.height)-1);const float weight=(i?tx:1-tx)*(j?ty:1-ty),alphaWeight=weight/255.f;
  const auto* pixel=texture.pixels.data()+(size_t(py)*texture.width+px)*4;
  for(int k=0;k<3;++k)out[k]+=decoded[pixel[k]]*weight;out[3]+=pixel[3]*alphaWeight;}
 return out;
}
bool BuildProjectedScene(const AnalysisBridge::FaceLandmarkBundle* lm,int w,int h,bool glasses,bool cap,float size,ProjectedScene& out,std::string& error,int capStyle,int glassesStyle,float capScale,float capVerticalOffset,int faceFilterStyle,float faceFilterScale,float faceFilterVerticalOffset,const std::shared_ptr<const NativeFaceFilter>& customModel,const CustomFaceFilterFit& customFit,uint32_t renderEdge){
 out={};error.clear();if(glassesStyle<0||glassesStyle>3){error="Invalid glasses style";return false;}if(!std::isfinite(capScale)||capScale<.75f||capScale>1.5f||!std::isfinite(capVerticalOffset)||capVerticalOffset<-2.f||capVerticalOffset>3.f){error="Cap fit outside supported bounds";return false;}if(capStyle<0||capStyle>2){error="Invalid cap style";return false;}if(faceFilterStyle<0||faceFilterStyle>5||!std::isfinite(faceFilterScale)||faceFilterScale<.75f||faceFilterScale>1.5f||!std::isfinite(faceFilterVerticalOffset)||faceFilterVerticalOffset<-3.f||faceFilterVerticalOffset>3.f){out.filterFailure=FilterBuildFailure::InvalidRequest;error="Face filter fit outside supported bounds";return false;}if(faceFilterStyle&&(renderEdge<256||renderEdge>HardModelRenderEdge)){out.filterFailure=FilterBuildFailure::InvalidRequest;error="Face filter raster budget outside supported bounds";return false;}if(faceFilterStyle==4&&!ValidateCustomFaceFilterFit(customFit,error)){out.filterFailure=FilterBuildFailure::InvalidRequest;return false;}if(!glasses&&!cap&&!faceFilterStyle)return true;
 if(!std::isfinite(size)||size<.5f||size>1.5f){error="Accessory size outside supported bounds";return false;}
 if(!lm||!lm->NativeGeometryMatches(w,h,lm->geometrySourceGeneration,lm->geometrySequenceId,lm->geometryTimestampPts)||!lm->accessoryMesh||
  !lm->accessoryMesh->Matches(w,h,lm->geometrySourceGeneration,lm->geometrySequenceId,lm->geometryTimestampPts)){error="Current native3D face geometry is unavailable";return false;}
 try{
 if(cap&&capStyle){out.capTexture=LoadCapTexture(capStyle,error);if(!out.capTexture)return false;}
 const auto& g=*lm->accessoryMesh;const auto& m=g.transform;
 V rx{m[0],m[4],m[8]},ry{m[1],m[5],m[9]},rz{m[2],m[6],m[10]};float nx=std::sqrt(Dot(rx,rx)),ny=std::sqrt(Dot(ry,ry)),nz=std::sqrt(Dot(rz,rz));
 if(nx<.5f||nx>2||ny<.5f||ny>2||nz<.5f||nz>2){error="Invalid native pose scale";return false;}
 rx=Unit(rx);ry=Unit(ry-rx*Dot(rx,ry));V normal=Unit(Cross(rx,ry));if(Dot(normal,Unit(rz))<.8f){error="Invalid native rigid pose";return false;}rz=normal;
 const auto& left=g.vertices[468];const auto& right=g.vertices[473];const auto& bridge=g.vertices[168];float dx=(right.x-left.x)*w,dy=(right.y-left.y)*h,dz=(right.z-left.z)*w,span=std::sqrt(dx*dx+dy*dy+dz*dz);
 if(span<4||!std::isfinite(span)){error="Invalid pupil baseline";return false;}
 out.focalPixels=h/(2.f*std::tan(3.14159265358979323846f/8.f));out.faceDistance=std::clamp(out.focalPixels*6.3f/span,18.f,160.f);
 V anchor=ToWorld(bridge,w,h,out.focalPixels,out.faceDistance,bridge.z);float clearance=.45f;
 for(int index:{6,197,195,5,98,327}){V p=ToWorld(g.vertices[index],w,h,out.focalPixels,out.faceDistance,bridge.z)-anchor;V local{Dot(p,rx),Dot(p,ry),Dot(p,rz)};if(std::abs(local.x)<1.8f&&local.y>-2.8f&&local.y<.8f)clearance=std::max(clearance,local.z+.25f);}
 out.noseClearance=std::clamp(clearance,.45f,1.5f);anchor=anchor+rz*out.noseClearance;
 // Fit the reference model to the current pupil centers, without changing its
 // rigid orientation or moving the rims through the nasal clearance plane.
 const V eyeMid=(ToWorld(left,w,h,out.focalPixels,out.faceDistance,bridge.z)+
                 ToWorld(right,w,h,out.focalPixels,out.faceDistance,bridge.z))*.5f;
 const float glassesUnits=14.f*size*(glassesStyle?ReferenceGlassesMaterials(glassesStyle).scaleMultiplier:1.f);
 const float lensCenterZ=-.055f*(.255f/.5f)*(.255f/.5f)-.004f;
 const V modeledEye=anchor+Rotate({0,-.10f,lensCenterZ},rx,ry,rz)*glassesUnits;
 const V fitDelta=eyeMid-modeledEye;
 const V glassesAnchor=anchor+rx*std::clamp(Dot(fitDelta,rx),-1.5f,1.5f)+
                              ry*std::clamp(Dot(fitDelta,ry),-1.5f,1.5f);
 float minX=float(w),minY=float(h),maxX=0,maxY=0;
 auto append=[&](const Mesh& model,V origin,bool textured=false,V capUnits=V{1,1,1},const GlassesMaterialSet* glassesMaterial=nullptr){
  if(!ValidateAccessoryMesh(model)){error="Invalid bounded accessory mesh";return false;}
  // Each model vertex has one material. Project and shade it once, then expand
  // the indexed triangle list for the CPU/GPU common scene.
  std::vector<Material> materials(model.vertices.size(),Material::Frame);
  for(const auto& t:model.triangles)for(uint32_t i:{t.a,t.b,t.c})materials[i]=t.material;
  std::vector<ProjectedVertex> projected;projected.reserve(model.vertices.size());
  for(size_t i=0;i<model.vertices.size();++i){const auto& v=model.vertices[i];
   // Keep the existing glasses/legacy path bit-identical. Imported caps fit
   // the measured crown independently; normals follow its instance transform.
   V p=textured?origin+Rotate({v.x*capUnits.x,v.y*capUnits.y,v.z*capUnits.z},rx,ry,rz):origin+Rotate({v.x,v.y,v.z},rx,ry,rz)*(glassesMaterial?glassesUnits:14.f*size);
   if(-p.z<1.1f||-p.z>249.f){error="Accessory crosses perspective range";return false;}
   auto vertex=Project(p,Rotate(textured?V{v.nx/capUnits.x,v.ny/capUnits.y,v.nz/capUnits.z}:V{v.nx,v.ny,v.nz},rx,ry,rz),materials[i],v.u,v.v,w,h,out.focalPixels,textured,glassesMaterial);
   projected.push_back(vertex);minX=std::min(minX,vertex.x);maxX=std::max(maxX,vertex.x);
   minY=std::min(minY,vertex.y);maxY=std::max(maxY,vertex.y);
  }
  for(const auto& t:model.triangles){auto& target=(t.material==Material::Lens||t.material==Material::NosePad)?out.lenses:out.opaque;
   for(uint32_t i:{t.a,t.b,t.c})target.push_back(projected[i]);
  }return true;
 };
 if(glasses&&!append(ReferenceGlassesMesh(glassesStyle),glassesAnchor,false,{1,1,1},glassesStyle?&ReferenceGlassesMaterials(glassesStyle):nullptr))return false;
 if(cap){
 const Mesh& model=ReferenceCapMesh(capStyle);
 const V forehead=ToWorld(g.vertices[10],w,h,out.focalPixels,out.faceDistance,bridge.z);
 if(!capStyle){ // Retain the legacy procedural calibration exactly.
  V hat=forehead+ry*.75f+rz*(.1f-.50f*14.f*size);
  if(!append(model,hat))return false;
 }else{
  // Native eyes set camera distance; pose-local temple widths set one
  // uniform scale. Forehead height only anchors the brim and head proxy;
  // it never compresses authored crown volume or stretches one mesh axis.
  std::vector<float> widths;
  for(const auto& pair:std::array<std::array<int,2>,4>{{{{54,284}},{{103,332}},{{127,356}},{{234,454}}}}){
   const V a=ToWorld(g.vertices[pair[0]],w,h,out.focalPixels,out.faceDistance,bridge.z)-eyeMid;
   const V b=ToWorld(g.vertices[pair[1]],w,h,out.focalPixels,out.faceDistance,bridge.z)-eyeMid;
   const float width=std::abs(Dot(b-a,rx));const V mid=(a+b)*.5f;
   if(width>=8.f&&width<=23.f&&std::abs(Dot(mid,rx))<=2.5f&&std::abs(Dot(mid,ry))<=8.f&&std::abs(Dot(a-b,ry))<=3.f&&std::abs(Dot(a-b,rz))<=6.f)widths.push_back(width);
  }
  std::sort(widths.begin(),widths.end());const float measured=widths.empty()?6.3f*2.25f:widths[widths.size()/2];
  const float headWidth=std::clamp(measured,11.f,21.f);
  const float foreheadHeight=std::clamp(Dot(forehead-eyeMid,ry),1.8f,5.5f);
  float minModelX=10,maxModelX=-10,minModelY=10,maxModelY=-10,minFrontY=10;
  for(const auto& v:model.vertices){minModelX=std::min(minModelX,v.x);maxModelX=std::max(maxModelX,v.x);minModelY=std::min(minModelY,v.y);maxModelY=std::max(maxModelY,v.y);if(v.z>=.45f)minFrontY=std::min(minFrontY,v.y);}
  if(!std::isfinite(minFrontY)||minFrontY>maxModelY||maxModelX-minModelX<.5f||maxModelY-minFrontY<.1f){error="Cap model has no usable crown/bill calibration";return false;}
  // Apply exactly the same metric units to X/Y/Z and the user's cap size.
  // Original GLB proportions, surface curvature, UVs and normals stay intact.
  const float uniform=std::clamp(headWidth*1.06f/(maxModelX-minModelX),9.f,23.f)*capScale;
  // The authored central front band is local(y0,z+.5). The lowest front
  // point belongs to the far lateral brim, not the wearing anchor; lifting
  // it to forehead height would float the central band several cm above hair.
  // Translate the authored band directly, then enforce the pupil safety floor.
  float raise=capVerticalOffset-.35f;
  const float eyeHeight=Dot(forehead-eyeMid,ry);
  raise=std::max(raise,.45f-eyeHeight-minModelY*uniform);
  const V hat=forehead+ry*raise+rz*(.15f-.50f*uniform);
  out.capFit={true,true,uint32_t(widths.size()),headWidth,foreheadHeight,uniform,uniform,(maxModelY-minFrontY)*uniform,eyeHeight+raise+minModelY*uniform};
  if(!append(model,hat,true,{uniform,uniform,uniform}))return false;
  // A bounded depth-only skull approximation can occlude cap geometry
  // behind the head; this is not head/hair segmentation or visor-wrap proof.
  // Its foremost surface stays behind the current pupil plane, so it cannot
  // hide glasses rims placed in front even if forehead depth is noisy.
  const V center=forehead-ry*(foreheadHeight*.70f)-rz*(headWidth*.48f)-
                 rz*std::max(0.f,Dot(forehead-eyeMid,rz)+.1f);
  const V radii{headWidth*.47f,foreheadHeight*.95f+headWidth*.10f,headWidth*.48f};
  constexpr int rows=12,columns=24;constexpr float pi=3.14159265358979323846f;
  auto point=[&](int row,int column){const float latitude=-pi*.5f+pi*row/rows,longitude=2*pi*column/columns;
   const V local{radii.x*std::cos(latitude)*std::cos(longitude),radii.y*std::sin(latitude),radii.z*std::cos(latitude)*std::sin(longitude)};
   const V p=center+Rotate(local,rx,ry,rz);return Project(p,{0,0,1},Material::Frame,0,0,w,h,out.focalPixels);};
  for(int row=0;row<rows;++row)for(int column=0;column<columns;++column){const auto a=point(row,column),b=point(row,column+1),c=point(row+1,column+1),d=point(row+1,column);
   for(const auto& v:{a,b,c,a,c,d}){if(!std::isfinite(v.x+v.y+v.depth+v.distance)||v.distance<1.1f||v.distance>249.f){error="Cap head proxy outside perspective bounds";return false;}out.faces.push_back(v);}}
 }
 }

 if(faceFilterStyle){
  const auto model=faceFilterStyle==4?customModel:LoadFaceFilter(faceFilterStyle,error);if(!model&&error.empty())error="Local model is not loaded";if(!model||!ValidateFaceFilter(*model,error)){out.filterFailure=FilterBuildFailure::AssetUnavailable;return false;}
  out.filterBaseTexture=model->baseTexture;out.filterMetalRoughTexture=model->metalRoughTexture;
  if(out.filterBaseTexture&&!out.filterMetalRoughTexture){static const auto neutral=std::make_shared<const TextureRGBA>(TextureRGBA{1,1,{255,255,255,255}});out.filterMetalRoughTexture=neutral;}
  // Authored metric eye anchors, not model bounds, align to the observed eyes.
  // One scale preserves crown depth, eye spacing and all authored proportions.
  const float units=100.f*(.063f/model->authoredIpdMeters)*faceFilterScale;
  const bool crown=faceFilterStyle==5;
  const auto surface=(faceFilterStyle==4||crown)?std::shared_ptr<const FilterSurface>{}:FrontSurface(faceFilterStyle,model);
  const bool localModel=faceFilterStyle==4;
  auto userRotate=[&](V v){const float r=3.14159265358979323846f/180.f;const float cx=std::cos(customFit.tiltDegrees*r),sx=std::sin(customFit.tiltDegrees*r),cy=std::cos(customFit.turnDegrees*r),sy=std::sin(customFit.turnDegrees*r),cz=std::cos(customFit.rollDegrees*r),sz=std::sin(customFit.rollDegrees*r);v={v.x,cx*v.y-sx*v.z,sx*v.y+cx*v.z};v={cy*v.x+sy*v.z,v.y,-sy*v.x+cy*v.z};return V{cz*v.x-sz*v.y,sz*v.x+cz*v.y,v.z};};
  const V fx=localModel?Rotate(userRotate({1,0,0}),rx,ry,rz):rx,fy=localModel?Rotate(userRotate({0,1,0}),rx,ry,rz):ry,fz=localModel?Rotate(userRotate({0,0,1}),rx,ry,rz):rz;
  if(localModel&&!customFit.headOcclusion)out.faces.clear();
  // Rigid masks must sit outside the measured face surface. A fixed bridge
  // offset leaves the brow/nose intersecting a shallow authored shell. Query
  // its closed front triangles at stable forehead/nose/cheek landmarks, then
  // translate the entire model forward; never deform or stretch its vertices.
  // Rear helmet surfaces and the transparent visor must not push a helmet
  // away from the wearer when its front portal intentionally has no shell.
  constexpr float minClearance=.35f,maxClearance=4.5f;
  float forward=(localModel||crown)?0.f:minClearance;uint32_t surfaceSamples=0;
  // The private crown bakes its accepted size/offset about the pupil midpoint.
  // Anchor translation preserves the original mesh, including its crown volume.
  const V targetEye=eyeMid+ry*faceFilterVerticalOffset+(localModel?Rotate({customFit.xCm,customFit.yCm,customFit.zCm},rx,ry,rz):V{});
  const float eyePlaneDistance=Dot(targetEye,rz);
  if(!std::isfinite(eyePlaneDistance)||eyePlaneDistance>-5.f){error="Face filter eye plane is outside stable fit range";return false;}
  V translation{};bool clearanceLimited=false;
  constexpr int probes[]={10,151,9,8,168,6,197,195,5,4,1,19,94,2,164,
    0,11,12,13,14,15,16,17,18,200,199,175,152,109,67,103,54,21,71,
    68,104,69,108,299,297,332,284,251,301,298,333,337,50,101,205,
    425,330,280,100,203,423,329,36,266,207,427};
  // Forward clearance changes perspective registration under yaw/pitch.
  // Keep the authored eye midpoint on the camera ray through the observed
  // pupil midpoint (plus the requested height), solving only translation.
  // Re-query the shell after each correction because a rigid side shift can
  // bring a different authored shell section over the same skin landmark.
  for(int pass=0;surface&&pass<4;++pass){
   translation=targetEye*(forward/eyePlaneDistance);
   float required=minClearance;surfaceSamples=0;
   for(int index:probes){
   const V delta=ToWorld(g.vertices[index],w,h,out.focalPixels,out.faceDistance,bridge.z)-eyeMid;
   const V local{Dot(delta-translation,rx),Dot(delta-translation,ry)-faceFilterVerticalOffset,Dot(delta,rz)};
   if(std::abs(local.x)>8.5f||local.y<-10.f||local.y>7.5f||local.z<-4.f||local.z>6.f)continue;
   const float x=local.x/units,y=local.y/units+model->authoredEyeHeightMeters;
   const float front=FrontDepth(*surface,x,y);
   if(std::isfinite(front)){++surfaceSamples;required=std::max(required,local.z-(front-model->authoredEyeDepthMeters)*units+minClearance);}
   }
   if(required<=forward+.005f)break;
   if(required>maxClearance){clearanceLimited=true;forward=maxClearance;break;}
   forward=required;
  }
  translation=targetEye*(forward/eyePlaneDistance);
  out.filterForwardClearanceCm=forward;out.filterClearanceLimited=clearanceLimited;out.filterSurfaceSamples=surfaceSamples;
  const V origin=targetEye+translation-Rotate({0,model->authoredEyeHeightMeters,model->authoredEyeDepthMeters},fx,fy,fz)*units;
  out.filterMetricScale=units;out.filterAnchorCm={origin.x,origin.y,origin.z};
  std::vector<ProjectedVertex> projected;projected.reserve(model->vertices.size());
  for(const auto& v:model->vertices){
   const V p=origin+Rotate({v.x,v.y,v.z},fx,fy,fz)*units;if(-p.z<1.1f||-p.z>249.f){error="Face filter crosses perspective range";return false;}
   auto vertex=Project(p,{0,0,1},Material::Frame,v.u,v.v,w,h,out.focalPixels);const V n=Unit(Rotate({v.nx,v.ny,v.nz},fx,fy,fz));
   vertex.nx=n.x;vertex.ny=n.y;vertex.nz=n.z;vertex.u=v.u;vertex.v=v.v;projected.push_back(vertex);
   minX=std::min(minX,vertex.x);maxX=std::max(maxX,vertex.x);minY=std::min(minY,vertex.y);maxY=std::max(maxY,vertex.y);
  }
  for(const auto& triangle:model->triangles){const auto& material=model->materials[triangle.material];auto& target=material.blend?out.lenses:out.opaque;
   for(uint32_t index:{triangle.a,triangle.b,triangle.c}){auto vertex=projected[index];vertex.r=material.baseColor[0];vertex.g=material.baseColor[1];vertex.b=material.baseColor[2];vertex.a=material.blend?material.baseColor[3]:1.f;vertex.metallic=material.metallic;vertex.roughness=material.roughness;vertex.textured=material.textured?2.f:3.f;target.push_back(vertex);}
  }
  // Bounded depth-only skull keeps back surfaces behind the wearer. It is an
  // approximation, not hair segmentation or a deformable head-wrap model.
  if(!localModel||customFit.headOcclusion){
  float headWidth=14.2f;std::vector<float> widths;for(const auto& pair:std::array<std::array<int,2>,4>{{{{54,284}},{{103,332}},{{127,356}},{{234,454}}}}){const V a=ToWorld(g.vertices[pair[0]],w,h,out.focalPixels,out.faceDistance,bridge.z),b=ToWorld(g.vertices[pair[1]],w,h,out.focalPixels,out.faceDistance,bridge.z);const float value=std::abs(Dot(b-a,rx));if(value>=8.f&&value<=23.f)widths.push_back(value);}
  if(!widths.empty()){std::sort(widths.begin(),widths.end());headWidth=std::clamp(widths[widths.size()/2],11.f,21.f);}
  const V center=eyeMid+ry*1.f-rz*(headWidth*.50f),radii{headWidth*.47f,headWidth*.62f,headWidth*.48f};
  constexpr int rows=12,columns=24;constexpr float pi=3.14159265358979323846f;
  auto point=[&](int row,int column){const float latitude=-pi*.5f+pi*row/rows,longitude=2*pi*column/columns;const V local{radii.x*std::cos(latitude)*std::cos(longitude),radii.y*std::sin(latitude),radii.z*std::cos(latitude)*std::sin(longitude)};return Project(center+Rotate(local,rx,ry,rz),{0,0,1},Material::Frame,0,0,w,h,out.focalPixels);};
  for(int row=0;row<rows;++row)for(int column=0;column<columns;++column){const auto a=point(row,column),b=point(row,column+1),c=point(row+1,column+1),d=point(row+1,column);for(const auto& v:{a,b,c,a,c,d}){if(!std::isfinite(v.x+v.y+v.depth+v.distance)||v.distance<1.1f||v.distance>249.f){error="Face filter head proxy outside perspective range";return false;}out.faces.push_back(v);}}
  }
  // Blend surfaces are sorted in camera depth, so rear visor triangles cannot
  // paint over the front surface. The opaque head/shell depth remains read-only.
  std::vector<size_t> order(out.lenses.size()/3);for(size_t i=0;i<order.size();++i)order[i]=i;
  std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return out.lenses[a*3].distance+out.lenses[a*3+1].distance+out.lenses[a*3+2].distance>out.lenses[b*3].distance+out.lenses[b*3+1].distance+out.lenses[b*3+2].distance;});
  std::vector<ProjectedVertex> sorted;sorted.reserve(out.lenses.size());for(size_t i:order)for(int j=0;j<3;++j)sorted.push_back(out.lenses[i*3+j]);out.lenses.swap(sorted);
 }

 if(!std::isfinite(minX+minY+maxX+maxY)||out.opaque.size()+out.lenses.size()>uint64_t(HardModelTriangles)*3){error="Projected mesh exceeds supported bounds";return false;}
 out.left=int(std::clamp(std::floor(minX)-3,0.f,float(w)));out.top=int(std::clamp(std::floor(minY)-3,0.f,float(h)));int r=int(std::clamp(std::ceil(maxX)+3,0.f,float(w))),b=int(std::clamp(std::ceil(maxY)+3,0.f,float(h)));out.width=r-out.left;out.height=b-out.top;
 if(out.width<=0||out.height<=0){error="Accessory is outside the camera view";return false;}
 // Filter quality scales only the offscreen raster. 16/25 preserves the exact legacy
 // 640-edge/262144-pixel budget; glasses/caps retain it independently of filter mode.
 const uint32_t edge=faceFilterStyle?renderEdge:640u;const uint64_t pixelBudget=uint64_t(edge)*edge*16/25;
 float down=std::min({1.f,float(edge)/out.width,float(edge)/out.height,std::sqrt(float(pixelBudget)/(float(out.width)*out.height))});out.rasterWidth=std::max(1,int(out.width*down));out.rasterHeight=std::max(1,int(out.height*down));
 if(faceFilterStyle!=4||customFit.headOcclusion)for(const auto& tri:g.triangles)for(uint32_t index:tri){V p=ToWorld(g.vertices[index],w,h,out.focalPixels,out.faceDistance,bridge.z);if(-p.z<1.1f||-p.z>249.f){error="Face depth crosses perspective range";return false;}auto v=Project(p,{0,0,1},Material::Frame,0,0,w,h,out.focalPixels);out.faces.push_back(v);}
 if(out.faces.size()+out.opaque.size()+out.lenses.size()>uint64_t(HardModelTriangles)*3+8192){out={};error="Combined3D scene exceeds bounded vertex capacity";return false;}
 return true;
 }catch(...){out={};error="Could not prepare bounded3D accessories";return false;}
}
FaceAccessory3DRenderer::~FaceAccessory3DRenderer(){Reset();}
void FaceAccessory3DRenderer::Reset(){av_frame_free(&output);depth.clear();rgba.clear();lastError.clear();}
AVFrame* FaceAccessory3DRenderer::ProcessFrame(AVFrame* input,const AnalysisBridge::FaceLandmarkBundle* lm,bool glasses,bool cap,float size,OverlayOutcome* outcome,LayerReport* layers,int capStyle,int glassesStyle,float capScale,float capVerticalOffset,int faceFilterStyle,float faceFilterScale,float faceFilterVerticalOffset,const std::shared_ptr<const NativeFaceFilter>& customModel,const CustomFaceFilterFit& customFit,uint32_t renderEdge){
 lastError.clear();if(outcome)*outcome=OverlayOutcome::Bypass;if(layers)*layers={glasses?2:1,cap?2:1,1,faceFilterStyle?2:1};if(!glasses&&!cap&&!faceFilterStyle)return input;
 if(!ValidPlane(input)){lastError="Invalid native3D camera input plane";if(outcome)*outcome=OverlayOutcome::InvalidInput;if(layers)*layers={glasses?3:1,cap?3:1,1,faceFilterStyle?3:1};return nullptr;}
 ProjectedScene scene;std::string error;if(!BuildProjectedScene(lm,input->width,input->height,glasses,cap,size,scene,error,capStyle,glassesStyle,capScale,capVerticalOffset,faceFilterStyle,faceFilterScale,faceFilterVerticalOffset,customModel,customFit,renderEdge)){lastError=error;if(outcome)*outcome=scene.filterFailure==FilterBuildFailure::None?OverlayOutcome::NotReady:OverlayOutcome::InvalidInput;if(layers&&scene.filterFailure!=FilterBuildFailure::None)layers->faceFilter=3;return input;}
 try{
  size_t pixels=size_t(scene.rasterWidth)*scene.rasterHeight;depth.assign(pixels,1.f);rgba.assign(pixels*4,0);
  Raster(scene.faces,scene,depth,rgba,true,false);Raster(scene.opaque,scene,depth,rgba,false,false);Raster(scene.lenses,scene,depth,rgba,false,true);
  // Fresh properties and COW pixels keep retained preview/virtual/snapshot frames stable.
  AVFrame* next=av_frame_alloc();if(!next)throw std::bad_alloc();next->format=AV_PIX_FMT_BGR24;next->width=input->width;next->height=input->height;
  if(av_frame_get_buffer(next,32)<0||av_frame_copy_props(next,input)<0){av_frame_free(&next);throw std::bad_alloc();}
  for(int y=0;y<input->height;++y)std::memcpy(next->data[0]+size_t(y)*next->linesize[0],input->data[0]+size_t(y)*input->linesize[0],size_t(input->width)*3);
  for(int y=scene.top;y<scene.top+scene.height;++y)for(int x=scene.left;x<scene.left+scene.width;++x){float fx=(x-scene.left+.5f)*scene.rasterWidth/scene.width-.5f,fy=(y-scene.top+.5f)*scene.rasterHeight/scene.height-.5f;int ix=int(std::floor(fx)),iy=int(std::floor(fy));float tx=fx-ix,ty=fy-iy,c[4]{};
   for(int j=0;j<2;++j)for(int i=0;i<2;++i){int px=std::clamp(ix+i,0,scene.rasterWidth-1),py=std::clamp(iy+j,0,scene.rasterHeight-1);float weight=(i?tx:1-tx)*(j?ty:1-ty);for(int k=0;k<4;++k)c[k]+=rgba[(size_t(py)*scene.rasterWidth+px)*4+k]*weight;}
   auto* dst=next->data[0]+size_t(y)*next->linesize[0]+x*3;for(int k=0;k<3;++k)dst[k]=uint8_t(std::clamp(int(std::lround(c[2-k]*255+dst[k]*(1-c[3]))),0,255));
  }
  av_frame_free(&output);output=next;if(outcome)*outcome=OverlayOutcome::Success;if(layers)*layers={glasses?0:1,cap?0:1,1,faceFilterStyle?0:1};return output;
 }catch(...){lastError="Native3D output allocation failed";if(outcome)*outcome=OverlayOutcome::OutOfMemory;if(layers)*layers={glasses?3:1,cap?3:1,1,faceFilterStyle?3:1};return nullptr;}
}
}
