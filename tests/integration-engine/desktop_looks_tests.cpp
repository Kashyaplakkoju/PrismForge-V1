#include "windows/src/effects/studiostack/studio_stack.h"
#include "windows/src/effects/studiostack/gpu/d3d11_desktop_looks.h"
#include "windows/src/effects/studiostack/gpu/d3d11_studiostack_lut.h"
#include <dxgi.h>
#include <iostream>
#include <sstream>
#include <limits>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/dict.h>
}
using namespace PrismForge::Effects;
namespace S=PrismForge::Effects::StudioStack;
namespace C=PrismForge::Effects::StudioStack::Gpu;
namespace P=PrismForge::Effects::Presets;
using Microsoft::WRL::ComPtr;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));} while(0)
struct Device {ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;};
static Device MakeDevice(bool warp=false) {Device d;D3D_FEATURE_LEVEL f;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d.dev,&f,&d.ctx)));CHECK(f>=D3D_FEATURE_LEVEL_11_0);return d;}
struct Texture {ComPtr<ID3D11Texture2D> tex;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;};
static Texture MakeTexture(Device& d,int w,int h,const std::vector<uint8_t>& bytes={},int stride=0) {
 Texture t;D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
 D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=bytes.data();initial.SysMemPitch=stride?stride:w*4;
 CHECK(SUCCEEDED(d.dev->CreateTexture2D(&desc,bytes.empty()?nullptr:&initial,&t.tex)));
 CHECK(SUCCEEDED(d.dev->CreateShaderResourceView(t.tex.Get(),nullptr,&t.srv)));CHECK(SUCCEEDED(d.dev->CreateUnorderedAccessView(t.tex.Get(),nullptr,&t.uav)));return t;
}
static std::vector<uint8_t> Read(Device& d,ID3D11Texture2D* tex) {D3D11_TEXTURE2D_DESC desc{};tex->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;CHECK(SUCCEEDED(d.dev->CreateTexture2D(&desc,nullptr,&staging)));d.ctx->CopyResource(staging.Get(),tex);D3D11_MAPPED_SUBRESOURCE m{};CHECK(SUCCEEDED(d.ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)));std::vector<uint8_t> v(static_cast<size_t>(desc.Width)*desc.Height*4);for(UINT y=0;y<desc.Height;++y)std::copy_n(static_cast<const uint8_t*>(m.pData)+y*m.RowPitch,desc.Width*4,v.data()+static_cast<size_t>(y)*desc.Width*4);d.ctx->Unmap(staging.Get(),0);return v;}
static std::vector<uint8_t> Pattern(int w,int h,int stride=0,int seed=0) {if(!stride)stride=w*4;std::vector<uint8_t> v(static_cast<size_t>(stride)*h,0xD7);for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* p=v.data()+static_cast<size_t>(y)*stride+x*4;p[0]=(x*37+y*21+seed)&255;p[1]=(x*17+y*81+seed*3)&255;p[2]=(x*13+y*41+seed*5)&255;p[3]=(x*7+y*11+17)&255;}return v;}
static uint8_t Blend(uint8_t v,int graded,float s){return static_cast<uint8_t>(std::floor((1.0f-s)*v+s*graded+0.5f));}
static void Oracle(uint8_t* p,int look,float strength) {int r=p[0],g=p[1],b=p[2];if(look==1){r=std::min(255,r+14);g=std::min(255,g+4);b=std::max(0,b-12);}if(look==2){r=std::max(0,r-12);g=std::min(255,g+2);b=std::min(255,b+14);}if(look==3){r=g=b=(77*r+150*g+29*b+128)/256;}p[0]=Blend(p[0],r,strength);p[1]=Blend(p[1],g,strength);p[2]=Blend(p[2],b,strength);}
static std::string Cube(int mode=0,bool domains=false){std::ostringstream ss;ss<<"LUT_3D_SIZE 2\n";if(domains)ss<<"DOMAIN_MIN 0.1 0.2 -0.1\nDOMAIN_MAX 0.8 0.9 0.7\n";for(int b=0;b<2;++b)for(int g=0;g<2;++g)for(int r=0;r<2;++r){if(mode==0)ss<<r<<' '<<g<<' '<<b;else if(mode==1)ss<<1-r<<' '<<1-g<<' '<<1-b;else ss<<b<<' '<<r<<' '<<g;ss<<'\n';}return ss.str();}
static std::shared_ptr<const CubeLut> Lut(int mode=0,bool domains=false){auto p=CubeLut::Parse(Cube(mode,domains));CHECK(p);return std::shared_ptr<const CubeLut>(std::move(p));}
static void LutOracle(uint8_t* p,const CubeLut& lut,float s){auto q=lut.SampleBytes(p[0],p[1],p[2]);float vals[]={q.r,q.g,q.b};for(int c=0;c<3;++c)p[c]=static_cast<uint8_t>(std::clamp((1-s)*p[c]+s*std::clamp(vals[c],0.0f,1.0f)*255+0.5f,0.0f,255.0f));}
static AVFrame* Frame(int w=19,int h=11,int seed=0){auto* f=av_frame_alloc();CHECK(f);f->width=w;f->height=h;f->format=AV_PIX_FMT_BGR24;f->pts=7654321;f->time_base={1,1000000};f->color_trc=AVCOL_TRC_BT709;CHECK(av_frame_get_buffer(f,64)==0);auto pixels=Pattern(w,h,0,seed);for(int y=0;y<h;++y){std::fill_n(f->data[0]+y*f->linesize[0],f->linesize[0],0xD7);for(int x=0;x<w;++x){const auto* p=pixels.data()+(y*w+x)*4;auto* o=f->data[0]+y*f->linesize[0]+x*3;o[0]=p[2];o[1]=p[1];o[2]=p[0];}}CHECK(av_dict_set(&f->metadata,"owned","current",0)==0);return f;}
static std::vector<uint8_t> Bytes(const AVFrame* f){std::vector<uint8_t> v(static_cast<size_t>(f->width)*f->height*3);for(int y=0;y<f->height;++y)std::copy_n(f->data[0]+y*f->linesize[0],f->width*3,v.data()+static_cast<size_t>(y)*f->width*3);return v;}
static void Compare(const AVFrame* f,const std::vector<uint8_t>& expected,int maxError=0){auto got=Bytes(f);CHECK(got.size()==expected.size());for(size_t i=0;i<got.size();++i)CHECK(std::abs(int(got[i])-int(expected[i]))<=maxError);}
static std::vector<uint8_t> FrameOracle(const AVFrame* f,int look,float strength,const CubeLut* input,float is,const CubeLut* post=nullptr,float ps=0){auto v=Bytes(f);for(size_t i=0;i<v.size();i+=3){uint8_t p[]={v[i+2],v[i+1],v[i],255};Oracle(p,look,strength);if(input)LutOracle(p,*input,is);if(post)LutOracle(p,*post,ps);v[i]=p[2];v[i+1]=p[1];v[i+2]=p[0];}return v;}
static void LookPixels(bool warp){auto d=MakeDevice(warp);const int w=37,h=23,stride=w*4+28;auto src=Pattern(w,h,stride);auto input=MakeTexture(d,w,h,src,stride),output=MakeTexture(d,w,h);C::D3D11DesktopLooks pass(d.dev.Get(),d.ctx.Get());for(int look=1;look<=3;++look)for(float s:{0.5f,1.0f,0.0005f}){CHECK(pass.Process(input.srv.Get(),output.uav.Get(),w,h,static_cast<C::DesktopLook>(look),s)==C::GpuDesktopLooksOutcome::Success);auto got=Read(d,output.tex.Get());for(int y=0;y<h;++y)for(int x=0;x<w;++x){uint8_t expected[4];std::copy_n(src.data()+y*stride+x*4,4,expected);Oracle(expected,look,s);for(int c=0;c<4;++c)CHECK(got[(y*w+x)*4+c]==expected[c]);}}
 CHECK(pass.Process(input.srv.Get(),output.uav.Get(),w,h,C::DesktopLook::Warm,0)==C::GpuDesktopLooksOutcome::Bypass);CHECK(pass.Process(input.srv.Get(),output.uav.Get(),w,h,C::DesktopLook::Original,1)==C::GpuDesktopLooksOutcome::Bypass);
}
static void CpuRounding(){for(int i=0;i<256;++i)for(int look=0;look<4;++look)for(float s:{0.0f,0.5f,1.0f}){uint8_t p[]={uint8_t(i),uint8_t(255-i),uint8_t((i*3)&255),17};auto r=p[0],g=p[1],b=p[2];Oracle(p,look,s);C::DesktopLookApplyPixelCpu(r,g,b,static_cast<C::DesktopLook>(look),s);CHECK(r==p[0]&&g==p[1]&&b==p[2]);}}
static void Validation(){auto d=MakeDevice(),d2=MakeDevice(true);auto a=MakeTexture(d,3,5),b=MakeTexture(d,3,5),other=MakeTexture(d2,3,5);C::D3D11DesktopLooks pass(d.dev.Get(),d.ctx.Get());for(float s:{-1.0f,2.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,5,C::DesktopLook::Warm,s)==C::GpuDesktopLooksOutcome::InvalidInput);
 CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,5,static_cast<C::DesktopLook>(-1),1)==C::GpuDesktopLooksOutcome::InvalidInput);CHECK(pass.Process(a.srv.Get(),a.uav.Get(),3,5,C::DesktopLook::Warm,1)==C::GpuDesktopLooksOutcome::InvalidInput);CHECK(pass.Process(a.srv.Get(),other.uav.Get(),3,5,C::DesktopLook::Warm,1)==C::GpuDesktopLooksOutcome::InvalidInput);CHECK(pass.Process(a.srv.Get(),b.uav.Get(),4,5,C::DesktopLook::Warm,1)==C::GpuDesktopLooksOutcome::InvalidInput);
 C::D3D11DesktopLooks badContext(d.dev.Get(),d2.ctx.Get());CHECK(badContext.Process(a.srv.Get(),b.uav.Get(),3,5,C::DesktopLook::Warm,1)==C::GpuDesktopLooksOutcome::InvalidInput);
 C::D3D11StudioStackLut lutPass(d.dev.Get(),d.ctx.Get());CHECK(lutPass.Process(a.srv.Get(),a.uav.Get(),3,5,Lut(),1)==C::GpuLutOutcome::InvalidInput);CHECK(lutPass.Process(a.srv.Get(),b.uav.Get(),3,5,Lut(),std::numeric_limits<float>::quiet_NaN())==C::GpuLutOutcome::InvalidInput);
}
static void LutPixels(bool warp){auto d=MakeDevice(warp);auto bytes=Pattern(17,9);auto a=MakeTexture(d,17,9,bytes),b=MakeTexture(d,17,9);C::D3D11StudioStackLut pass(d.dev.Get(),d.ctx.Get());for(int mode=0;mode<3;++mode){auto lut=Lut(mode,true);for(float s:{0.5f,1.0f,0.0005f}){CHECK(pass.Process(a.srv.Get(),b.uav.Get(),17,9,lut,s)==C::GpuLutOutcome::Success);auto got=Read(d,b.tex.Get());for(size_t i=0;i<bytes.size();i+=4){uint8_t p[4];std::copy_n(bytes.data()+i,4,p);LutOracle(p,*lut,s);for(int c=0;c<4;++c)CHECK(std::abs(int(p[c])-int(got[i+c]))<=1);}}}CHECK(pass.Process(a.srv.Get(),b.uav.Get(),17,9,Lut(),0)==C::GpuLutOutcome::Bypass);}
static void CacheAndContent(){auto d=MakeDevice();auto a=MakeTexture(d,3,1,Pattern(3,1)),b=MakeTexture(d,3,1);C::D3D11StudioStackLut pass(d.dev.Get(),d.ctx.Get());auto first=Lut(0);std::weak_ptr<const CubeLut> weak=first;CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,1,first,1)==C::GpuLutOutcome::Success);auto v1=Read(d,b.tex.Get());first.reset();CHECK(!weak.expired());CHECK(pass.GetUploadCount()==1);auto equal=Lut(0);CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,1,equal,1)==C::GpuLutOutcome::Success);CHECK(pass.GetUploadCount()==1);CHECK(weak.expired());CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,1,Lut(1),1)==C::GpuLutOutcome::Success);CHECK(pass.GetUploadCount()==2);CHECK(Read(d,b.tex.Get())!=v1);CHECK(pass.GetCachedTextureCount()==1);
 auto raw=CubeLut::Parse(Cube(0));CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,1,raw.get(),1)==C::GpuLutOutcome::Success);auto newer=CubeLut::Parse(Cube(2));*raw=std::move(*newer);CHECK(pass.Process(a.srv.Get(),b.uav.Get(),3,1,raw.get(),1)==C::GpuLutOutcome::Success);CHECK(pass.GetCachedTextureCount()==1);CHECK(Read(d,b.tex.Get())!=v1);
}
static void PresetRoundTrip(){auto p=P::StudioPreset::Neutral();p.bypassAll=false;p.inputLook=2;p.inputLookStrength=.37123457f;p.inputLutIntensity=.91234565f;p.selectedInputLutPath="D:\\looks\\input\" test.cube";p.selectedLutPath="post.cube";p.lutIntensity=.5f;auto text=P::PresetStore::Serialize(p);P::StudioPreset result;std::string error;CHECK(P::PresetStore::Deserialize(text,result,error));CHECK(result.inputLook==p.inputLook && result.inputLookStrength==p.inputLookStrength && result.inputLutIntensity==p.inputLutIntensity && result.selectedInputLutPath==p.selectedInputLutPath);CHECK(result.selectedLutPath=="post.cube");
 for(const char* bad:{"NaN","1e999","-1","2","null","\"0.5\""}){auto malformed="{\"presetId\":\"bad\",\"inputColour\":{\"strength\":"+std::string(bad)+"}}";auto previous=result;CHECK(!P::PresetStore::Deserialize(malformed,result,error));CHECK(result.presetId==previous.presetId&&result.inputLookStrength==previous.inputLookStrength);}
 for(const char* bad:{"1.5","-1","4","1e30"})CHECK(!P::PresetStore::Deserialize("{\"presetId\":\"bad\",\"inputColour\":{\"look\":"+std::string(bad)+"}}",result,error));
 CHECK(P::PresetStore::Deserialize("{\"presetId\":\"legacy\",\"lut\":{\"path\":\"old.cube\",\"intensity\":0.5}}",result,error));CHECK(result.inputLook==0&&result.inputLutIntensity==0&&result.inputLookStrength==1&&result.lutIntensity==.5f);
}
static void StackCpuAndGpu(bool warp){auto d=MakeDevice(warp);for(bool gpu:{false,true}){S::StudioStack stack;if(gpu)CHECK(stack.InitializeGpu(d.dev.Get(),d.ctx.Get()));auto p=P::StudioPreset::Neutral();p.bypassAll=false;p.inputLook=1;p.inputLookStrength=.5f;p.inputLutIntensity=.5f;p.lutIntensity=.5f;stack.SetPreset(p);std::string error;CHECK(stack.LoadInputLutFromBuffer(Cube(1,true),error));CHECK(stack.LoadLutFromBuffer(Cube(2),error));auto input=Lut(1,true),post=Lut(2);auto* f=Frame();auto original=Bytes(f);auto expected=FrameOracle(f,1,.5f,input.get(),.5f,post.get(),.5f);S::StackExecutionReport report;auto* result=stack.ProcessFrame(f,1,f->pts,&report);CHECK(result);Compare(result,expected,gpu?1:0);Compare(f,original);CHECK(report.inputLookStatus==S::StageStatus::Applied && report.inputLutStatus==S::StageStatus::Applied && report.lutStatus==S::StageStatus::Applied);CHECK(!report.gpuFallbackUsed);CHECK(report.imageUploadCount==(gpu?1:0)&&report.imageReadbackCount==(gpu?1:0));CHECK(result->pts==f->pts&&result->time_base.num==1&&result->time_base.den==1000000);CHECK(av_dict_get(result->metadata,"owned",nullptr,0));
 auto* retained=av_frame_clone(result);CHECK(retained);auto before=Bytes(retained);auto* next=Frame(19,11,17);CHECK(stack.LoadInputLutFromBuffer(Cube(2),error));p.inputLook=2;stack.SetPreset(p);CHECK(stack.ProcessFrame(next,2,next->pts,&report));Compare(retained,before);CHECK(report.inputLookStatus==S::StageStatus::Applied&&report.inputLutStatus==S::StageStatus::Applied);av_frame_free(&retained);av_frame_free(&next);av_frame_free(&f);}}
