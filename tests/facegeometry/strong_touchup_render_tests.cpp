#include "effects/appearance/appearanceprocessor.h"
#include "effects/appearance/gpu/d3d11_appearanceprocessor.h"
#include "effects/gpu/d3d11_context.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
namespace A=PrismForge::Effects::Appearance;
namespace G=PrismForge::Effects::Gpu;
namespace {
constexpr int Width=128,Height=96;
int assertions=0;
void Require(bool yes,const char* why){++assertions;if(!yes)throw std::runtime_error(why);}
struct DeleteFrame{void operator()(AVFrame* f)const{av_frame_free(&f);}};
using Frame=std::unique_ptr<AVFrame,DeleteFrame>;
struct Fixture {
    Frame frame;
    std::vector<float> skin,protection;
    Fixture():frame(av_frame_alloc()),skin(Width*Height),protection(Width*Height) {
        Require(bool(frame),"frame allocation");frame->format=AV_PIX_FMT_BGR24;frame->width=Width;frame->height=Height;
        Require(av_frame_get_buffer(frame.get(),32)==0,"owned pixels allocation");
        frame->pts=194321;frame->time_base={1,1000000};
        for(int y=0;y<Height;++y)for(int x=0;x<Width;++x) {
            const bool eligible=x>=12&&x<116&&y>=8&&y<88;
            // Broad six-to-nine pixel patches plus fine grain. Pure patterned fixtures,
            // independent from any face detector or semantic skin assertion.
            const double wave=std::sin(x*6.283185307179586/18.0)*std::sin(y*6.283185307179586/14.0);
            const int textured=static_cast<int>(std::lround(24.0*wave))+((x+y)%2?6:-6);
            auto* p=frame->data[0]+size_t(y)*frame->linesize[0]+x*3;
            p[0]=uint8_t(112+textured);p[1]=uint8_t(145+textured);p[2]=uint8_t(167+textured);
            skin[size_t(y)*Width+x]=eligible?1.f:0.f;
            // Abstract feature masks representing paired eyes, brows and lips.
            const bool feature=((x>=25&&x<45)||(x>=79&&x<99))&&((y>=22&&y<28)||(y>=32&&y<42)) ||
                (x>=46&&x<80&&y>=77&&y<83);
            if(feature){protection[size_t(y)*Width+x]=1.f;p[0]=12;p[1]=8;p[2]=25;}
            if(!eligible){p[0]=230;p[1]=42;p[2]=17;}
        }
    }
    A::SkinMask Skin() const{return {skin.data(),Width,Height,Width};}
    A::ProtectionMask Protect() const{return {protection.data(),Width,Height,Width};}
};
std::vector<uint8_t> Bytes(const AVFrame* f){
    Require(f&&f->width==Width&&f->height==Height&&f->format==AV_PIX_FMT_BGR24,"valid output frame");
    std::vector<uint8_t> out(size_t(Width)*Height*3);
    for(int y=0;y<Height;++y)std::memcpy(out.data()+size_t(y)*Width*3,f->data[0]+size_t(y)*f->linesize[0],Width*3);
    return out;
}
A::AppearanceConfig Config(float boost){A::AppearanceConfig c;c.smoothingStrength=.65f;c.detailPreservation=0.f;c.sharpenStrength=0.f;c.touchupBoost=boost;return c;}
std::vector<uint8_t> Process(A::AppearanceProcessor& p,const Fixture& f,const A::AppearanceConfig& c){
    auto skin=f.Skin();auto protect=f.Protect();A::AppearanceOutcome outcome;
    auto* result=p.ProcessFrame(f.frame.get(),&skin,&protect,c,outcome);
    Require(result&&outcome==A::AppearanceOutcome::Success,"CPU successful render");
    Require(result->pts==f.frame->pts,"current timing preserved");return Bytes(result);
}
double TextureRms(const std::vector<uint8_t>& b){
    double square=0;size_t n=0;
    for(int y=51;y<74;++y)for(int x=28;x<100;++x) {
        const double delta=double(b[(size_t(y)*Width+x)*3])-112.0;square+=delta*delta;++n;
    }
    return std::sqrt(square/n);
}
void ProtectedExact(const Fixture& f,const std::vector<uint8_t>& input,const std::vector<uint8_t>& output){
    for(size_t i=0;i<f.skin.size();++i)if(f.skin[i]==0.f||f.protection[i]==1.f)
        for(size_t c=0;c<3;++c)Require(input[3*i+c]==output[3*i+c],"protected or background destination stays byte exact");
}
void Attenuation(){
    Fixture f;A::AppearanceProcessor p;auto before=Bytes(f.frame.get());
    auto old=Process(p,f,Config(0.f));auto medium=Process(p,f,Config(.5f));auto strong=Process(p,f,Config(1.f));
    const double input=TextureRms(before),legacy=TextureRms(old),mid=TextureRms(medium),maximum=TextureRms(strong);
    std::cout<<"TEXTURE_RMS input="<<input<<" legacy="<<legacy<<" boost50="<<mid<<" boost100="<<maximum<<"\n";
    Require(legacy<input,"legacy already smooths original texture");
    Require(mid<legacy,"middle boost attenuates more broad texture");
    Require(maximum<mid,"strongest boost improves over middle boost");
    Require(maximum<legacy*.7,"maximum removes at least30percent additional broad texture RMS");
    ProtectedExact(f,before,strong);
    std::cout<<"PASS stronger broad-patch attenuation and protected destinations\n";
}
void NeighborIsolation(){
    Fixture f;A::AppearanceProcessor p;const auto normal=Process(p,f,Config(1.f));
    for(size_t i=0;i<f.skin.size();++i)if(f.skin[i]==0.f||f.protection[i]==1.f){
        const size_t y=i/Width,x=i%Width;auto* pixel=f.frame->data[0]+y*f.frame->linesize[0]+x*3;
        pixel[0]=17;pixel[1]=249;pixel[2]=200;
    }
    const auto changed=Process(p,f,Config(1.f));
    for(size_t i=0;i<f.skin.size();++i)if(f.skin[i]==1.f&&f.protection[i]==0.f)
        for(size_t c=0;c<3;++c)Require(normal[3*i+c]==changed[3*i+c],"protected/outside neighbors cannot bleed into eligible output");
    ProtectedExact(f,Bytes(f.frame.get()),changed);
    std::cout<<"PASS high-contrast protected/background neighbor isolation\n";
}
void IdentityAndFinite(){
    Fixture f;A::AppearanceProcessor p;const auto before=Bytes(f.frame.get());auto cfg=Config(1.f);
    cfg.smoothingStrength=0.f;Require(Process(p,f,cfg)==before,"zero smoothing remains identity even boost1");
    cfg=Config(1.f);cfg.detailPreservation=1.f;Require(Process(p,f,cfg)==before,"detail1 identity even boost1");
    cfg=Config(1.f);cfg.bypass=true;auto skin=f.Skin();auto prot=f.Protect();A::AppearanceOutcome status;
    auto* output=p.ProcessFrame(f.frame.get(),&skin,&prot,cfg,status);
    Require(output&&status==A::AppearanceOutcome::Bypass&&Bytes(output)==before,"explicit bypass identity");
    auto legacy=Process(p,f,Config(0.f));
    for(float bad:{-5.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()})
        Require(Process(p,f,Config(bad))==legacy,"negative/nonfinite boost uses legacy safe result");
    Require(Process(p,f,Config(5.f))==Process(p,f,Config(1.f)),"positive boost clamped toone");
    cfg=Config(1.f);cfg.smoothingStrength=std::numeric_limits<float>::quiet_NaN();
    Require(Process(p,f,cfg)==before,"nonfinite smoothing is safe identity");
    cfg=Config(1.f);cfg.detailPreservation=std::numeric_limits<float>::infinity();
    Require(Process(p,f,cfg)==before,"nonfinite detail is safe identity");
    std::cout<<"PASS identities, bounds and finite parameter guards\n";
}
void CopyOnWrite(){
    Fixture f;A::AppearanceProcessor p;auto cfg=Config(1.f);auto skin=f.Skin();auto prot=f.Protect();A::AppearanceOutcome status;
    auto* output=p.ProcessFrame(f.frame.get(),&skin,&prot,cfg,status);Require(output&&status==A::AppearanceOutcome::Success,"initial COW output");
    Frame retained(av_frame_clone(output));Require(bool(retained),"retained clone allocation");const auto retainedBytes=Bytes(retained.get());
    for(int y=0;y<Height;++y)for(int x=0;x<Width;++x){auto* pixel=f.frame->data[0]+size_t(y)*f.frame->linesize[0]+x*3;pixel[0]=pixel[1]=pixel[2]=120;}
    const auto current=Process(p,f,cfg);Require(current!=retainedBytes,"next frame changed processed pixels");
    Require(Bytes(retained.get())==retainedBytes,"retained old frame cannot be mutated by reuse");
    std::cout<<"PASS retained-output copy-on-write\n";
}
void GpuParity(bool hardware){
    G::D3D11Context context;Require(context.Initialize(hardware),"GPU fixture device initialization");
    if(hardware)Require(context.IsHardwareAdapter(),"hardware pass actually uses hardware");
    else Require(!context.IsHardwareAdapter(),"software pass actually usesWARP");
    Fixture f;A::AppearanceProcessor cpu;A::Gpu::D3D11AppearanceProcessorAdapter gpu(context.GetDevice(),context.GetContext());
    auto skin=f.Skin();auto protect=f.Protect();const auto before=Bytes(f.frame.get());int maxDelta=0;
    for(float boost:{0.f,.5f,1.f,-3.f,5.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
        const auto cfg=Config(boost);const auto expected=Process(cpu,f,cfg);A::AppearanceOutcome status;A::Gpu::D3D11AppearanceProcessorAdapter::TimingStats timing;
        auto* output=gpu.ProcessFrame(f.frame.get(),&skin,&protect,cfg,status,&timing);
        Require(output&&status==A::AppearanceOutcome::Success&&!timing.fallbackUsed,"actual GPU render with no CPU fallback");
        const auto actual=Bytes(output);for(size_t i=0;i<actual.size();++i){const int d=std::abs(int(actual[i])-int(expected[i]));maxDelta=std::max(maxDelta,d);Require(d<=1,"CPU/GPU channel agreement withinonebyte");}
        ProtectedExact(f,before,actual);
    }
    for(int neutral=0;neutral<3;++neutral){auto cfg=Config(1.f);if(neutral==0)cfg.smoothingStrength=0.f;if(neutral==1)cfg.detailPreservation=1.f;if(neutral==2)cfg.bypass=true;
        A::AppearanceOutcome status;A::Gpu::D3D11AppearanceProcessorAdapter::TimingStats timing;
        auto* output=gpu.ProcessFrame(f.frame.get(),&skin,&protect,cfg,status,&timing);
        Require(output&&!timing.fallbackUsed&&Bytes(output)==before,"GPU high-boost neutral/bypass exactidentity");
    }
    std::cout<<"PASS "<<(hardware?"hardware":"WARP")<<" strong/legacy/finite/identity parity maxChannelDelta="<<maxDelta<<"\n";
}
}
int main(){try{Attenuation();NeighborIsolation();IdentityAndFinite();CopyOnWrite();GpuParity(true);GpuParity(false);
    std::cout<<"STRONG_TOUCHUP_RENDER PASS groups=5 assertions="<<assertions<<" (GPU hardware/WARP repeats deduplicated)\n";return 0;
}catch(const std::exception& e){std::cerr<<"STRONG_TOUCHUP_RENDER FAILED: "<<e.what()<<" assertions="<<assertions<<"\n";return 1;}}
