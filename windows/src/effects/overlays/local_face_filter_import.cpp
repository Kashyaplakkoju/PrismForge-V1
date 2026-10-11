#include "local_face_filter_import.h"
#include "../packs/sha256.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <array>
#define CGLTF_IMPLEMENTATION
#include "../../../3rdparty/cgltf/cgltf.h"
namespace PrismForge::Effects::Overlays::Realistic {
namespace {
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr size_t MaxBytes=size_t(HardModelFileBytes), MaxParse=256u*1024*1024;
struct Failure:std::runtime_error {using std::runtime_error::runtime_error;};
void Need(bool ok,const char* why){if(!ok)throw Failure(why);}
void Limit(size_t count,size_t maximum,const char* kind){if(count>maximum)throw Failure(std::string("GLB has ")+std::to_string(count)+" "+kind+"; maximum "+std::to_string(maximum)+".");}
void CheckBudget(const ModelResourceBudget& b){
 auto positive=[](uint64_t count,uint64_t maximum,const char* name){Need(count>0,(std::string("Model budget ")+name+" must be positive").c_str());if(count>maximum)throw Failure(std::string("Model budget ")+name+" is "+std::to_string(count)+"; hard maximum "+std::to_string(maximum)+".");};
 positive(b.nodes,16384,"nodes");positive(b.meshes,4096,"meshes");positive(b.primitives,4096,"primitives");positive(b.vertices,HardModelVertices,"vertices");positive(b.triangles,HardModelTriangles,"triangles");positive(b.materials,HardModelMaterials,"materials");Limit(b.images,64,"budget images");Limit(b.textures,64,"budget textures");positive(b.accessorElements,3000000,"accessor elements");positive(b.fileBytes,HardModelFileBytes,"file bytes");Need(b.fileBytes>=28,"Model budget file bytes must be at least 28");positive(b.parseBytes,MaxParse,"parse bytes");Need(b.renderEdge>=256&&b.renderEdge<=HardModelRenderEdge,"Model budget render edge must be within 256..1536");Need(int(b.resolvedMode)>=int(ModelPerformanceMode::Performance)&&int(b.resolvedMode)<=int(ModelPerformanceMode::Custom),"Model budget must have a resolved performance mode");
}
bool FallbackMaterial(const char* name){return name&&(std::strcmp(name,"KHR_materials_clearcoat")==0||std::strcmp(name,"KHR_materials_transmission")==0||std::strcmp(name,"KHR_materials_volume")==0||std::strcmp(name,"KHR_materials_ior")==0||std::strcmp(name,"KHR_materials_specular")==0);}
struct ExtensionNames {
 std::vector<std::string> names;bool omitted=false;
 void Add(const char* raw){if(!raw)return;std::string name;for(size_t i=0;raw[i]&&i<64;++i){unsigned char c=static_cast<unsigned char>(raw[i]);name+=(c>=32&&c<=126)?char(c):'?';}if(raw[name.size()])name+="...";for(auto& n:names)if(n==name)return;if(names.size()<6)names.push_back(std::move(name));else omitted=true;}
 std::string Text()const{std::string out;for(auto& n:names){if(!out.empty())out+=", ";out+=n;}if(omitted)out+=", ...";return out;}
 bool Empty()const{return names.empty()&&!omitted;}
};
void RejectExtensions(const ExtensionNames& list,const char* context){if(list.Empty())return;bool material=true,compression=false;for(auto& n:list.names){material&=n.compare(0,14,"KHR_materials_")==0;compression|=n=="KHR_draco_mesh_compression"||n=="EXT_meshopt_compression"||n=="KHR_texture_basisu"||n=="EXT_texture_webp";}throw Failure(std::string("Unsupported ")+(compression?"compressed geometry/texture":(material?"material":"GLB"))+" extension(s) in "+context+": "+list.Text()+". Export standard embedded metallic-roughness GLB.");}
void RejectExtensions(cgltf_extension* extensions,size_t count,const char* context){ExtensionNames list;for(size_t i=0;i<count;++i)list.Add(extensions[i].name);RejectExtensions(list,context);}
void CheckDeclaredExtensions(const cgltf_data& data,ExtensionNames& fallback){ExtensionNames unsupported;auto add=[&](const char* name){if(FallbackMaterial(name))fallback.Add(name);else unsupported.Add(name);};for(size_t i=0;i<data.extensions_required_count;++i)add(data.extensions_required[i]);for(size_t i=0;i<data.extensions_used_count;++i)add(data.extensions_used[i]);RejectExtensions(unsupported,"model declarations");RejectExtensions(data.data_extensions,data.data_extensions_count,"root");}
void CheckMaterial(const cgltf_material& src,ExtensionNames& fallback){ExtensionNames unsupported;for(size_t i=0;i<src.extensions_count;++i)unsupported.Add(src.extensions[i].name);
 if(src.has_clearcoat)fallback.Add("KHR_materials_clearcoat");if(src.has_transmission)fallback.Add("KHR_materials_transmission");if(src.has_volume)fallback.Add("KHR_materials_volume");if(src.has_ior)fallback.Add("KHR_materials_ior");if(src.has_specular)fallback.Add("KHR_materials_specular");
 if(src.has_pbr_specular_glossiness)unsupported.Add("KHR_materials_pbrSpecularGlossiness");if(src.has_sheen)unsupported.Add("KHR_materials_sheen");if(src.has_emissive_strength)unsupported.Add("KHR_materials_emissive_strength");if(src.has_iridescence)unsupported.Add("KHR_materials_iridescence");if(src.has_anisotropy)unsupported.Add("KHR_materials_anisotropy");if(src.has_diffuse_transmission)unsupported.Add("KHR_materials_diffuse_transmission");if(src.has_dispersion)unsupported.Add("KHR_materials_dispersion");if(src.unlit)unsupported.Add("KHR_materials_unlit");RejectExtensions(unsupported,"material");
}
using Matrix=std::array<float,16>;
Matrix Identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
void Affine(const float* m,const char* why){for(size_t i=0;i<16;++i)Need(std::isfinite(m[i])&&std::abs(m[i])<=1e8f,why);Need(std::abs(m[3])<=1e-5f&&std::abs(m[7])<=1e-5f&&std::abs(m[11])<=1e-5f&&std::abs(m[15]-1)<=1e-5f,why);}
Matrix Multiply(const Matrix& a,const Matrix& b){Matrix out{};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)out[c*4+r]+=a[k*4+r]*b[c*4+k];Affine(out.data(),"Invalid rest-pose skin matrix");return out;}
std::vector<Matrix> SkinMatrices(const cgltf_skin* skin){std::vector<Matrix> out;if(!skin)return out;Need(skin->joints_count>0,"Skin has 0 joints; export a valid rest-pose rig");Limit(skin->joints_count,512,"skin joints");RejectExtensions(skin->extensions,skin->extensions_count,"skin");auto* bind=skin->inverse_bind_matrices;Need(!bind||(bind->type==cgltf_type_mat4&&bind->component_type==cgltf_component_type_r_32f&&bind->count==skin->joints_count),"Skin inverse-bind matrices must be float MAT4 with one matrix per joint");out.reserve(skin->joints_count);
 for(size_t i=0;i<skin->joints_count;++i){Need(skin->joints[i],"Skin references a missing joint");Matrix world{},inverse=Identity();cgltf_node_transform_world(skin->joints[i],world.data());Affine(world.data(),"Invalid authored rest joint transform");if(bind)Need(cgltf_accessor_read_float(bind,i,inverse.data(),16),"Cannot read skin inverse-bind matrix");Affine(inverse.data(),"Invalid skin inverse-bind matrix");out.push_back(Multiply(world,inverse));}return out;
}
float TransformVertex(const float* m,float* xyz,float* normal,bool hasNormal){Affine(m,"Invalid GLB vertex transform");float a=m[0],b=m[4],c=m[8],e=m[1],f=m[5],g=m[9],h=m[2],j=m[6],k=m[10];float co[9]={f*k-g*j,g*h-e*k,e*j-f*h,c*j-b*k,a*k-c*h,b*h-a*j,b*g-c*f,c*e-a*g,a*f-b*e};float det=a*co[0]+b*co[1]+c*co[2];Need(std::isfinite(det)&&std::abs(det)>1e-15f,"Authored rest pose has a singular vertex transform; bake a nonsingular mesh before importing");float p[3]={m[0]*xyz[0]+m[4]*xyz[1]+m[8]*xyz[2]+m[12],m[1]*xyz[0]+m[5]*xyz[1]+m[9]*xyz[2]+m[13],m[2]*xyz[0]+m[6]*xyz[1]+m[10]*xyz[2]+m[14]};std::copy_n(p,3,xyz);
 if(hasNormal){float n[3]={(co[0]*normal[0]+co[1]*normal[1]+co[2]*normal[2])/det,(co[3]*normal[0]+co[4]*normal[1]+co[5]*normal[2])/det,(co[6]*normal[0]+co[7]*normal[1]+co[8]*normal[2])/det};float len=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);Need(std::isfinite(len)&&len>1e-12f,"Invalid rest-pose normal length");for(int q=0;q<3;++q)normal[q]=n[q]/len;}return det;
}
void Cancel(const std::atomic<bool>* flag){Need(!flag||!flag->load(),"Local model import cancelled");}
struct File {HANDLE h=INVALID_HANDLE_VALUE;~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool EqualPath(const fs::path& a,const fs::path& b){return _wcsicmp(a.lexically_normal().c_str(),b.lexically_normal().c_str())==0;}
void PlainParents(const fs::path& path,bool create){
 Need(path.is_absolute()&&path.wstring().size()<32700,"Choose an absolute local path");
 // Walk from the volume root so existing junctions cannot redirect library creation.
 fs::path current=path.root_path();
 for(const auto& part:path.relative_path()){
  Need(part!=L".."&&part!=L".","Relative path components are unsupported");current/=part;
  DWORD attrs=GetFileAttributesW(current.c_str());
  if(attrs==INVALID_FILE_ATTRIBUTES&&create){Need(CreateDirectoryW(current.c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS,"Cannot create the local model library");attrs=GetFileAttributesW(current.c_str());}
  Need(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY)&&!(attrs&FILE_ATTRIBUTE_REPARSE_POINT),"Local model path contains a missing directory or reparse link");
 }
}
std::vector<uint8_t> Read(const fs::path& path,size_t cap){
 PlainParents(path.parent_path(),false);File f;
 f.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
 Need(f.h!=INVALID_HANDLE_VALUE,"Cannot open the local model file");BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER n{};
 Need(GetFileInformationByHandle(f.h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileType(f.h)==FILE_TYPE_DISK&&GetFileSizeEx(f.h,&n)&&n.QuadPart>0,"The model file is empty, linked or invalid");Limit(size_t(n.QuadPart),cap,"file bytes");
 std::vector<uint8_t> bytes(size_t(n.QuadPart),uint8_t{});DWORD got=0;Need(ReadFile(f.h,bytes.data(),DWORD(bytes.size()),&got,nullptr)&&got==bytes.size(),"The model could not be read completely");return bytes;
}
struct Budget {size_t cap=0,used=0,peak=0,denied=0;};
struct alignas(std::max_align_t) Allocation {size_t size;};
void* Allocate(void* user,cgltf_size size){auto& b=*static_cast<Budget*>(user);if(size>SIZE_MAX-sizeof(Allocation)){b.denied=SIZE_MAX;return nullptr;}size_t total=size+sizeof(Allocation);if(total>b.cap-b.used){b.denied=b.used+total;return nullptr;}auto* p=static_cast<Allocation*>(std::malloc(total));if(!p)return nullptr;p->size=total;b.used+=total;b.peak=std::max(b.peak,b.used);return p+1;}
void Free(void* user,void* ptr){if(ptr){auto* p=static_cast<Allocation*>(ptr)-1;static_cast<Budget*>(user)->used-=p->size;std::free(p);}}
struct Parsed {cgltf_data* data=nullptr;~Parsed(){if(data)cgltf_free(data);}};
struct Com {HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~Com(){if(SUCCEEDED(hr))CoUninitialize();}};
std::shared_ptr<TextureRGBA> Decode(IWICImagingFactory* wic,const cgltf_texture_view& view){
 if(!view.texture)return {};
 Need(view.texcoord==0&&!view.has_transform,"Use UV0 without texture transforms for local textures");auto* t=view.texture;
 Need(!t->has_basisu&&!t->has_webp&&!t->extensions_count&&t->image,"Only embedded PNG or JPEG textures are supported");auto* im=t->image;
 Need(!im->uri&&im->buffer_view&&im->mime_type&&!im->extensions_count,"Texture images must be embedded in the GLB");
 bool png=std::strcmp(im->mime_type,"image/png")==0,jpg=std::strcmp(im->mime_type,"image/jpeg")==0;
 Need(png||jpg,"Texture images must be PNG or JPEG");auto* v=im->buffer_view;const auto* raw=static_cast<const uint8_t*>(v->buffer->data)+v->offset;
 Need(v->size>8&&v->size<=MaxBytes,"Invalid embedded texture extent");
 Need((png&&std::memcmp(raw,"\x89PNG\r\n\x1a\n",8)==0)||(jpg&&raw[0]==255&&raw[1]==216),"Embedded texture signature disagrees with its type");
 ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
 Need(SUCCEEDED(wic->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(raw),DWORD(v->size)))&&SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)),"Cannot decode embedded texture");
 UINT frames=0,w=0,h=0;GUID container{};
 Need(SUCCEEDED(decoder->GetContainerFormat(&container))&&((png&&IsEqualGUID(container,GUID_ContainerFormatPng))||(jpg&&IsEqualGUID(container,GUID_ContainerFormatJpeg)))&&SUCCEEDED(decoder->GetFrameCount(&frames))&&frames==1&&SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(frame->GetSize(&w,&h))&&w&&h&&w<=2048&&h<=2048,"Texture exceeds 2048 pixels or contains multiple frames");
 auto out=std::make_shared<TextureRGBA>();out->width=w;out->height=h;out->pixels.resize(size_t(w)*h*4);
 Need(SUCCEEDED(wic->CreateFormatConverter(&converter))&&SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))&&SUCCEEDED(converter->CopyPixels(nullptr,w*4,UINT(out->pixels.size()),out->pixels.data())),"Cannot read complete embedded texture pixels");return out;
}
void Tile(TextureRGBA& atlas,const TextureRGBA* src,unsigned tile,unsigned columns,unsigned side){
 unsigned tx=(tile%columns)*side,ty=(tile/columns)*side;
 for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
  auto* out=&atlas.pixels[(size_t(ty+y)*atlas.width+tx+x)*4];
  if(!src){std::fill(out,out+4,uint8_t(255));continue;}
  // Clamp a two-pixel gutter around each material's UV rectangle.
  float u=std::clamp((float(x)-2.f)/float(side-5),0.f,1.f),v=std::clamp((float(y)-2.f)/float(side-5),0.f,1.f);
  unsigned sx=unsigned(std::round(u*(src->width-1))),sy=unsigned(std::round(v*(src->height-1)));
  std::copy_n(&src->pixels[(size_t(sy)*src->width+sx)*4],4,out);
 }
}
void PutU(std::vector<uint8_t>& b,uint32_t n){for(int j=0;j<4;++j)b.push_back(uint8_t(n>>(8*j)));}
void PutF(std::vector<uint8_t>& b,float f){uint32_t n;std::memcpy(&n,&f,4);PutU(b,n);}
std::vector<uint8_t> Encode(const NativeFaceFilter& m){
 auto dims=[](const std::shared_ptr<const TextureRGBA>& t,bool width){return t?(width?t->width:t->height):0u;};
 std::vector<uint8_t>b={'P','F','M','1'};PutU(b,1);PutU(b,uint32_t(m.vertices.size()));PutU(b,uint32_t(m.triangles.size()));PutU(b,uint32_t(m.materials.size()));
 PutU(b,dims(m.baseTexture,true));PutU(b,dims(m.baseTexture,false));PutU(b,dims(m.metalRoughTexture,true));PutU(b,dims(m.metalRoughTexture,false));
 uint64_t payload=m.vertices.size()*32+m.triangles.size()*16+m.materials.size()*32+(m.baseTexture?m.baseTexture->pixels.size():0)+(m.metalRoughTexture?m.metalRoughTexture->pixels.size():0);
 Need(payload<=HardModelFileBytes-52,"Converted model exceeds the native format limit");PutU(b,uint32_t(payload));PutF(b,m.authoredIpdMeters);PutF(b,m.authoredEyeHeightMeters);PutF(b,m.authoredEyeDepthMeters);b.reserve(size_t(payload)+52);
 for(auto& v:m.vertices)for(float f:{v.x,v.y,v.z,v.nx,v.ny,v.nz,v.u,v.v})PutF(b,f);
 for(auto& t:m.triangles)for(uint32_t n:{t.a,t.b,t.c,t.material})PutU(b,n);
 for(auto& mat:m.materials){for(float f:mat.baseColor)PutF(b,f);PutF(b,mat.metallic);PutF(b,mat.roughness);PutU(b,(mat.blend?1u:0)|(mat.textured?2u:0));PutU(b,0);}
 for(auto t:{m.baseTexture,m.metalRoughTexture})if(t)b.insert(b.end(),t->pixels.begin(),t->pixels.end());return b;
}
std::string Utf8(const std::wstring& value){int n=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);std::string out(size_t(n),' ');if(n)WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),out.data(),n,nullptr,nullptr);return out;}
void Publish(const fs::path& final,const std::vector<uint8_t>& bytes,const std::atomic<bool>* cancel){
 if(GetFileAttributesW(final.c_str())!=INVALID_FILE_ATTRIBUTES){Need(Read(final,size_t(HardModelFileBytes))==bytes,"Existing local model copy failed its content check");return;}
 fs::path temp;File file;
 for(unsigned attempt=0;attempt<20;++attempt){temp=final.parent_path()/(L".import-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt)+L".tmp");file.h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file.h!=INVALID_HANDLE_VALUE)break;}
 Need(file.h!=INVALID_HANDLE_VALUE,"Cannot stage the converted model");
 try{DWORD wrote=0;Need(WriteFile(file.h,bytes.data(),DWORD(bytes.size()),&wrote,nullptr)&&wrote==bytes.size()&&FlushFileBuffers(file.h),"Cannot write complete converted model");CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;Cancel(cancel);
  if(!MoveFileExW(temp.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH)){Need(GetLastError()==ERROR_ALREADY_EXISTS||GetLastError()==ERROR_FILE_EXISTS,"Cannot publish the converted model");Need(Read(final,size_t(HardModelFileBytes))==bytes,"Concurrent local model copy failed its content check");DeleteFileW(temp.c_str());}
 }catch(...){if(file.h!=INVALID_HANDLE_VALUE){CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;}DeleteFileW(temp.c_str());throw;}
}
}
bool ImportLocalFaceFilter(const std::wstring& source,const std::wstring& library,LocalFaceFilterImportResult& result,std::string& error,const std::atomic<bool>* cancel){
 ModelPerformanceSettings settings;settings.mode=ModelPerformanceMode::Balanced;return ImportLocalFaceFilter(source,library,result,error,ResolveModelResourceBudget(settings),cancel);
}
bool ImportLocalFaceFilter(const std::wstring& source,const std::wstring& library,LocalFaceFilterImportResult& result,std::string& error,const ModelResourceBudget& resource,const std::atomic<bool>* cancel){
 error.clear();try{
  Cancel(cancel);CheckBudget(resource);fs::path input(source),root(library);Need(input.is_absolute()&&_wcsicmp(input.extension().c_str(),L".glb")==0,"Choose a self-contained static .glb file");
  auto bytes=Read(input,size_t(resource.fileBytes));Need(bytes.size()>=28&&U32(bytes.data())==0x46546c67&&U32(bytes.data()+4)==2&&U32(bytes.data()+8)==bytes.size(),"Invalid exact GLB2 file length or header");
  uint32_t jsonBytes=U32(bytes.data()+12);Need(jsonBytes&&jsonBytes<=4u*1024*1024&&jsonBytes%4==0&&jsonBytes<=bytes.size()-20&&U32(bytes.data()+16)==0x4e4f534a,"Invalid bounded GLB JSON chunk");
  size_t binHeader=20+size_t(jsonBytes);Need(binHeader+8<=bytes.size()&&U32(bytes.data()+binHeader+4)==0x004e4942&&U32(bytes.data()+binHeader)%4==0&&size_t(U32(bytes.data()+binHeader))+binHeader+8==bytes.size(),"GLB must contain exactly one embedded binary chunk");
  Budget budget{size_t(resource.parseBytes)};Parsed parsed;cgltf_options options{};options.type=cgltf_file_type_glb;options.memory={Allocate,Free,&budget};
  auto parsedResult=cgltf_parse(&options,bytes.data(),bytes.size(),&parsed.data);if(parsedResult!=cgltf_result_success&&budget.denied)throw Failure("GLB parser requires "+std::to_string(budget.denied)+" live allocation bytes; budget maximum "+std::to_string(resource.parseBytes)+" (peak "+std::to_string(budget.peak)+").");Need(parsedResult==cgltf_result_success,"Cannot parse bounded GLB2 structure");auto* d=parsed.data;
  Need(d->buffers_count==1&&!d->buffers[0].uri&&d->buffers[0].size<=d->bin_size&&d->bin_size-d->buffers[0].size<=3,"GLB must use one embedded buffer");
  ExtensionNames fallback;CheckDeclaredExtensions(*d,fallback);
  Need(d->nodes_count>0,"GLB has 0 nodes; at least 1 is required.");Limit(d->nodes_count,resource.nodes,"nodes");Limit(d->meshes_count,resource.meshes,"meshes");Limit(d->materials_count,resource.materials,"materials");Limit(d->images_count,resource.images,"images");Limit(d->textures_count,resource.textures,"textures");Limit(d->accessors_count,4096,"accessors");Limit(d->buffer_views_count,4096,"buffer views");Limit(d->skins_count,256,"skins");Limit(d->animations_count,256,"animations");
  size_t animationChannels=0,animationSamplers=0;for(size_t i=0;i<d->animations_count;++i){auto& animation=d->animations[i];RejectExtensions(animation.extensions,animation.extensions_count,"animation");animationChannels+=animation.channels_count;animationSamplers+=animation.samplers_count;Limit(animationChannels,4096,"animation channels");Limit(animationSamplers,4096,"animation samplers");for(size_t j=0;j<animation.channels_count;++j)RejectExtensions(animation.channels[j].extensions,animation.channels[j].extensions_count,"animation channel");for(size_t j=0;j<animation.samplers_count;++j)RejectExtensions(animation.samplers[j].extensions,animation.samplers[j].extensions_count,"animation sampler");}
  d->buffers[0].data=const_cast<void*>(d->bin);d->buffers[0].data_free_method=cgltf_data_free_method_none;
  for(size_t i=0;i<d->buffer_views_count;++i){auto& v=d->buffer_views[i];RejectExtensions(v.extensions,v.extensions_count,"buffer view");Need(!v.has_meshopt_compression,"Unsupported compressed geometry extension EXT_meshopt_compression in buffer view");Need(v.buffer==d->buffers&&v.offset<=v.buffer->size&&v.size<=v.buffer->size-v.offset,"Invalid GLB buffer view bounds");}
  for(size_t i=0;i<d->accessors_count;++i){auto& a=d->accessors[i];size_t element=cgltf_calc_size(a.type,a.component_type);Limit(a.count,resource.accessorElements,"accessor elements");RejectExtensions(a.extensions,a.extensions_count,"accessor");Need(a.buffer_view&&!a.is_sparse&&element&&a.stride>=element&&a.stride<=256&&a.offset<=a.buffer_view->size,"Unsupported sparse or invalid GLB accessor");if(a.count)Need(element<=a.buffer_view->size-a.offset&&(a.count-1)<=(a.buffer_view->size-a.offset-element)/a.stride,"GLB accessor exceeds its buffer view");}
  Need(cgltf_validate(d)==cgltf_result_success,"GLB accessor or scene validation failed");
  for(size_t i=0;i<d->images_count;++i)Need(!d->images[i].uri&&d->images[i].buffer_view,"All GLB images must be embedded");
  for(size_t i=0;i<d->nodes_count;++i){auto& n=d->nodes[i];RejectExtensions(n.extensions,n.extensions_count,"node");Need(!n.has_mesh_gpu_instancing,"Unsupported GLB extension EXT_mesh_gpu_instancing in node");Limit(n.weights_count,32,"node morph weights");std::unordered_set<const cgltf_node*> chain;auto* p=&n;while(p){Need(chain.size()<32&&chain.insert(p).second,"GLB node hierarchy is cyclic or too deep");p=p->parent;}}
  Cancel(cancel);auto model=std::make_shared<NativeFaceFilter>();LocalFaceFilterImportResult next;next.sourceSha256=Packs::Sha256::ComputeHex(bytes.data(),bytes.size());next.displayName=Utf8(input.stem().wstring());if(next.displayName.size()>120)next.displayName.resize(120);
  const unsigned nm=unsigned(std::max<size_t>(1,d->materials_count)),columns=nm==1?1:(nm<=4?2:4),rows=(nm+columns-1)/columns,side=2048/columns;
  Com com;Need(SUCCEEDED(com.hr)||com.hr==RPC_E_CHANGED_MODE,"Cannot initialize the local image decoder");ComPtr<IWICImagingFactory> wic;Need(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic))),"Windows image decoder is unavailable");
  auto base=std::make_shared<TextureRGBA>(),mr=std::make_shared<TextureRGBA>();base->width=mr->width=columns*side;base->height=mr->height=rows*side;base->pixels.resize(size_t(base->width)*base->height*4);mr->pixels.resize(base->pixels.size());
  bool ignoredMaps=false,bakedSkin=false,bakedMorph=false,normalizedWeights=false;
  for(unsigned i=0;i<nm;++i){Cancel(cancel);FilterMaterial mat;mat.textured=true;std::shared_ptr<TextureRGBA> a,b;
   if(i<d->materials_count){auto& src=d->materials[i];CheckMaterial(src,fallback);Need(src.alpha_mode!=cgltf_alpha_mode_mask,"Alpha-cutout materials are unsupported; export opaque or blended surfaces");
    std::copy_n(src.pbr_metallic_roughness.base_color_factor,4,mat.baseColor.begin());mat.metallic=src.pbr_metallic_roughness.metallic_factor;mat.roughness=src.pbr_metallic_roughness.roughness_factor;mat.blend=src.alpha_mode==cgltf_alpha_mode_blend;if(!mat.blend)mat.baseColor[3]=1;
    a=Decode(wic.Get(),src.pbr_metallic_roughness.base_color_texture);b=Decode(wic.Get(),src.pbr_metallic_roughness.metallic_roughness_texture);
    ignoredMaps|=src.normal_texture.texture||src.occlusion_texture.texture||src.emissive_texture.texture||src.emissive_factor[0]!=0||src.emissive_factor[1]!=0||src.emissive_factor[2]!=0;
   }
   Tile(*base,a.get(),i,columns,side);Tile(*mr,b.get(),i,columns,side);model->materials.push_back(mat);
  }
  model->baseTexture=base;model->metalRoughTexture=mr;
  std::unordered_set<cgltf_node*> seen;size_t primitives=0;
  std::function<void(cgltf_node*,unsigned)> visit=[&](cgltf_node* node,unsigned depth){Cancel(cancel);Need(node&&depth<32&&seen.insert(node).second,"GLB scene shares or cycles node instances");Matrix world{};cgltf_node_transform_world(node,world.data());Affine(world.data(),"Invalid GLB transform");
   auto skin=SkinMatrices(node->skin);
   if(node->mesh){RejectExtensions(node->mesh->extensions,node->mesh->extensions_count,"mesh");Limit(node->mesh->weights_count,32,"mesh morph weights");Limit(node->mesh->target_names_count,32,"morph target names");
    for(size_t pi=0;pi<node->mesh->primitives_count;++pi){auto& p=node->mesh->primitives[pi];Limit(++primitives,resource.primitives,"primitives");RejectExtensions(p.extensions,p.extensions_count,"primitive");Need(!p.has_draco_mesh_compression,"Unsupported compressed geometry extension KHR_draco_mesh_compression in primitive");Need(p.type==cgltf_primitive_type_triangles&&!p.mappings_count,"Use uncompressed triangle primitives without material variants");Limit(p.targets_count,32,"morph targets");cgltf_accessor *pos=nullptr,*normal=nullptr,*uv=nullptr;std::array<cgltf_accessor*,4> joints{},weights{};
     for(size_t ai=0;ai<p.attributes_count;++ai){auto& at=p.attributes[ai];if(at.type==cgltf_attribute_type_position){Need(!pos,"Duplicate POSITION attribute");pos=at.data;}if(at.type==cgltf_attribute_type_normal){Need(!normal,"Duplicate NORMAL attribute");normal=at.data;}if(at.type==cgltf_attribute_type_texcoord&&at.index==0){Need(!uv,"Duplicate UV0 attribute");uv=at.data;}if(at.type==cgltf_attribute_type_joints||at.type==cgltf_attribute_type_weights){Need(at.index>=0&&at.index<4,"Rest-pose skin supports JOINTS_0..3 / WEIGHTS_0..3; bake additional influence sets before importing");auto& slot=at.type==cgltf_attribute_type_joints?joints[size_t(at.index)]:weights[size_t(at.index)];Need(!slot,"Duplicate skin joint or weight attribute");slot=at.data;}}
     Need(pos&&pos->type==cgltf_type_vec3&&pos->count&&pos->component_type==cgltf_component_type_r_32f,"Model requires nonempty float VEC3 positions");Limit(model->vertices.size()+pos->count,resource.vertices,"vertices");
     if(node->skin){Need(joints[0]&&weights[0],"Skin needs paired JOINTS_0 and WEIGHTS_0 attributes for rest-pose baking");bakedSkin=true;for(size_t si=0;si<4;++si){Need(bool(joints[si])==bool(weights[si]),"Skin joint/weight influence sets must be paired");if(!joints[si])continue;auto* ja=joints[si];auto* wa=weights[si];Need(ja->type==cgltf_type_vec4&&ja->count==pos->count&&!ja->normalized&&(ja->component_type==cgltf_component_type_r_8u||ja->component_type==cgltf_component_type_r_16u),"Skin JOINTS must be unsigned VEC4 with one element per vertex");Need(wa->type==cgltf_type_vec4&&wa->count==pos->count&&((wa->component_type==cgltf_component_type_r_32f&&!wa->normalized)||((wa->component_type==cgltf_component_type_r_8u||wa->component_type==cgltf_component_type_r_16u)&&wa->normalized)),"Skin WEIGHTS must be float or normalized unsigned VEC4 with one element per vertex");}}
     std::vector<std::array<cgltf_accessor*,2>> targets(p.targets_count);const float* morphWeights=node->weights_count?node->weights:node->mesh->weights;size_t morphCount=node->weights_count?node->weights_count:node->mesh->weights_count;Need(!morphCount||morphCount==p.targets_count,"Default morph weight count must match primitive morph targets");
     for(size_t mi=0;mi<p.targets_count;++mi){float weight=morphCount?morphWeights[mi]:0;Need(std::isfinite(weight)&&std::abs(weight)<=1e4f,"Invalid authored default morph weight");for(size_t ai=0;ai<p.targets[mi].attributes_count;++ai){auto& at=p.targets[mi].attributes[ai];Need(at.type==cgltf_attribute_type_position||at.type==cgltf_attribute_type_normal,"Rest-pose morph baking supports POSITION and NORMAL deltas; bake other target attributes before importing");auto& slot=targets[mi][at.type==cgltf_attribute_type_position?0:1];Need(!slot,"Duplicate morph delta attribute");slot=at.data;Need(slot&&slot->type==cgltf_type_vec3&&slot->component_type==cgltf_component_type_r_32f&&slot->count==pos->count,"Morph deltas must be float VEC3 with one element per vertex");}Need(!targets[mi][1]||normal,"Morph NORMAL deltas require a base NORMAL attribute");bakedMorph|=weight!=0;}
     std::vector<bool> reflected(pos->count);
     Need(!normal||(normal->type==cgltf_type_vec3&&normal->count==pos->count),"Invalid normal attribute");Need(!uv||(uv->type==cgltf_type_vec2&&uv->count==pos->count),"Invalid UV0 attribute");
     uint32_t material=p.material?uint32_t(p.material-d->materials):0;Need(material<nm,"Invalid material reference");const bool usesUv=p.material&&(p.material->pbr_metallic_roughness.base_color_texture.texture||p.material->pbr_metallic_roughness.metallic_roughness_texture.texture);if(usesUv)Need(uv,"Textured geometry requires UV0");
     const uint32_t start=uint32_t(model->vertices.size());
     for(size_t vi=0;vi<pos->count;++vi){if((vi&255)==0)Cancel(cancel);float xyz[3]{},n[3]{},t[2]{.5f,.5f};Need(cgltf_accessor_read_float(pos,vi,xyz,3),"Cannot read model vertex");if(normal)Need(cgltf_accessor_read_float(normal,vi,n,3),"Cannot read model normal");if(usesUv)Need(cgltf_accessor_read_float(uv,vi,t,2),"Cannot read model UV");for(float z:xyz)Need(std::isfinite(z)&&std::abs(z)<=1e8,"Invalid model position");for(float z:n)Need(std::isfinite(z),"Invalid model normal");for(float z:t)Need(std::isfinite(z)&&z>=-1e-5f&&z<=1.00001f,"Export UV0 coordinates within 0..1; tiled UVs are unsupported");
      for(size_t mi=0;mi<targets.size();++mi){float weight=morphCount?morphWeights[mi]:0;if(weight==0)continue;for(size_t qi=0;qi<2;++qi)if(targets[mi][qi]){float delta[3]{};Need(cgltf_accessor_read_float(targets[mi][qi],vi,delta,3),"Cannot read authored morph delta");for(size_t q=0;q<3;++q){Need(std::isfinite(delta[q])&&std::abs(delta[q])<=1e8f,"Invalid authored morph delta");(qi?n:xyz)[q]+=weight*delta[q];}}}
      Matrix transform=world;
      if(node->skin){transform.fill(0);float total=0;for(size_t si=0;si<4;++si)if(joints[si]){float jointIds[4]{},influences[4]{};Need(cgltf_accessor_read_float(joints[si],vi,jointIds,4)&&cgltf_accessor_read_float(weights[si],vi,influences,4),"Cannot read authored skin influences");for(size_t q=0;q<4;++q){Need(std::isfinite(jointIds[q])&&jointIds[q]>=0&&jointIds[q]<skin.size()&&std::floor(jointIds[q])==jointIds[q],"Skin JOINTS index exceeds the authored joint count");float weight=influences[q];Need(std::isfinite(weight)&&weight>=0&&weight<=1,"Skin WEIGHTS must be finite within 0..1");total+=weight;const auto& joint=skin[size_t(jointIds[q])];for(size_t qi=0;qi<16;++qi)transform[qi]+=weight*joint[qi];}}Need(std::isfinite(total)&&total>1e-8f,"Skin vertex has zero or invalid total influence weight");normalizedWeights|=std::abs(total-1)>1e-5f;for(float& component:transform)component/=total;}
      reflected[vi]=TransformVertex(transform.data(),xyz,n,bool(normal))<0;
      float u=((material%columns)*side+(usesUv?2+std::clamp(t[0],0.f,1.f)*(side-5):side*.5f))/base->width,v=((material/columns)*side+(usesUv?2+std::clamp(t[1],0.f,1.f)*(side-5):side*.5f))/base->height;
      model->vertices.push_back({xyz[0],xyz[1],xyz[2],n[0],n[1],n[2],u,v});
     }
     size_t count=p.indices?p.indices->count:pos->count;Need(count%3==0,"Triangle index count must be divisible by 3");Limit(model->triangles.size()+count/3,resource.triangles,"triangles");if(p.indices)Need(p.indices->type==cgltf_type_scalar&&(p.indices->component_type==cgltf_component_type_r_8u||p.indices->component_type==cgltf_component_type_r_16u||p.indices->component_type==cgltf_component_type_r_32u),"Invalid triangle index type");
     for(size_t ti=0;ti<count;ti+=3){if((ti&255)==0)Cancel(cancel);uint32_t ids[3];for(int q=0;q<3;++q){size_t id=p.indices?cgltf_accessor_read_index(p.indices,ti+q):ti+q;Need(id<pos->count,"Triangle index is outside its primitive");ids[q]=start+uint32_t(id);}Need(reflected[ids[0]-start]==reflected[ids[1]-start]&&reflected[ids[1]-start]==reflected[ids[2]-start],"Rest-pose skin triangle changes reflection across vertices; bake its mesh before importing");if(reflected[ids[0]-start])std::swap(ids[1],ids[2]);model->triangles.push_back({ids[0],ids[1],ids[2],material});
      if(!normal){auto& v0=model->vertices[ids[0]];auto& v1=model->vertices[ids[1]];auto& v2=model->vertices[ids[2]];float ux=v1.x-v0.x,uy=v1.y-v0.y,uz=v1.z-v0.z,vx=v2.x-v0.x,vy=v2.y-v0.y,vz=v2.z-v0.z;float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;for(auto id:ids){auto& v=model->vertices[id];v.nx+=nx;v.ny+=ny;v.nz+=nz;}}
     }
     if(!normal)for(size_t vi=start;vi<model->vertices.size();++vi){auto& v=model->vertices[vi];float len=std::sqrt(v.nx*v.nx+v.ny*v.ny+v.nz*v.nz);Need(std::isfinite(len)&&len>1e-15f,"Degenerate geometry cannot produce normals");v.nx/=len;v.ny/=len;v.nz/=len;}
    }
   }
   for(size_t i=0;i<node->children_count;++i){Need(node->children[i]->parent==node,"Inconsistent GLB hierarchy");visit(node->children[i],depth+1);}
  };
  auto* scene=d->scene?d->scene:(d->scenes_count?&d->scenes[0]:nullptr);if(scene){Limit(scene->nodes_count,resource.nodes,"scene roots");for(size_t i=0;i<scene->nodes_count;++i){Need(!scene->nodes[i]->parent,"Scene root has a parent");visit(scene->nodes[i],0);}}else for(size_t i=0;i<d->nodes_count;++i)if(!d->nodes[i].parent)visit(&d->nodes[i],0);
  Need(!model->vertices.empty()&&!model->triangles.empty(),"GLB scene has no renderable triangles");
  float lo[3]={FLT_MAX,FLT_MAX,FLT_MAX},hi[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};for(auto& v:model->vertices){float values[]={v.x,v.y,v.z};for(int q=0;q<3;++q){Need(std::isfinite(values[q]),"Invalid transformed vertex");lo[q]=std::min(lo[q],values[q]);hi[q]=std::max(hi[q],values[q]);}}
  float width=hi[0]-lo[0];Need(width>1e-8f&&std::isfinite(width)&&(hi[1]-lo[1])/width<=20&&(hi[2]-lo[2])/width<=20,"Model needs a usable horizontal width facing +Z");
  next.sourceOrigin={(hi[0]+lo[0])*.5f,(hi[1]+lo[1])*.5f,(hi[2]+lo[2])*.5f};next.sourceToMeters=.18f/width;
  bool original=false;const auto& hash=next.sourceSha256;
  if(hash=="5cc99787d917b53289dc0af47d4285a7cc814139d622bd4862d33f69c1b57849"){model->authoredEyeDepthMeters=.014f;original=true;}
  if(hash=="7918c045aa8c65e94b30d70bea191bfd201da2e69935bd125238e96157ac5b2c"){model->authoredIpdMeters=.076f;model->authoredEyeDepthMeters=.007f;original=true;}
  if(hash=="b1c19757ae096dd852d45ddfde7f1468bbd7895e5e6e3e678928fad4e453f926")original=true;
  float front=1,bandY=0,bandZ=0;bool cap=false;
  if(hash=="2ef37cee54fb735323c7813632ea955d5ee586757c8cad7ff6860d4fb6d773f2"){cap=true;bandY=.3106876611709595f;bandZ=1.061342716217041f;}
  if(hash=="0cfc418e8bfda81fab7c59d4b065b7ba12cfdd83bdb4d3376655daf914caf4ba"||hash=="2a155da0da40320b4237912316b2394b604319df892a32368c233a2e877fbf6e"){cap=true;front=-1;bandY=526.87109375f;bandZ=-867.6121826171875f;}
  if(original){next.sourceToMeters=1;next.sourceOrigin={0,0,0};next.preservedAuthoredAnchors=true;}
  if(cap){next.sourceToMeters=.20f/width;next.sourceOrigin[1]=bandY-.058f/next.sourceToMeters;next.sourceOrigin[2]=bandZ-front*.035f/next.sourceToMeters;}
  for(auto& v:model->vertices){v.x=front*(v.x-next.sourceOrigin[0])*next.sourceToMeters;v.y=(v.y-next.sourceOrigin[1])*next.sourceToMeters;v.z=front*(v.z-next.sourceOrigin[2])*next.sourceToMeters;v.nx*=front;v.nz*=front;}
  std::string why;Need(ValidateFaceFilter(*model,why),why.c_str());Cancel(cancel);auto encoded=Encode(*model);std::string content=Packs::Sha256::ComputeHex(encoded.data(),encoded.size());PlainParents(root,true);
  fs::path final=root/(std::wstring(content.begin(),content.end())+L".pfmodel");Publish(final,encoded,cancel);auto loaded=LoadFaceFilterFile(final.wstring(),why);Need(bool(loaded),why.c_str());
  next.nativePath=final.wstring();next.model=loaded;next.notice=original?"Original authored pupil anchors retained.":(cap?"Reviewed forehead-band starting fit; adjust to your head.":"Uniform starting fit; adjust position and rotation to your face.");if(d->animations_count)next.notice+=" Frozen authored rest pose; "+std::to_string(d->animations_count)+" animation(s) are not played.";if(bakedSkin)next.notice+=" Rest-pose joint skin baked to static geometry.";if(normalizedWeights)next.notice+=" Skin influence weights normalized.";if(bakedMorph)next.notice+=" Authored default morph weights baked to static geometry.";if(!fallback.Empty())next.notice+=" Baseline metallic-roughness fallback: "+fallback.Text()+"; no clearcoat or refraction.";if(ignoredMaps)next.notice+=" Normal, occlusion and emissive maps are not rendered.";result=std::move(next);return true;
 }catch(const std::exception& e){error=e.what();return false;}catch(...){error="Could not import bounded local GLB";return false;}
}
bool IsOwnedLocalFaceFilterPath(const std::wstring& path,const std::wstring& library,std::string& error){error.clear();try{fs::path p(path),root(library);PlainParents(root,false);Need(p.is_absolute()&&EqualPath(p.parent_path(),root)&&p.extension()==L".pfmodel","Saved model must be inside PrismForge's local library");auto name=p.stem().wstring();Need(name.size()==64&&std::all_of(name.begin(),name.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"Saved local model has an invalid content name");auto b=Read(p,size_t(HardModelFileBytes));Need(Packs::Sha256::ComputeHex(b.data(),b.size())==std::string(name.begin(),name.end()),"Saved local model content does not match its name");return true;}catch(const std::exception& e){error=e.what();return false;}catch(...){error="Invalid owned local model path";return false;}}
bool RemoveOwnedLocalFaceFilter(const std::wstring& path,const std::wstring& library,std::string& error){if(!IsOwnedLocalFaceFilterPath(path,library,error))return false;if(!DeleteFileW(path.c_str())){error="Cannot remove converted local model copy";return false;}return true;}
}
