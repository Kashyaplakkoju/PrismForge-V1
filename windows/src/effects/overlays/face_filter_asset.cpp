#include "face_filter_asset.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
namespace PrismForge::Effects::Overlays::Realistic {
namespace {
constexpr uint64_t MaxFile=HardModelFileBytes;
constexpr uint32_t MaxVertices=HardModelVertices,MaxTriangles=HardModelTriangles,MaxMaterials=HardModelMaterials,MaxTexture=HardModelTexture;
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
float F32(const uint8_t* p){uint32_t bits=U32(p);float out;std::memcpy(&out,&bits,4);return out;}
bool Finite(float v,float limit){return std::isfinite(v)&&std::abs(v)<=limit;}
bool TextureValid(const std::shared_ptr<const TextureRGBA>& t){
 return !t||(t->width&&t->height&&t->width<=MaxTexture&&t->height<=MaxTexture&&t->pixels.size()==uint64_t(t->width)*t->height*4);
}
struct AssetCache {
 std::mutex mutex;std::wstring root;
 std::array<std::shared_ptr<const NativeFaceFilter>,4> snapshots{};
 std::array<bool,4> attempted{};std::array<std::string,4> errors{};
};
AssetCache& Cache(){static AssetCache cache;return cache;}
bool PlainDirectory(const std::filesystem::path& path){
 DWORD a=GetFileAttributesW(path.c_str());
 return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);
}
}
bool ValidateFaceFilter(const NativeFaceFilter& m,std::string& error){
 error.clear();auto fail=[&](const char* s){error=s;return false;};
 if(m.vertices.empty()||m.vertices.size()>MaxVertices||m.triangles.empty()||m.triangles.size()>MaxTriangles||
    m.materials.empty()||m.materials.size()>MaxMaterials)return fail("Filter geometry exceeds supported bounds");
 if(!Finite(m.authoredIpdMeters,.12f)||m.authoredIpdMeters<.035f)return fail("Invalid authored pupil spacing");
 if(!Finite(m.authoredEyeHeightMeters,.2f)||!Finite(m.authoredEyeDepthMeters,.2f))return fail("Invalid authored pupil anchor");
 if(!TextureValid(m.baseTexture)||!TextureValid(m.metalRoughTexture))return fail("Invalid bounded filter texture");
 if(m.metalRoughTexture&&!m.baseTexture)return fail("Metallic-roughness image requires base texture");
 for(const auto& v:m.vertices){
  for(float x:{v.x,v.y,v.z})if(!Finite(x,2.f))return fail("Invalid metric filter position");
  for(float x:{v.nx,v.ny,v.nz})if(!Finite(x,2.f))return fail("Invalid filter normal");
  float n=v.nx*v.nx+v.ny*v.ny+v.nz*v.nz;if(n<.25f||n>4.f)return fail("Invalid filter normal length");
  if(!Finite(v.u,10000.f)||!Finite(v.v,10000.f))return fail("Invalid filter texture coordinate");
 }
 for(const auto& material:m.materials){
  for(float x:material.baseColor)if(!std::isfinite(x)||x<0||x>1)return fail("Invalid filter base colour");
  for(float x:{material.metallic,material.roughness})if(!std::isfinite(x)||x<0||x>1)return fail("Invalid filter material factor");
  if(material.textured&&!m.baseTexture)return fail("Filter material requires missing base texture");
  if(!material.blend&&material.baseColor[3]!=1.f)return fail("Opaque filter material has nonopaque factor");
 }
 for(const auto& t:m.triangles)if(t.a>=m.vertices.size()||t.b>=m.vertices.size()||t.c>=m.vertices.size()||
   t.material>=m.materials.size())return fail("Filter triangle references missing geometry or material");
 return true;
}
bool ValidateCustomFaceFilterFit(const CustomFaceFilterFit& fit,std::string& error){
 error.clear();for(float value:{fit.xCm,fit.yCm,fit.zCm})if(!std::isfinite(value)||std::abs(value)>20.f){error="Local model position is outside -20 to 20 cm";return false;}
 for(float value:{fit.tiltDegrees,fit.turnDegrees,fit.rollDegrees})if(!std::isfinite(value)||std::abs(value)>180.f){error="Local model rotation is outside -180 to 180 degrees";return false;}
 return true;
}
std::shared_ptr<const NativeFaceFilter> LoadFaceFilterFile(const std::wstring& filename,std::string& error){
 error.clear();auto fail=[&](const char* message)->std::shared_ptr<const NativeFaceFilter>{error=message;return {};};
 try{
  std::filesystem::path path(filename);
  if(filename.empty()||filename.size()>32700||!path.is_absolute()||!PlainDirectory(path.parent_path()))return fail("Invalid local model library path");
  Handle file;
  file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(file.value==INVALID_HANDLE_VALUE)return fail("Selected 3D face filter is missing");
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
  if(!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
     GetFileType(file.value)!=FILE_TYPE_DISK||!GetFileSizeEx(file.value,&length)||length.QuadPart<52||uint64_t(length.QuadPart)>MaxFile)
   return fail("Invalid bounded 3D filter asset file");
  std::array<uint8_t,52> header{};DWORD read=0;
  if(!ReadFile(file.value,header.data(),DWORD(header.size()),&read,nullptr)||read!=header.size()||
     std::memcmp(header.data(),"PFM1",4)||U32(header.data()+4)!=1)return fail("Invalid 3D filter header");
  uint32_t nv=U32(header.data()+8),nt=U32(header.data()+12),nm=U32(header.data()+16);
  uint32_t bw=U32(header.data()+20),bh=U32(header.data()+24),mw=U32(header.data()+28),mh=U32(header.data()+32);
  uint32_t payload=U32(header.data()+36);float ipd=F32(header.data()+40);
  auto dims=[](uint32_t w,uint32_t h){return (w==0&&h==0)||(w&&h&&w<=MaxTexture&&h<=MaxTexture);};
  if(!nv||nv>MaxVertices||!nt||nt>MaxTriangles||!nm||nm>MaxMaterials||!dims(bw,bh)||!dims(mw,mh)||(mw&&!bw))
   return fail("Invalid 3D filter counts or dimensions");
  uint64_t expected=uint64_t(nv)*32+uint64_t(nt)*16+uint64_t(nm)*32+uint64_t(bw)*bh*4+uint64_t(mw)*mh*4;
  if(expected>MaxFile-52||payload!=expected||uint64_t(length.QuadPart)!=expected+52)return fail("Invalid exact 3D filter payload extent");
  std::vector<uint8_t> bytes(size_t(expected),uint8_t{});
  if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())return fail("3D filter could not be read completely");
  auto model=std::make_shared<NativeFaceFilter>();model->authoredIpdMeters=ipd;model->authoredEyeHeightMeters=F32(header.data()+44);model->authoredEyeDepthMeters=F32(header.data()+48);size_t offset=0;
  model->vertices.resize(nv);for(auto& v:model->vertices){float f[8];for(size_t j=0;j<8;++j)f[j]=F32(bytes.data()+offset+j*4);v={f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]};offset+=32;}
  model->triangles.resize(nt);for(auto& t:model->triangles){auto* p=bytes.data()+offset;t={U32(p),U32(p+4),U32(p+8),U32(p+12)};offset+=16;}
  model->materials.resize(nm);for(auto& m:model->materials){
   auto* p=bytes.data()+offset;uint32_t flags=U32(p+24);
   if(flags&~3u||U32(p+28))return fail("Unsupported 3D filter material flags");
   for(size_t j=0;j<4;++j)m.baseColor[j]=F32(p+j*4);m.metallic=F32(p+16);m.roughness=F32(p+20);
   m.blend=bool(flags&1);m.textured=bool(flags&2);offset+=32;
  }
  auto image=[&](uint32_t w,uint32_t h)->std::shared_ptr<const TextureRGBA>{
   if(!w)return {};auto t=std::make_shared<TextureRGBA>();t->width=w;t->height=h;size_t count=size_t(w)*h*4;
   t->pixels.assign(bytes.begin()+offset,bytes.begin()+offset+count);offset+=count;return t;
  };
  model->baseTexture=image(bw,bh);model->metalRoughTexture=image(mw,mh);
  std::string why;if(offset!=bytes.size()||!ValidateFaceFilter(*model,why)){error=why.empty()?"Invalid 3D filter payload":why;return {};}
  return model;
 }catch(...){return fail("Could not load bounded 3D face filter");}
}
std::shared_ptr<const NativeFaceFilter> LoadFaceFilter(int style,std::string& error){
 error.clear();if(style<1||style>5||style==4){error="Invalid built-in 3D face filter style";return {};}
 auto& cache=Cache();std::lock_guard<std::mutex> guard(cache.mutex);size_t index=style==5?3:size_t(style-1);
 if(cache.attempted[index]){error=cache.errors[index];return cache.snapshots[index];}
 cache.attempted[index]=true;
 try{
  std::filesystem::path root(cache.root);
  if(root.empty()){
   std::array<wchar_t,32768> exe{};DWORD n=GetModuleFileNameW(nullptr,exe.data(),DWORD(exe.size()));
   if(!n||n>=exe.size()){error="Cannot locate filter assets beside executable";cache.errors[index]=error;return {};}
   root=std::filesystem::path(exe.data()).parent_path()/L"res"/L"face-filters";
  }
  if(!PlainDirectory(root)){error="Filter asset directory is missing or reparse-linked";cache.errors[index]=error;return {};}
  const wchar_t* names[]={L"aurora-fox-mask.pfmodel",L"nocturne-owl-mask.pfmodel",L"atlas-explorer-helmet.pfmodel",L"royal-crown.pfmodel"};
  cache.snapshots[index]=LoadFaceFilterFile((root/names[index]).wstring(),error);if(style==5&&!cache.snapshots[index])error="Royal crown asset unavailable: "+error;cache.errors[index]=error;return cache.snapshots[index];
 }catch(...){error="Could not load bounded 3D face filter";cache.errors[index]=error;return {};}
}
bool SetFaceFilterAssetRootForTesting(const std::wstring& root){
 try{
  if(!root.empty()){std::filesystem::path p(root);if(root.size()>32700||!p.is_absolute()||!PlainDirectory(p))return false;}
  auto& cache=Cache();std::lock_guard<std::mutex> guard(cache.mutex);
  cache.root=root;cache.snapshots={};cache.attempted={};cache.errors={};return true;
 }catch(...){return false;}
}
}