static void ViewFaults(){auto d=MakeDevice();auto a=MakeTexture(d,7,5,Pattern(7,5)),b=MakeTexture(d,7,5);C::D3D11DesktopLooks look(d.dev.Get(),d.ctx.Get());C::D3D11StudioStackLut lut(d.dev.Get(),d.ctx.Get());for(auto fault:{C::DesktopColourFault::ShaderCompile,C::DesktopColourFault::ShaderCreation,C::DesktopColourFault::BufferCreation,C::DesktopColourFault::Map,C::DesktopColourFault::Dispatch,C::DesktopColourFault::DeviceRemoved}){look.SetFault(fault);CHECK(look.Process(a.srv.Get(),b.uav.Get(),7,5,C::DesktopLook::Warm,1)!=C::GpuDesktopLooksOutcome::Success);look.SetFault(C::DesktopColourFault::None);CHECK(look.Process(a.srv.Get(),b.uav.Get(),7,5,C::DesktopLook::Warm,1)==C::GpuDesktopLooksOutcome::Success);lut.SetFault(fault);CHECK(lut.Process(a.srv.Get(),b.uav.Get(),7,5,Lut(1),1)!=C::GpuLutOutcome::Success);lut.SetFault(C::DesktopColourFault::None);CHECK(lut.Process(a.srv.Get(),b.uav.Get(),7,5,Lut(1),1)==C::GpuLutOutcome::Success);}for(auto fault:{C::DesktopColourFault::LutTexture,C::DesktopColourFault::LutView}){lut.SetFault(fault);CHECK(lut.Process(a.srv.Get(),b.uav.Get(),7,5,Lut(2),1)!=C::GpuLutOutcome::Success);lut.SetFault(C::DesktopColourFault::None);CHECK(lut.Process(a.srv.Get(),b.uav.Get(),7,5,Lut(2),1)==C::GpuLutOutcome::Success);CHECK(lut.GetCachedTextureCount()==1);}}
static void StackFaultsAndRetention(){auto d=MakeDevice();auto src=MakeTexture(d,19,11,Pattern(19,11));S::StudioStack stack;CHECK(stack.InitializeGpu(d.dev.Get(),d.ctx.Get()));auto p=P::StudioPreset::Neutral();p.bypassAll=false;p.inputLook=1;p.inputLutIntensity=.5f;stack.SetPreset(p);std::string error;CHECK(stack.LoadInputLutFromBuffer(Cube(1),error));S::StackExecutionReport report;auto retained=stack.ProcessTextureNative(src.srv.Get(),19,11,1,1000,&report);CHECK(retained.isValid);auto saved=Read(d,retained.texture);auto* f=Frame();auto lut=Lut(1);auto expected=FrameOracle(f,1,1,lut.get(),.5f);
 for(auto seam:{S::GpuFaultSeam::InputLook_ShaderCompile,S::GpuFaultSeam::InputLook_ShaderCreation,S::GpuFaultSeam::InputLook_BufferCreation,S::GpuFaultSeam::InputLook_Map,S::GpuFaultSeam::InputLook_Dispatch,S::GpuFaultSeam::InputLut_ShaderCompile,S::GpuFaultSeam::InputLut_ShaderCreation,S::GpuFaultSeam::InputLut_BufferCreation,S::GpuFaultSeam::InputLut_Texture,S::GpuFaultSeam::InputLut_View,S::GpuFaultSeam::InputLut_Map,S::GpuFaultSeam::InputLut_Dispatch}) {
   // Changed content forces texture/view boundary rather than a legitimate cache hit.
   CHECK(stack.LoadInputLutFromBuffer(Cube(2),error));stack.SetGpuFaultSeam(seam);auto fail=stack.ProcessTextureNative(src.srv.Get(),19,11,2,1000,&report);CHECK(!fail.isValid&&report.imageReadbackCount==0&&!report.errorDescription.empty());CHECK(report.inputLookStatus==S::StageStatus::Failed||report.inputLutStatus==S::StageStatus::Failed);
   CHECK(stack.LoadInputLutFromBuffer(Cube(1),error));auto* out=stack.ProcessFrame(f,3,f->pts,&report);CHECK(out&&report.gpuFallbackUsed);Compare(out,expected);CHECK(report.inputLookStatus==S::StageStatus::Applied&&report.inputLutStatus==S::StageStatus::Applied);stack.SetGpuFaultSeam(S::GpuFaultSeam::None);CHECK(stack.ProcessFrame(f,4,f->pts,&report));CHECK(!report.gpuFallbackUsed);CHECK(Read(d,retained.texture)==saved);
 }
 auto alias=retained;retained.Release();CHECK(stack.GetActiveLeaseCount()==1);stack.ReleaseGpu();CHECK(Read(d,alias.texture)==saved);CHECK(stack.InitializeGpu(d.dev.Get(),d.ctx.Get()));CHECK(Read(d,alias.texture)==saved);alias.Release();CHECK(stack.GetActiveLeaseCount()==0);av_frame_free(&f);
}
static void PresetApplicationTransactional(){S::StudioStack stack;auto p=P::StudioPreset::Neutral();p.bypassAll=false;p.inputLook=1;stack.SetPreset(p);auto bad=p;bad.inputLookStrength=std::numeric_limits<float>::quiet_NaN();stack.SetPreset(bad);CHECK(stack.GetPreset().inputLookStrength==1);std::string error;bad=p;bad.selectedInputLutPath="missing.cube";CHECK(!stack.ApplyPreset(bad,"",error));CHECK(stack.GetPreset().selectedInputLutPath.empty());CHECK(stack.LoadInputLutFromBuffer(Cube(1),error));p.inputLutIntensity=1;stack.SetPreset(p);auto* f=Frame();auto* out=stack.ProcessFrame(f,1);CHECK(out);auto first=Bytes(out);CHECK(!stack.LoadInputLutFromBuffer("invalid",error));Compare(stack.ProcessFrame(f,2),first);stack.ClearInputLut();S::StackExecutionReport r;CHECK(stack.ProcessFrame(f,3,f->pts,&r));CHECK(r.inputLutStatus==S::StageStatus::Bypassed);av_frame_free(&f);}
static void SharedInputLutSnapshots(bool warp) {
 auto d=MakeDevice(warp);
 for(bool gpu:{false,true}) {
  S::StudioStack stack;
  if(gpu) CHECK(stack.InitializeGpu(d.dev.Get(),d.ctx.Get()));
  auto p=P::StudioPreset::Neutral();p.bypassAll=false;p.inputLutIntensity=1;
  stack.SetPreset(p);
  auto first=Lut(0),second=Lut(1);std::weak_ptr<const CubeLut> firstOwner=first;
  stack.SetInputLut(first);first.reset();CHECK(!firstOwner.expired());
  auto* f=Frame();S::StackExecutionReport report;
  auto* out=stack.ProcessFrame(f,1,f->pts,&report);CHECK(out);
  Compare(out,Bytes(f),gpu?1:0);CHECK(report.inputLutStatus==S::StageStatus::Applied);
  auto* retained=av_frame_clone(out);CHECK(retained);const auto saved=Bytes(retained);
  const auto expected=FrameOracle(f,0,1,second.get(),1);
  stack.SetInputLut(second);second.reset();
  out=stack.ProcessFrame(f,2,f->pts,&report);CHECK(out);
  Compare(out,expected,gpu?1:0);CHECK(firstOwner.expired());Compare(retained,saved);
  stack.SetInputLut(nullptr);out=stack.ProcessFrame(f,3,f->pts,&report);CHECK(out);
  Compare(out,Bytes(f));CHECK(report.inputLutStatus==S::StageStatus::Bypassed);Compare(retained,saved);
  stack.SetInputLut(Lut(1));stack.ClearInputLut();out=stack.ProcessFrame(f,4,f->pts,&report);CHECK(out);
  Compare(out,Bytes(f));CHECK(report.inputLutStatus==S::StageStatus::Bypassed);
  av_frame_free(&retained);av_frame_free(&f);
 }
}
int main(int argc,char** argv){if(argc>1&&std::string(argv[1])=="--fail")return 19;if(argc>1&&std::string(argv[1])=="--hang"){std::this_thread::sleep_for(std::chrono::hours(1));return 0;}try{
#define RUN(name,expr) std::cout<<"[TEST] " name <<std::endl;expr;std::cout<<"PASS " name <<std::endl;
 RUN("CpuExactRounding",CpuRounding());RUN("HardwareLookPixels",LookPixels(false));RUN("WarpLookPixels",LookPixels(true));RUN("StrictViewValidation",Validation());RUN("HardwareLutPixels",LutPixels(false));RUN("WarpLutPixels",LutPixels(true));RUN("OwnedBoundedContentCache",CacheAndContent());RUN("TransactionalPresets",PresetRoundTrip());RUN("HardwareCpuGpuStackParityAndFrameRetention",StackCpuAndGpu(false));RUN("WarpCpuGpuStackParityAndFrameRetention",StackCpuAndGpu(true));RUN("RealColourBoundaryFaultRecovery",ViewFaults());RUN("NativeFailureSameFrameFallbackLeaseEpochRetention",StackFaultsAndRetention());RUN("TransactionalStackConfig",PresetApplicationTransactional());
 RUN("HardwareSharedInputLutSnapshots",SharedInputLutSnapshots(false));RUN("WarpSharedInputLutSnapshots",SharedInputLutSnapshots(true));
 std::cout<<"SUMMARY 15/15 PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
