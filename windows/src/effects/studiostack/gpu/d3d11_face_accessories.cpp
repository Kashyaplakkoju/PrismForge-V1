#include "d3d11_face_accessories.h"
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>
#include <limits>
using Microsoft::WRL::ComPtr;
namespace PrismForge::Effects::StudioStack::Gpu {
namespace {
const char* shader=R"(
struct VIn {float4 position:POSITION;float4 color:COLOR;float3 uv:TEXCOORD;float3 normal:NORMAL;float2 material:TEXCOORD1;};
struct VOut {float4 position:SV_POSITION;float4 color:COLOR;float3 uv:TEXCOORD;float3 normal:NORMAL;float2 material:TEXCOORD1;};
VOut VS(VIn i){VOut o;o.position=float4(i.position.xy*i.position.w,i.position.z*i.position.w,i.position.w);o.color=i.color;o.uv=i.uv;o.normal=i.normal;o.material=i.material;return o;}
Texture2D<float4> filterBaseTexture:register(t3);Texture2D<float4> filterMetalRoughTexture:register(t4);
Texture2D<float4> capTexture:register(t2);SamplerState capSampler:register(s0);
float EncodeSrgb(float value){value=saturate(value);return value<=.0031308?12.92*value:1.055*pow(value,1.0/2.4)-.055;}
// The cap SRGB view decodes RGB before bilinear filtering; alpha stays linear.
// Encode only the shaded cap before the existing encoded camera composition.
float3 FilterShade(float3 n,float3 base,float metallic,float roughness){
 n=normalize(n);float3 key=normalize(float3(-.4,.65,1)),fill=normalize(float3(.7,.2,1)),halfway=normalize(key+float3(0,0,1));
 metallic=saturate(metallic);roughness=clamp(roughness,.045,1);float diffuse=.30+.55*max(0,dot(n,key))+.18*max(0,dot(n,fill));
 float shine=pow(max(0,dot(n,halfway)),clamp(2/(roughness*roughness)-2,2,256));float3 f0=.04*(1-metallic)+base*metallic;
 float3 color=base*diffuse*(1-.65*metallic)+f0*shine*(.30+.45*metallic);return float3(EncodeSrgb(color.r),EncodeSrgb(color.g),EncodeSrgb(color.b));}
float4 PS(VOut i):SV_TARGET{float4 c=i.color;
 if(i.uv.z>.5&&i.uv.z<1.5){c*=capTexture.Sample(capSampler,i.uv.xy);c.rgb=float3(EncodeSrgb(c.r),EncodeSrgb(c.g),EncodeSrgb(c.b));}
 if(i.uv.z>=1.5){float metal=i.material.x,rough=i.material.y;if(i.uv.z<2.5){c*=filterBaseTexture.Sample(capSampler,i.uv.xy);float4 mr=filterMetalRoughTexture.Sample(capSampler,i.uv.xy);rough*=mr.g;metal*=mr.b;}c.rgb=FilterShade(i.normal,c.rgb,metal,rough);}
 float a=saturate(c.a);clip(a-1.0/255.0);return float4(saturate(c.rgb)*a,a);}

Texture2D<float4> source:register(t0);Texture2D<float4> overlay:register(t1);SamplerState linearClamp:register(s0);
RWTexture2D<float4> target:register(u0);
cbuffer Size:register(b0){uint W,H,L,T,RW,RH,unused0,unused1;};
[numthreads(16,16,1)] void CS(uint3 p:SV_DispatchThreadID){if(p.x>=W||p.y>=H)return;float4 c=source.Load(int3(p.xy,0));
 if(p.x>=L&&p.y>=T&&p.x<L+RW&&p.y<T+RH){float2 uv=(float2(p.xy)-float2(L,T)+.5)/float2(RW,RH);float4 o=overlay.SampleLevel(linearClamp,uv,0);c.rgb=o.rgb+c.rgb*(1-o.a);}target[p.xy]=c;}
)";
HRESULT Compile(const char* entry,const char* profile,ID3DBlob** out,std::string& error){ComPtr<ID3DBlob> message;HRESULT hr=D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,out,&message);if(FAILED(hr))error=message?std::string(static_cast<char*>(message->GetBufferPointer()),std::min<size_t>(message->GetBufferSize(),240)):"3D accessory shader compilation failed";return hr;}
struct Vertex {float x,y,z,distance,r,g,b,a,u,v,textured,nx,ny,nz,metallic,roughness;};
struct Constants {UINT w,h,left,top,width,height,pad0,pad1;};
constexpr uint64_t MaxSceneVertices=uint64_t(Overlays::Realistic::HardModelTriangles)*3+8192;
static_assert(MaxSceneVertices*sizeof(Vertex)<=std::numeric_limits<UINT>::max(),"Bounded vertex buffer byte width");
}
struct D3D11FaceAccessories::Impl {
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11ComputeShader> cs;ComPtr<ID3D11InputLayout> layout;
 ComPtr<ID3D11Buffer> vertices,constants;ComPtr<ID3D11RasterizerState> raster;
 ComPtr<ID3D11BlendState> color,depthOnly;ComPtr<ID3D11DepthStencilState> writeDepth,readDepth;
 ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11Texture2D> rgba,depth;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11DepthStencilView> dsv;
 ComPtr<ID3D11Texture2D> capImage;ComPtr<ID3D11ShaderResourceView> capView;
 std::shared_ptr<const Overlays::Realistic::TextureRGBA> capSnapshot,filterBaseSnapshot,filterMrSnapshot;
 ComPtr<ID3D11Texture2D> filterBaseImage,filterMrImage;ComPtr<ID3D11ShaderResourceView> filterBaseView,filterMrView;
 Overlays::Realistic::FilterBuildFailure filterFailure=Overlays::Realistic::FilterBuildFailure::None;
 int rw=0,rh=0;uint32_t vertexCapacity=0;bool ready=false;
 HRESULT EnsureVertices(size_t count){
  if(!count||uint64_t(count)>MaxSceneVertices||uint64_t(count)>std::numeric_limits<UINT>::max()/sizeof(Vertex))return E_INVALIDARG;
  if(vertices&&count<=vertexCapacity)return S_OK;
  uint64_t capacity=std::max<uint64_t>(4096,vertexCapacity);
  while(capacity<count)capacity=std::min<uint64_t>(MaxSceneVertices,capacity*2);
  if(capacity<count||capacity>std::numeric_limits<UINT>::max()/sizeof(Vertex))return E_INVALIDARG;
  D3D11_BUFFER_DESC vb{};vb.ByteWidth=UINT(capacity*sizeof(Vertex));vb.Usage=D3D11_USAGE_DYNAMIC;vb.BindFlags=D3D11_BIND_VERTEX_BUFFER;vb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Buffer> next;const HRESULT hr=device->CreateBuffer(&vb,nullptr,&next);if(FAILED(hr))return hr;
  vertices=std::move(next);vertexCapacity=uint32_t(capacity);return S_OK;
 }
 HRESULT EnsureCapTexture(const std::shared_ptr<const Overlays::Realistic::TextureRGBA>& snapshot){
  if(!snapshot)return S_OK;if(snapshot==capSnapshot&&capImage&&capView)return S_OK;
  if(!snapshot->width||!snapshot->height||snapshot->width>2048||snapshot->height>2048||snapshot->pixels.size()!=size_t(snapshot->width)*snapshot->height*4)return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC td{};td.Width=snapshot->width;td.Height=snapshot->height;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA pixels{};pixels.pSysMem=snapshot->pixels.data();pixels.SysMemPitch=snapshot->width*4;
  ComPtr<ID3D11Texture2D> next;ComPtr<ID3D11ShaderResourceView> nextView;
  HRESULT hr=device->CreateTexture2D(&td,&pixels,&next);if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(next.Get(),nullptr,&nextView);if(FAILED(hr))return hr;
  capImage=std::move(next);capView=std::move(nextView);capSnapshot=snapshot;return S_OK;
 }
 HRESULT EnsureFilterTexture(const std::shared_ptr<const Overlays::Realistic::TextureRGBA>& snapshot,bool srgb,ComPtr<ID3D11Texture2D>& image,ComPtr<ID3D11ShaderResourceView>& view,std::shared_ptr<const Overlays::Realistic::TextureRGBA>& previous){
  if(!snapshot)return S_OK;if(snapshot==previous&&image&&view)return S_OK;
  if(!snapshot->width||!snapshot->height||snapshot->width>2048||snapshot->height>2048||snapshot->pixels.size()!=size_t(snapshot->width)*snapshot->height*4)return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC td{};td.Width=snapshot->width;td.Height=snapshot->height;td.MipLevels=td.ArraySize=1;td.Format=srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA pixels{};pixels.pSysMem=snapshot->pixels.data();pixels.SysMemPitch=snapshot->width*4;ComPtr<ID3D11Texture2D> next;ComPtr<ID3D11ShaderResourceView> nextView;
  HRESULT hr=device->CreateTexture2D(&td,&pixels,&next);if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(next.Get(),nullptr,&nextView);if(FAILED(hr))return hr;image=std::move(next);view=std::move(nextView);previous=snapshot;return S_OK;
 }
 HRESULT Init(std::string& error){
  if(ready)return S_OK;if(!device||!context){error="3D accessory GPU is unavailable";return E_INVALIDARG;}
  ComPtr<ID3D11Device> owner;context->GetDevice(&owner);if(owner.Get()!=device.Get()){error="3D accessory context belongs to another device";return E_INVALIDARG;}
  ComPtr<ID3DBlob> v,p,c;HRESULT hr=Compile("VS","vs_5_0",&v,error);if(FAILED(hr))return hr;hr=Compile("PS","ps_5_0",&p,error);if(FAILED(hr))return hr;hr=Compile("CS","cs_5_0",&c,error);if(FAILED(hr))return hr;
#define CREATE(expr) hr=(expr);if(FAILED(hr)){error="3D accessory resource creation failed";return hr;}
  CREATE(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));CREATE(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));CREATE(device->CreateComputeShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,&cs));
  const D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32B32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,44,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,56,D3D11_INPUT_PER_VERTEX_DATA,0}};
  CREATE(device->CreateInputLayout(elements,5,v->GetBufferPointer(),v->GetBufferSize(),&layout));
  D3D11_BUFFER_DESC vb{};vb.ByteWidth=sizeof(Constants);vb.Usage=D3D11_USAGE_DYNAMIC;vb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;vb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;CREATE(device->CreateBuffer(&vb,nullptr,&constants));
  D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;CREATE(device->CreateRasterizerState(&rs,&raster));
  D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;CREATE(device->CreateDepthStencilState(&ds,&writeDepth));ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;CREATE(device->CreateDepthStencilState(&ds,&readDepth));
  D3D11_BLEND_DESC blend{};auto& b=blend.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_ONE;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;CREATE(device->CreateBlendState(&blend,&color));b.RenderTargetWriteMask=0;CREATE(device->CreateBlendState(&blend,&depthOnly));
  D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;CREATE(device->CreateSamplerState(&sd,&sampler));ready=true;return S_OK;
