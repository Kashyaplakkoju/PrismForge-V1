#include "effects/desktopstudiopipeline.h"
#include "video/directshowscaler.h"
#include "video/guipreviewscaler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace PrismForge::Effects;
using namespace PrismForge::Effects::StudioStack;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; return false; } } while(false)
struct Frame {
    AVFrame* f=av_frame_alloc();
    Frame(int w=161,int h=91,AVPixelFormat fmt=AV_PIX_FMT_BGR24) {
        if(!f)throw std::bad_alloc();f->width=w;f->height=h;f->format=fmt;
        if(av_frame_get_buffer(f,64)<0)throw std::bad_alloc();
        f->pts=1234;f->time_base={1,1000};f->duration=33;f->pkt_dts=1220;
        f->best_effort_timestamp=1230;f->sample_aspect_ratio={1,1};
        f->color_range=AVCOL_RANGE_JPEG;f->colorspace=AVCOL_SPC_RGB;
        f->color_primaries=AVCOL_PRI_BT709;f->color_trc=AVCOL_TRC_BT709;
    }
    ~Frame(){av_frame_free(&f);}
    void Fill(int b=100,int g=100,int r=100) {
        if(f->format==AV_PIX_FMT_NV12) {
            for(int y=0;y<f->height;++y)std::memset(f->data[0]+y*f->linesize[0],b,f->width);
            for(int y=0;y<(f->height+1)/2;++y)std::memset(f->data[1]+y*f->linesize[1],128,((f->width+1)/2)*2);
            f->color_range=AVCOL_RANGE_MPEG;f->colorspace=AVCOL_SPC_BT709;return;
        }
        int n=f->format==AV_PIX_FMT_RGBA?4:3;
        for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x){auto*p=f->data[0]+y*f->linesize[0]+n*x;
            p[0]=f->format==AV_PIX_FMT_RGBA?r:b;p[1]=g;p[2]=f->format==AV_PIX_FMT_RGBA?b:r;if(n==4)p[3]=255;}
    }
};
std::vector<uint8_t> Bytes(const AVFrame* f) {
    if(!f||f->format!=AV_PIX_FMT_BGR24)return{};std::vector<uint8_t> b;
    b.reserve(size_t(f->width)*f->height*3);
    for(int y=0;y<f->height;++y)b.insert(b.end(),f->data[0]+y*f->linesize[0],f->data[0]+y*f->linesize[0]+f->width*3);
    return b;
}
std::array<int,3> Pixel(const AVFrame* f,int x=-1,int y=-1) {
    if(x<0)x=f->width/2;if(y<0)y=f->height/2;const auto*p=f->data[0]+y*f->linesize[0]+x*3;return {p[0],p[1],p[2]};
}
DesktopStudioSettings Light(DesktopLightMode m=DesktopLightMode::Warm,DesktopLightDirection d=DesktopLightDirection::Left,float strength=.5f) {
    DesktopStudioSettings s;s.light=m;s.direction=d;s.strength=strength;return s;
}
int Diff(const std::vector<uint8_t>&a,const std::vector<uint8_t>&b) {
    if(a.size()!=b.size())return 1000;int d=0;for(size_t i=0;i<a.size();++i)d=std::max(d,std::abs(int(a[i])-b[i]));return d;
}
std::shared_ptr<const CubeLut> SwapLut() {
    std::string s="LUT_3D_SIZE 2\n";
    for(int b=0;b<2;++b)for(int g=0;g<2;++g)for(int r=0;r<2;++r)s+=std::to_string(b)+" "+std::to_string(g)+" "+std::to_string(r)+"\n";
    return CubeLut::Parse(s);
}
bool Neutral() {DesktopStudioPipeline p(false);Frame f;f.Fill();CHECK(p.Process(nullptr)==nullptr);CHECK(p.Process(f.f,7)==f.f);auto r=p.GetLastReport();CHECK(r.sourceGeneration==7&&r.frameSequence==1);CHECK(r.imageUploadCount==0&&r.imageReadbackCount==0);CHECK(r.lightingStatus==StageStatus::Bypassed);return true;}
bool WarmCool() {DesktopStudioPipeline p(false);Frame f;f.Fill();p.ConfigureStudio(Light());auto*out=p.Process(f.f);CHECK(out!=f.f&&p.GetLastReport().lightingStatus==StageStatus::Applied);auto a=Pixel(out);CHECK(a[2]>a[0]);p.ConfigureStudio(Light(DesktopLightMode::Cool));a=Pixel(p.Process(f.f));CHECK(a[0]>a[2]);CHECK(p.GetLastReport().analysisStatus==StageStatus::Bypassed&&p.GetLastReport().backgroundStatus==StageStatus::Bypassed);return true;}
bool Direction() {DesktopStudioPipeline p(false);Frame f;f.Fill();p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Left,1));auto*out=p.Process(f.f);auto left=Pixel(out,20,45),right=Pixel(out,140,45);CHECK(left[2]>right[2]);p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Right,1));out=p.Process(f.f);CHECK(Pixel(out,140,45)[2]>Pixel(out,20,45)[2]);p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Front,1));out=p.Process(f.f);CHECK(std::abs(Pixel(out,20,45)[2]-Pixel(out,140,45)[2])<=1);return true;}
bool Strength() {DesktopStudioPipeline p(false);Frame f;f.Fill();auto original=Bytes(f.f);p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Front,0));CHECK(p.Process(f.f)==f.f);p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Front,.01f));CHECK(Diff(Bytes(p.Process(f.f)),original)<=1);p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Front,.25f));auto a=Bytes(p.Process(f.f));p.ConfigureStudio(Light(DesktopLightMode::Warm,DesktopLightDirection::Front,1));auto b=Bytes(p.Process(f.f));CHECK(a!=original&&b!=a);CHECK(Pixel(p.Process(f.f))[2]>100);return true;}
bool Border() {DesktopStudioPipeline p(false);Frame f(640,360);f.Fill();auto s=DesktopStudioSettings{};s.studioFrame=true;p.ConfigureStudio(s);auto*out=p.Process(f.f);CHECK(Bytes(out)!=Bytes(f.f));CHECK(Pixel(out)==Pixel(f.f));CHECK(p.GetLastReport().overlayStatus==StageStatus::Applied&&p.GetLastReport().lightingStatus==StageStatus::Bypassed);return true;}
bool MasterBypass() {DesktopStudioPipeline p(false);Frame f;f.Fill(45,80,145);auto s=Light();s.studioFrame=true;p.ConfigureStudio(s);DesktopColorSettings c;c.look=Look::Warm;c.lut=SwapLut();p.Configure(c);auto expected=Bytes(p.Process(f.f));c.bypassed=true;p.Configure(c);CHECK(p.Process(f.f)==f.f);CHECK(p.GetStudioSettings().studioFrame&&p.GetStudioSettings().light==DesktopLightMode::Warm&&p.GetSettings().lut==c.lut);CHECK(p.GetLastReport().imageUploadCount==0);c.bypassed=false;p.Configure(c);CHECK(Bytes(p.Process(f.f))==expected);return true;}
bool ColorOrder() {Frame f;f.Fill(31,90,173);DesktopColorSettings c;c.look=Look::Warm;c.lut=SwapLut();c.intensity=.75f;DesktopColorPipeline color;color.Configure(c);auto*cooked=color.Process(f.f);DesktopStudioPipeline expected(false);expected.ConfigureStudio(Light(DesktopLightMode::Cool));auto bytes=Bytes(expected.Process(cooked));DesktopStudioPipeline p(false);p.Configure(c);p.ConfigureStudio(Light(DesktopLightMode::Cool));CHECK(Bytes(p.Process(f.f))==bytes);p.ConfigureStudio({});CHECK(Bytes(p.Process(f.f))==Bytes(color.Process(f.f)));return true;}
bool Generation() {DesktopStudioPipeline p(false);Frame f;f.Fill();p.ConfigureStudio(Light());p.Process(f.f,44);auto r=p.GetLastReport();CHECK(r.frameSequence==1&&r.sourceGeneration==44&&r.timestampPts==1234000);p.Process(f.f,44);CHECK(p.GetLastReport().frameSequence==2);f.f->pts=11;f.Fill(20,30,40);auto*out=p.Process(f.f,55);r=p.GetLastReport();CHECK(r.frameSequence==1&&r.sourceGeneration==55&&r.timestampPts==11000&&out->pts==11);p.Process(f.f,0);CHECK(p.GetLastReport().sourceGeneration==1&&p.GetLastReport().frameSequence==1);return true;}
bool Retention() {for(bool allowGpu:{false,true}){AVFrame* held=nullptr;std::vector<uint8_t> before;{DesktopStudioPipeline p(allowGpu);p.ConfigureStudio(Light());Frame f;f.Fill();held=av_frame_clone(p.Process(f.f));CHECK(held);before=Bytes(held);for(int i=0;i<8;++i){f.Fill(20+i,30+i,40+i);p.Process(f.f,i+5);CHECK(Bytes(held)==before);}Frame larger(322,182);larger.Fill();p.Process(larger.f,90);CHECK(Bytes(held)==before);}CHECK(Bytes(held)==before);av_frame_free(&held);}return true;}
bool Metadata() {DesktopStudioPipeline p(false);p.ConfigureStudio(Light());Frame f;f.Fill();av_dict_set(&f.f->metadata,"first","camera-one",0);auto*side=av_frame_new_side_data(f.f,AV_FRAME_DATA_SEI_UNREGISTERED,16);CHECK(side);std::memset(side->data,23,16);auto*out=p.Process(f.f);CHECK(out->pts==1234&&out->duration==33&&out->pkt_dts==1220&&out->time_base.den==1000);CHECK(av_dict_get(out->metadata,"first",nullptr,0));CHECK(av_frame_get_side_data(out,AV_FRAME_DATA_SEI_UNREGISTERED));av_dict_free(&f.f->metadata);av_frame_remove_side_data(f.f,AV_FRAME_DATA_SEI_UNREGISTERED);av_dict_set(&f.f->metadata,"second","camera-two",0);f.f->pts=33;out=p.Process(f.f,2);CHECK(out->pts==33&&!av_dict_get(out->metadata,"first",nullptr,0));CHECK(av_dict_get(out->metadata,"second",nullptr,0));CHECK(!av_frame_get_side_data(out,AV_FRAME_DATA_SEI_UNREGISTERED));return true;}
bool Formats() {DesktopStudioPipeline p(false);p.ConfigureStudio(Light());for(auto fmt:{AV_PIX_FMT_RGBA,AV_PIX_FMT_NV12}){Frame f(162,92,fmt);f.Fill(100,100,100);auto*out=p.Process(f.f,fmt+1);CHECK(out&&out!=f.f&&out->format==AV_PIX_FMT_BGR24);CHECK(out->color_range==AVCOL_RANGE_JPEG&&out->colorspace==AVCOL_SPC_RGB);CHECK(Pixel(out)[2]>Pixel(out)[0]);CHECK(p.GetLastReport().lightingStatus==StageStatus::Applied);}return true;}
bool InvalidPlanes() {DesktopStudioPipeline p(false);p.ConfigureStudio(Light());DesktopColorSettings c;c.look=Look::Warm;c.lut=SwapLut();p.Configure(c);Frame f;f.Fill();auto bytes=Bytes(f.f);auto pitch=f.f->linesize[0];f.f->linesize[0]=1;CHECK(p.Process(f.f)==f.f);CHECK(p.GetLastReport().lightingStatus==StageStatus::Failed);f.f->linesize[0]=pitch;auto*data=f.f->data[0];f.f->data[0]=f.f->buf[0]->data+f.f->buf[0]->size-2;CHECK(p.Process(f.f)==f.f);CHECK(p.GetLastReport().lightingStatus==StageStatus::Failed);f.f->data[0]=data;CHECK(Bytes(f.f)==bytes);return true;}
bool UnsupportedHdr() {DesktopStudioPipeline p(false);p.ConfigureStudio(Light());Frame f;f.Fill();f.f->color_trc=AVCOL_TRC_SMPTE2084;CHECK(p.Process(f.f)==f.f&&p.GetLastReport().lightingStatus==StageStatus::Unsupported);f.f->color_trc=AVCOL_TRC_ARIB_STD_B67;CHECK(p.Process(f.f)==f.f);f.f->color_trc=AVCOL_TRC_BT709;f.f->color_range=AVCOL_RANGE_MPEG;auto prior=Bytes(f.f);CHECK(p.Process(f.f)==f.f&&p.GetLastReport().lightingStatus==StageStatus::Unsupported&&Bytes(f.f)==prior&&f.f->color_range==AVCOL_RANGE_MPEG);return true;}
bool ConfigSnapshots() {DesktopStudioPipeline p(false);Frame f;f.Fill();auto a=Light(),b=Light(DesktopLightMode::Cool,DesktopLightDirection::Right,.75f);p.ConfigureStudio(a);auto first=Bytes(p.Process(f.f));p.ConfigureStudio(b);auto second=Bytes(p.Process(f.f));std::atomic<bool> stop{false};std::thread gui([&]{while(!stop){p.ConfigureStudio(a);p.ConfigureStudio(b);}});bool valid=true;for(int i=0;i<80;++i){auto bytes=Bytes(p.Process(f.f));if(bytes!=first&&bytes!=second)valid=false;}stop=true;gui.join();CHECK(valid);a.strength=std::numeric_limits<float>::quiet_NaN();a.light=static_cast<DesktopLightMode>(99);a.direction=static_cast<DesktopLightDirection>(99);p.ConfigureStudio(a);CHECK(p.GetStudioSettings().light==DesktopLightMode::Off&&p.GetStudioSettings().strength==0&&p.GetStudioSettings().direction==DesktopLightDirection::Left);return true;}
bool ConsumerParity() {DesktopStudioPipeline p(false);p.ConfigureStudio(Light());Frame f(160,90);f.Fill();auto*out=p.Process(f.f);auto expected=Bytes(out);Video::GuiPreviewScaler preview(160,90);Video::DirectShowScaler camera(160,90);int w=0,h=0;auto*rgb=preview.Process(out,w,h);CHECK(rgb&&w==160&&h==90);for(int i=0;i<w*h;++i)CHECK(rgb[i*3]==expected[i*3]&&rgb[i*3+1]==expected[i*3+1]&&rgb[i*3+2]==expected[i*3+2]);auto*bgr=camera.Process(out,w,h);CHECK(bgr&&w==160&&h==90);CHECK(std::memcmp(bgr,expected.data(),expected.size())==0);auto*shot=av_frame_clone(out);CHECK(shot&&Bytes(shot)==expected);av_frame_free(&shot);return true;}
bool GpuRecovery() {DesktopStudioPipeline cpu(false),gpu(true);Frame f(320,180);f.Fill(100,90,80);auto s=Light(DesktopLightMode::Balanced);s.studioFrame=true;cpu.ConfigureStudio(s);gpu.ConfigureStudio(s);auto expected=Bytes(cpu.Process(f.f,7));auto got=Bytes(gpu.Process(f.f,7));auto r=gpu.GetLastReport();CHECK(r.lightingStatus==StageStatus::Applied&&r.overlayStatus==StageStatus::Applied);CHECK(r.imageUploadCount==1&&r.imageReadbackCount==1&&!r.gpuFallbackUsed);CHECK(Diff(expected,got)<=2);f.Fill(43,53,63);expected=Bytes(cpu.Process(f.f,8));gpu.InjectGpuReadbackFailureForTesting(true);got=Bytes(gpu.Process(f.f,8));r=gpu.GetLastReport();CHECK(r.gpuFallbackUsed&&r.lightingStatus==StageStatus::Applied&&r.overlayStatus==StageStatus::Applied);CHECK(got==expected&&r.sourceGeneration==8&&r.frameSequence==1);gpu.InjectGpuReadbackFailureForTesting(false);got=Bytes(gpu.Process(f.f,8));r=gpu.GetLastReport();CHECK(!r.gpuFallbackUsed&&r.imageUploadCount==1&&r.imageReadbackCount==1);CHECK(Diff(got,expected)<=2);return true;}
bool Benchmark() {for(auto wh:{std::array<int,2>{1280,720},std::array<int,2>{1920,1080}})for(bool lut:{false,true}){DesktopStudioPipeline p;Frame f(wh[0],wh[1]);auto s=Light(DesktopLightMode::Balanced);s.studioFrame=true;p.ConfigureStudio(s);DesktopColorSettings c;c.look=lut?Look::Warm:Look::Original;if(lut)c.lut=SwapLut();p.Configure(c);std::vector<double> ms;volatile int observer=0;for(int i=0;i<21;++i){f.Fill(70+i%9,90+i%13,110+i%17);auto start=std::chrono::steady_clock::now();auto*out=p.Process(f.f,1);auto stop=std::chrono::steady_clock::now();auto r=p.GetLastReport();CHECK(out&&out!=f.f&&r.lightingStatus==StageStatus::Applied&&r.overlayStatus==StageStatus::Applied);CHECK(r.imageUploadCount==1&&r.imageReadbackCount==1&&!r.gpuFallbackUsed);observer+=Pixel(out)[0];if(i>=5)ms.push_back(std::chrono::duration<double,std::milli>(stop-start).count());}std::sort(ms.begin(),ms.end());std::cout<<"BENCH "<<wh[0]<<"x"<<wh[1]<<" lighting+border"<<(lut?"+warm+2nodeLUT":"")<<" total p50="<<ms[7]<<"ms p95="<<ms[15]<<"ms samples=16 warmup=5 software upload/readback=1/1\n";CHECK(observer>0);}return true;}
int main(int argc,char**argv){if(argc>1&&std::string(argv[1])=="--bench")return Benchmark()?0:1;
    struct Case{const char*name;bool(*fn)();};std::vector<Case> cases={{"neutral passthrough",Neutral},{"warm/cool no fake analysis",WarmCool},{"left/front/right lighting",Direction},{"strength and zero bypass",Strength},{"screen border",Border},{"master bypass/restore",MasterBypass},{"legacy look/LUT ordering",ColorOrder},{"source generations and PTS",Generation},{"retained output across churn/destruction",Retention},{"current metadata without stale side data",Metadata},{"RGBA/NV12 SDR conversion",Formats},{"plane owner bounds before processing",InvalidPlanes},{"HDR explicit unsupported",UnsupportedHdr},{"concurrent immutable settings",ConfigSnapshots},{"preview/virtual/snapshot common pixels",ConsumerParity},{"hardware parity/readback failure/recovery",GpuRecovery}};
    int passed=0;for(auto&t:cases){try{if(t.fn()){++passed;std::cout<<"PASS: "<<t.name<<"\n";}else std::cout<<"FAIL: "<<t.name<<"\n";}catch(const std::exception&e){std::cout<<"FAIL: "<<t.name<<" exception "<<e.what()<<"\n";}}
    std::cout<<passed<<"/"<<cases.size()<<" desktop Studio integration cases passed\n";return passed==cases.size()?0:1;}