#undef CREATE
 }
 HRESULT Ensure(int w,int h){
  if(w==rw&&h==rh&&rgba&&depth)return S_OK;
  D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> newRgba,newDepth;ComPtr<ID3D11RenderTargetView> newRtv;ComPtr<ID3D11ShaderResourceView> newSrv;ComPtr<ID3D11DepthStencilView> newDsv;
  HRESULT hr=device->CreateTexture2D(&td,nullptr,&newRgba);if(SUCCEEDED(hr))hr=device->CreateRenderTargetView(newRgba.Get(),nullptr,&newRtv);if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(newRgba.Get(),nullptr,&newSrv);if(FAILED(hr))return hr;
  td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;hr=device->CreateTexture2D(&td,nullptr,&newDepth);if(SUCCEEDED(hr))hr=device->CreateDepthStencilView(newDepth.Get(),nullptr,&newDsv);if(FAILED(hr))return hr;
  rgba=std::move(newRgba);rtv=std::move(newRtv);srv=std::move(newSrv);depth=std::move(newDepth);dsv=std::move(newDsv);rw=w;rh=h;return S_OK;
 }
 void Unbind(){if(!context)return;ID3D11ShaderResourceView* views[2]{};ID3D11UnorderedAccessView* uav=nullptr;context->CSSetShaderResources(0,2,views);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->CSSetShader(nullptr,nullptr,0);context->OMSetRenderTargets(0,nullptr,nullptr);context->VSSetShader(nullptr,nullptr,0);context->PSSetShader(nullptr,nullptr,0);ID3D11ShaderResourceView* emptyTextures[3]{};context->PSSetShaderResources(2,3,emptyTextures);ID3D11SamplerState* emptySampler=nullptr;context->PSSetSamplers(0,1,&emptySampler);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);ID3D11Buffer* vb=nullptr;UINT stride=0,offset=0;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);}
};
D3D11FaceAccessories::D3D11FaceAccessories(ID3D11Device* d,ID3D11DeviceContext* c):impl(std::make_unique<Impl>()){impl->device=d;impl->context=c;}
D3D11FaceAccessories::~D3D11FaceAccessories()=default;
Overlays::Realistic::FilterBuildFailure D3D11FaceAccessories::GetLastFilterBuildFailure() const {return impl->filterFailure;}
uint32_t D3D11FaceAccessories::GetVertexCapacityForTesting() const {return impl->vertexCapacity;}
GpuOverlayOutcome D3D11FaceAccessories::Process(ID3D11ShaderResourceView* input,ID3D11UnorderedAccessView* output,int width,int height,const AnalysisBridge::FaceLandmarkBundle* lm,bool glasses,bool cap,float size,Overlays::LayerReport* layers,std::string* outError,int capStyle,int glassesStyle,float capScale,float capVerticalOffset,int faceFilterStyle,float faceFilterScale,float faceFilterVerticalOffset,const std::shared_ptr<const Overlays::Realistic::NativeFaceFilter>& customModel,const Overlays::Realistic::CustomFaceFilterFit& customFit,uint32_t renderEdge){
 impl->filterFailure=Overlays::Realistic::FilterBuildFailure::None;
 if(layers)*layers={glasses?2:1,cap?2:1,1,faceFilterStyle?2:1};if(outError)outError->clear();if(!glasses&&!cap&&!faceFilterStyle)return GpuOverlayOutcome::Bypass;
 std::string error;auto fail=[&](HRESULT hr,const std::string& e){if(outError)*outError=e;if(layers)*layers={glasses?3:1,cap?3:1,1,faceFilterStyle?3:1};return hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET?GpuOverlayOutcome::DeviceLost:GpuOverlayOutcome::AllocationFailed;};
 if(!input||!output||width<=0||height<=0||width>8192||height>8192||!impl->device||!impl->context)return fail(E_INVALIDARG,"Invalid3D accessory target");
 Overlays::Realistic::ProjectedScene scene;if(!Overlays::Realistic::BuildProjectedScene(lm,width,height,glasses,cap,size,scene,error,capStyle,glassesStyle,capScale,capVerticalOffset,faceFilterStyle,faceFilterScale,faceFilterVerticalOffset,customModel,customFit,renderEdge)){impl->filterFailure=scene.filterFailure;if(outError)*outError=error;if(scene.filterFailure!=Overlays::Realistic::FilterBuildFailure::None){if(layers)layers->faceFilter=3;return scene.filterFailure==Overlays::Realistic::FilterBuildFailure::InvalidRequest?GpuOverlayOutcome::InvalidInput:GpuOverlayOutcome::AllocationFailed;}return GpuOverlayOutcome::NotReady;}
 ComPtr<ID3D11Resource> inResource,outResource;input->GetResource(&inResource);output->GetResource(&outResource);if(inResource.Get()==outResource.Get())return fail(E_INVALIDARG,"3D input and output must be distinct");
 ComPtr<ID3D11Texture2D> inTex,outTex;ComPtr<ID3D11Device> inDevice,outDevice;inResource->GetDevice(&inDevice);outResource->GetDevice(&outDevice);if(inDevice.Get()!=impl->device.Get()||outDevice.Get()!=impl->device.Get()||FAILED(inResource.As(&inTex))||FAILED(outResource.As(&outTex)))return fail(E_INVALIDARG,"3D resource device/type mismatch");
 D3D11_TEXTURE2D_DESC it{},ot{};inTex->GetDesc(&it);outTex->GetDesc(&ot);D3D11_SHADER_RESOURCE_VIEW_DESC iv{};D3D11_UNORDERED_ACCESS_VIEW_DESC ov{};input->GetDesc(&iv);output->GetDesc(&ov);
 if(it.Width!=UINT(width)||it.Height!=UINT(height)||ot.Width!=UINT(width)||ot.Height!=UINT(height)||it.SampleDesc.Count!=1||ot.SampleDesc.Count!=1||it.ArraySize!=1||ot.ArraySize!=1||iv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||ov.ViewDimension!=D3D11_UAV_DIMENSION_TEXTURE2D||iv.Texture2D.MostDetailedMip!=0||ov.Texture2D.MipSlice!=0||iv.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||ov.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)return fail(E_INVALIDARG,"Unsupported3D texture/view descriptor");
 uint64_t count=0;for(const auto* group:{&scene.faces,&scene.opaque,&scene.lenses}){if(group->size()%3||uint64_t(group->size())>MaxSceneVertices-count)return fail(E_INVALIDARG,"3D vertex bound exceeded");count+=group->size();}
 HRESULT hr=impl->Init(error);if(FAILED(hr))return fail(hr,error);hr=impl->EnsureVertices(size_t(count));if(FAILED(hr))return fail(hr,"3D vertex buffer allocation failed");hr=impl->Ensure(scene.rasterWidth,scene.rasterHeight);if(FAILED(hr))return fail(hr,"3D ROI allocation failed");hr=impl->EnsureCapTexture(scene.capTexture);if(FAILED(hr))return fail(hr,"3D cap texture allocation failed");hr=impl->EnsureFilterTexture(scene.filterBaseTexture,true,impl->filterBaseImage,impl->filterBaseView,impl->filterBaseSnapshot);if(FAILED(hr))return fail(hr,"3D filter base texture allocation failed");hr=impl->EnsureFilterTexture(scene.filterMetalRoughTexture,false,impl->filterMrImage,impl->filterMrView,impl->filterMrSnapshot);if(FAILED(hr))return fail(hr,"3D filter material texture allocation failed");
 struct Guard{Impl* p;~Guard(){p->Unbind();}}guard{impl.get()};impl->Unbind();
 D3D11_MAPPED_SUBRESOURCE mapped{};hr=impl->context->Map(impl->vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);if(FAILED(hr))return fail(hr,"3D vertex upload failed");if(!mapped.pData){impl->context->Unmap(impl->vertices.Get(),0);return fail(E_FAIL,"3D vertex map is empty");}
 auto* vertex=static_cast<Vertex*>(mapped.pData);for(const auto* group:{&scene.faces,&scene.opaque,&scene.lenses})for(const auto& v:*group)*vertex++={2*(v.x-scene.left)/scene.width-1,1-2*(v.y-scene.top)/scene.height,v.depth,v.distance,v.r,v.g,v.b,v.a,v.u,v.v,v.textured,v.nx,v.ny,v.nz,v.metallic,v.roughness};impl->context->Unmap(impl->vertices.Get(),0);
 const float clear[4]{};impl->context->ClearRenderTargetView(impl->rtv.Get(),clear);impl->context->ClearDepthStencilView(impl->dsv.Get(),D3D11_CLEAR_DEPTH,1,0);ID3D11RenderTargetView* rtv=impl->rtv.Get();impl->context->OMSetRenderTargets(1,&rtv,impl->dsv.Get());D3D11_VIEWPORT viewport{0,0,float(scene.rasterWidth),float(scene.rasterHeight),0,1};impl->context->RSSetViewports(1,&viewport);impl->context->RSSetState(impl->raster.Get());impl->context->IASetInputLayout(impl->layout.Get());impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=sizeof(Vertex),offset=0;ID3D11Buffer* vb=impl->vertices.Get();impl->context->IASetVertexBuffers(0,1,&vb,&stride,&offset);impl->context->VSSetShader(impl->vs.Get(),nullptr,0);impl->context->PSSetShader(impl->ps.Get(),nullptr,0);ID3D11ShaderResourceView* textureViews[]={scene.capTexture?impl->capView.Get():nullptr,scene.filterBaseTexture?impl->filterBaseView.Get():nullptr,scene.filterMetalRoughTexture?impl->filterMrView.Get():nullptr};ID3D11SamplerState* capSampler=impl->sampler.Get();impl->context->PSSetShaderResources(2,3,textureViews);impl->context->PSSetSamplers(0,1,&capSampler);impl->context->OMSetDepthStencilState(impl->writeDepth.Get(),0);impl->context->OMSetBlendState(impl->depthOnly.Get(),nullptr,~0u);impl->context->Draw(UINT(scene.faces.size()),0);impl->context->OMSetBlendState(impl->color.Get(),nullptr,~0u);impl->context->Draw(UINT(scene.opaque.size()),UINT(scene.faces.size()));impl->context->OMSetDepthStencilState(impl->readDepth.Get(),0);impl->context->Draw(UINT(scene.lenses.size()),UINT(scene.faces.size()+scene.opaque.size()));impl->context->OMSetRenderTargets(0,nullptr,nullptr);
 Constants constants{UINT(width),UINT(height),UINT(scene.left),UINT(scene.top),UINT(scene.width),UINT(scene.height),0,0};hr=impl->context->Map(impl->constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);if(FAILED(hr))return fail(hr,"3D composition constants map failed");if(!mapped.pData){impl->context->Unmap(impl->constants.Get(),0);return fail(E_FAIL,"3D composition map is empty");}std::memcpy(mapped.pData,&constants,sizeof(constants));impl->context->Unmap(impl->constants.Get(),0);ID3D11ShaderResourceView* views[]={input,impl->srv.Get()};ID3D11Buffer* cb=impl->constants.Get();ID3D11SamplerState* sampler=impl->sampler.Get();impl->context->CSSetShader(impl->cs.Get(),nullptr,0);impl->context->CSSetShaderResources(0,2,views);impl->context->CSSetUnorderedAccessViews(0,1,&output,nullptr);impl->context->CSSetConstantBuffers(0,1,&cb);impl->context->CSSetSamplers(0,1,&sampler);impl->context->Dispatch((width+15)/16,(height+15)/16,1);
 hr=impl->device->GetDeviceRemovedReason();if(FAILED(hr))return fail(hr,"3D accessory device removed");if(layers)*layers={glasses?0:1,cap?0:1,1,faceFilterStyle?0:1};return GpuOverlayOutcome::Success;
}
}
