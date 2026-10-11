#include "effects/desktopstudiopipeline.h"
#include "effects/analysisbridge/mask_preparer.h"
#include "video/guipreviewscaler.h"
#include "video/directshowscaler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace PrismForge::Effects;
namespace A=PrismForge::Effects::AnalysisBridge;
namespace S=PrismForge::Effects::StudioStack;
namespace P=PrismForge::Providers;
static int groups=0, assertions=0;
static void Require(bool condition,const char* description) {++assertions;if(!condition)throw std::runtime_error(description);}
struct Frame {
    AVFrame* f=av_frame_alloc();
    Frame(int width=160,int height=96) {
        Require(f!=nullptr,"allocate current camera frame");f->width=width;f->height=height;f->format=AV_PIX_FMT_BGR24;
        Require(av_frame_get_buffer(f,64)>=0,"owned current camera pixels");f->pts=100000;f->time_base={1,1000000};f->duration=16666;
        f->color_range=AVCOL_RANGE_JPEG;f->colorspace=AVCOL_SPC_RGB;f->color_primaries=AVCOL_PRI_BT709;f->color_trc=AVCOL_TRC_BT709;Fill();
    }
    ~Frame(){av_frame_free(&f);}
    void Fill(int b=64,int g=76,int r=92){Require(av_frame_make_writable(f)>=0,"camera frame writable");
        for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x){auto*p=f->data[0]+y*f->linesize[0]+x*3;p[0]=b;p[1]=g;p[2]=r;}}
};
static std::array<int,3> Pixel(const AVFrame* f,int x,int y){Require(f&&f->format==AV_PIX_FMT_BGR24,"BGR expected pixel");auto*p=f->data[0]+y*f->linesize[0]+x*3;return {p[0],p[1],p[2]};}
static std::vector<uint8_t> Bytes(const AVFrame* f){Require(f&&f->format==AV_PIX_FMT_BGR24,"BGR current output");std::vector<uint8_t> bytes;
    for(int y=0;y<f->height;++y)bytes.insert(bytes.end(),f->data[0]+y*f->linesize[0],f->data[0]+y*f->linesize[0]+f->width*3);return bytes;}
static int Difference(const std::vector<uint8_t>&a,const std::vector<uint8_t>&b){Require(a.size()==b.size(),"same expected geometry");int d=0;
    for(size_t i=0;i<a.size();++i)d=std::max(d,std::abs(int(a[i])-int(b[i])));return d;}
static DesktopStudioSettings Custom(){DesktopStudioSettings s;s.light=DesktopLightMode::Custom;s.strength=.8f;return s;}
static void Applied(DesktopStudioPipeline& p,bool gpu){const auto r=p.GetLastReport();Require(r.lightingStatus==S::StageStatus::Applied,"lighting actually applied");
    Require(r.effectiveLightingMode==Lighting::LightingMode::SpatialApproximation,"truthful spatial mode without normals");
    Require(!r.gpuFallbackUsed&&!r.fallbackToOriginal,"no silent fallback");
    Require(r.imageUploadCount==(gpu?1:0)&&r.imageReadbackCount==(gpu?1:0),"actual expected completed hardware transfer path");}
static double Linear(double s){return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);}
static int Srgb(double l){l=std::clamp(l,0.,1.);const double s=l<=.0031308?12.92*l:1.055*std::pow(l,1./2.4)-.055;return std::clamp(int(s*255+.5),0,255);}
// Independent double-precision blackbody chromaticity and position oracle. It
// never invokes the renderer or its conversion/lighting helpers.
static std::array<double,3> Colour(const Lighting::StudioLight& light){double t=light.colorTemperature/100.;
    double r=t<=66?255.:329.698727446*std::pow(t-60.,-.1332047592);
    double g=t<=66?99.4708025861*std::log(std::max(1.,t))-161.1195681661:288.1221695283*std::pow(std::max(1.,t-60.),-.0755148492);
    double b=t>=66?255.:t<=19?0.:138.5177312231*std::log(std::max(1.,t-10.))-305.0447927307;
    std::array<double,3> c={Linear(std::clamp(b,0.,255.)/255),Linear(std::clamp(g,0.,255.)/255)*(1.+light.tint*.25),Linear(std::clamp(r,0.,255.)/255)};
    double peak=std::max({c[0],c[1],c[2]});for(auto&value:c)value=std::clamp(value/peak,0.,1.);return c;}
static double Spatial(const Lighting::StudioLight&light,int x,int y,int w,int h){if(!light.enabled||light.intensity<=0)return 0;
    double u=2.*(x+.5)/w-1.,v=2.*(y+.5)/h-1.;double dx=(light.posX-u)*w/h,dy=light.posY-v,z=light.posZ,d=std::sqrt(dx*dx+dy*dy+z*z);
    double nx=.45*u,ny=.35*v,nz=std::sqrt(std::max(.05,1.-nx*nx-ny*ny)),n=std::sqrt(nx*nx+ny*ny+nz*nz);
    double diffuse=std::max(0.,.35+.65*(nx*dx+ny*dy+nz*z)/(n*d));return diffuse/(1.+.4*std::max(0.,d-z));}
static std::array<int,3> Expected(const Frame& f,const DesktopStudioSettings&s,int x,int y,double alpha=1.){auto src=Pixel(f.f,x,y);auto key=Colour(s.keyLight),fill=Colour(s.fillLight);
    std::array<int,3> output{};double a=Spatial(s.keyLight,x,y,f.f->width,f.f->height)*s.keyLight.intensity*s.strength;
    double b=Spatial(s.fillLight,x,y,f.f->width,f.f->height)*s.fillLight.intensity*s.strength;
    for(int c=0;c<3;++c){double multiplier=1.-.05*s.strength+key[c]*a+fill[c]*b;int lit=Srgb(Linear(src[c]/255.)*multiplier);output[c]=int(src[c]*(1-alpha)+lit*alpha+.5);}return output;}
static void OraclePixels(){for(bool gpu:{false,true}){DesktopStudioPipeline p(gpu);Frame f;auto s=Custom();p.ConfigureStudio(s);auto*out=p.Process(f.f,7);Applied(p,gpu);
    Require(p.GetLastReport().analysisStatus==S::StageStatus::Bypassed&&p.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"whole-frame custom does not create fake analysis");
    for(auto point:{std::array<int,2>{17,19},std::array<int,2>{80,48},std::array<int,2>{142,79}}){auto expected=Expected(f,s,point[0],point[1]);auto got=Pixel(out,point[0],point[1]);for(int c=0;c<3;++c)Require(std::abs(expected[c]-got[c])<=1,"independent two-source linear-colour expected pixel");}
    Require(Bytes(out)!=Bytes(f.f),"custom is not bypass-only");}}
static void InactiveCustomLights(){for(bool gpu:{false,true}){DesktopStudioPipeline p(gpu);Frame f;auto s=Custom();s.lightingSubjectOnly=true;
    s.keyLight.intensity=s.fillLight.intensity=0;p.ConfigureStudio(s);Require(p.Process(f.f,7)==f.f,"zero custom intensities exact current-camera bypass");
    auto r=p.GetLastReport();Require(r.lightingStatus==S::StageStatus::Bypassed&&r.imageUploadCount==0&&r.imageReadbackCount==0,"zero lights no work or transfers");Require(p.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"zero custom lights never start person inference");
    s.keyLight.intensity=s.fillLight.intensity=1;s.keyLight.enabled=s.fillLight.enabled=false;p.ConfigureStudio(s);Require(p.Process(f.f,7)==f.f,"disabled custom lights exact current-camera bypass");Require(p.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"disabled custom lights never start inference");
    s.keyLight.enabled=true;s.strength=0;p.ConfigureStudio(s);Require(p.Process(f.f,7)==f.f,"zero strength custom bypass");Require(p.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"zero custom strength never starts inference");
    s.strength=1;DesktopColorSettings c;c.look=Look::Mono;p.Configure(c);s.keyLight.enabled=false;p.ConfigureStudio(s);auto*out=p.Process(f.f,7);r=p.GetLastReport();Require(r.inputLookStatus==S::StageStatus::Applied&&r.lightingStatus==S::StageStatus::Bypassed,"inactive custom permits unrelated effects");Require(Pixel(out,20,20)[0]==Pixel(out,20,20)[2],"inactive custom preserves actual other processing");}}
static void IndependentLights(){for(bool gpu:{false,true}){DesktopStudioPipeline p(gpu);Frame f;f.Fill(90,90,90);auto s=Custom();s.fillLight.enabled=false;s.keyLight.posX=-.8f;s.keyLight.posY=-.4f;p.ConfigureStudio(s);
    auto*out=p.Process(f.f,7);Applied(p,gpu);Require(Pixel(out,25,30)[2]>Pixel(out,134,30)[2],"key follows left placement");auto warm=Bytes(out);
    s.keyLight.posX=.8f;p.ConfigureStudio(s);out=p.Process(f.f,7);Applied(p,gpu);Require(Pixel(out,134,30)[2]>Pixel(out,25,30)[2],"key follows right placement");
    s.keyLight.colorTemperature=11000;p.ConfigureStudio(s);out=p.Process(f.f,7);Require(Pixel(out,134,30)[0]>Pixel(out,134,30)[2],"Kelvin routes independent cool chromaticity");Require(Bytes(out)!=warm,"Kelvin materially affects source");
    s.keyLight.enabled=false;s.fillLight.enabled=true;s.fillLight.posX=-.6f;s.fillLight.posY=.6f;s.fillLight.intensity=1.4f;s.fillLight.colorTemperature=2500;p.ConfigureStudio(s);out=p.Process(f.f,7);Applied(p,gpu);
    Require(Pixel(out,35,72)[2]>Pixel(out,125,20)[2],"fill independent position and warm chromaticity");auto strong=Bytes(out);s.fillLight.intensity=.15f;p.ConfigureStudio(s);auto weak=Bytes(p.Process(f.f,7));Require(weak!=strong,"fill intensity independent");}}
static auto Bundle(uint64_t generation,uint64_t sequence,int64_t pts,int kind=1){auto b=std::make_shared<A::AnalysisBundle>();b->sourceGeneration=generation;b->sequenceId=sequence;b->timestampPts=pts;
    auto&m=b->personMask;m.width=20;m.height=12;m.strideFloats=23;m.data.assign(size_t(m.strideFloats)*m.height,kind==2?1.f:0.f);
    if(kind==1)for(int y=3;y<=8;++y)for(int x=6;x<=13;++x)m.data[y*m.strideFloats+x]=1;return b;}
static auto Masked(DesktopStudioPipeline&p,Frame&f,uint64_t generation=7){auto box=std::make_shared<A::AnalysisMailbox>();p.AttachAnalysisMailboxForTesting(box);auto s=Custom();s.lightingSubjectOnly=true;p.ConfigureStudio(s);Require(p.Process(f.f,generation)==f.f,"first masked frame unchanged while waiting");return box;}
static void SubjectProtection(){for(bool gpu:{false,true}){DesktopStudioPipeline p(gpu);Frame f;auto box=Masked(p,f);Require(box->PostAnalysis(Bundle(7,1,f.f->pts)),"post actual-shaped subject oracle mask");auto*out=p.Process(f.f,7);Applied(p,gpu);
    Require(p.GetLastReport().backgroundStatus==S::StageStatus::Bypassed,"lighting-only does not imply background error");Require(Pixel(out,0,0)==Pixel(f.f,0,0),"background pixels protected exactly");Require(Pixel(out,80,48)!=Pixel(f.f,80,48),"foreground lit");
    auto expected=Expected(f,p.GetStudioSettings(),80,48);auto got=Pixel(out,80,48);for(int c=0;c<3;++c)Require(std::abs(expected[c]-got[c])<=1,"foreground independently expected");
    Frame zero(160,96);DesktopStudioPipeline q(gpu);auto zeroBox=Masked(q,zero);Require(zeroBox->PostAnalysis(Bundle(7,1,zero.f->pts,0)),"zero oracle mask");Require(Bytes(q.Process(zero.f,7))==Bytes(zero.f),"zero subject mask exact identity");Applied(q,gpu);
    auto kept=Bytes(out);auto clone=av_frame_clone(out);Require(clone!=nullptr,"retained custom frame");f.Fill(41,69,100);f.f->pts+=16666;p.Process(f.f,7);Require(Bytes(clone)==kept,"processed consumer retains previous pixels");av_frame_free(&clone);}}
static void MissingStaleAndOtherEffects(){for(bool gpu:{false,true}){DesktopStudioPipeline p(gpu);Frame f;auto box=Masked(p,f);auto r=p.GetLastReport();Require(r.lightingStatus==S::StageStatus::NotReady&&r.backgroundStatus==S::StageStatus::Bypassed,"pending lighting truthfully reported");
    Require(r.imageUploadCount==0&&r.imageReadbackCount==0,"pending lighting-only never transfers");Require(box->PostAnalysis(Bundle(7,40,f.f->pts+1000000)),"future oracle post");Require(Bytes(p.Process(f.f,7))==Bytes(f.f),"future results cannot illuminate");
    DesktopColorSettings c;c.look=Look::Mono;p.Configure(c);auto*out=p.Process(f.f,7);r=p.GetLastReport();Require(r.inputLookStatus==S::StageStatus::Applied&&r.lightingStatus==S::StageStatus::NotReady,"missing subject leaves independent look active");Require(Pixel(out,20,20)[0]==Pixel(out,20,20)[2],"actual mono output survives missing mask");
    p.Configure({});p.Process(f.f,8);Require(box->PostAnalysis(Bundle(8,1,f.f->pts)),"current mask");p.Process(f.f,8);f.f->pts+=200000;Require(Bytes(p.Process(f.f,8))==Bytes(f.f),"expired mask never retained");
    Require(p.GetLastReport().lightingStatus==S::StageStatus::NotReady,"expired status");Frame portrait(96,160);Require(Bytes(p.Process(portrait.f,8))==Bytes(portrait.f),"same-source geometry clears masks");
    Require(p.GetLastReport().lightingStatus==S::StageStatus::NotReady,"geometry change waits for new mask");Require(!box->PostAnalysis(Bundle(7,60,portrait.f->pts)),"obsolete generation result rejected");}}
static void BypassAndSourceMetadata(){for(bool gpu:{false,true}){DesktopStudioPipeline p(gpu);Frame f;auto s=Custom();p.ConfigureStudio(s);auto expected=Bytes(p.Process(f.f,7));auto copy=av_frame_clone(p.Process(f.f,7));Require(copy!=nullptr,"retained custom fanout");
    DesktopColorSettings c;c.bypassed=true;p.Configure(c);Require(p.Process(f.f,7)==f.f,"master bypass exact pointer");Require(p.GetStudioSettings().light==DesktopLightMode::Custom&&p.GetStudioSettings().keyLight.colorTemperature==3200,"bypass retains all custom controls");c.bypassed=false;p.Configure(c);
    Require(Bytes(p.Process(f.f,7))==expected,"unbypass restores custom");Frame other(96,160);other.Fill(28,66,101);other.f->pts=340000;auto*out=p.Process(other.f,9);Applied(p,gpu);
    auto r=p.GetLastReport();Require(r.sourceGeneration==9&&r.frameSequence==1&&r.timestampPts==340000,"new source tags current output");Require(out->width==96&&out->height==160&&out->pts==340000,"portrait output/current PTS");Require(Bytes(copy)==expected,"retained frame survives source and geometry changes");av_frame_free(&copy);}}
static void SanitizationAndSnapshots(){DesktopStudioPipeline p(false);auto s=Custom();s.keyLight.posX=std::numeric_limits<float>::quiet_NaN();s.keyLight.posY=7;s.keyLight.posZ=-2;s.keyLight.colorTemperature=std::numeric_limits<float>::infinity();s.keyLight.tint=std::numeric_limits<float>::quiet_NaN();s.fillLight.intensity=9;s.fillLight.colorTemperature=-20;s.fillLight.colorB=std::numeric_limits<float>::quiet_NaN();p.ConfigureStudio(s);auto a=p.GetStudioSettings();
    Require(a.keyLight.posX==-.45f&&a.keyLight.posY==1&&a.keyLight.posZ==.1f&&a.keyLight.colorTemperature==3200&&a.keyLight.tint==0,"finite safe key bounds");Require(a.fillLight.intensity==2&&a.fillLight.colorTemperature==1500&&std::isfinite(a.fillLight.colorB),"finite safe fill bounds");
    Frame f;p.ConfigureStudio(Custom());auto first=Bytes(p.Process(f.f,7));auto b=Custom();b.keyLight.colorTemperature=12000;b.fillLight.enabled=false;b.keyLight.posX=.6f;p.ConfigureStudio(b);auto second=Bytes(p.Process(f.f,7));
    std::atomic<bool> stop{false};std::thread controls([&]{while(!stop.load()){p.ConfigureStudio(Custom());p.ConfigureStudio(b);}});bool valid=true;
    for(int i=0;i<40;++i){auto got=Bytes(p.Process(f.f,7));if(got!=first&&got!=second)valid=false;}stop=true;controls.join();Require(valid,"GUI settings captured atomically per frame");}
static void GpuFallbackAndConsumers(){DesktopStudioPipeline cpu(false),gpu(true);Frame f;auto s=Custom();cpu.ConfigureStudio(s);gpu.ConfigureStudio(s);auto expected=Bytes(cpu.Process(f.f,7));auto*out=gpu.Process(f.f,7);Applied(gpu,true);Require(Difference(Bytes(out),expected)<=2,"completed hardware two-light parity");
    f.Fill(41,78,120);f.f->pts+=16666;expected=Bytes(cpu.Process(f.f,7));gpu.InjectGpuReadbackFailureForTesting(true);out=gpu.Process(f.f,7);Require(gpu.GetLastReport().gpuFallbackUsed&&gpu.GetLastReport().lightingStatus==S::StageStatus::Applied,"real GPU failure current-frame CPU fallback");Require(Bytes(out)==expected,"fallback current independently processed camera");gpu.InjectGpuReadbackFailureForTesting(false);out=gpu.Process(f.f,7);Applied(gpu,true);
    Video::GuiPreviewScaler preview(160,96);Video::DirectShowScaler virtualCamera(160,96);int w=0,h=0;auto*previewBytes=preview.Process(out,w,h);auto got=Bytes(out);Require(previewBytes&&w==160&&h==96&&!std::memcmp(previewBytes,got.data(),got.size()),"common preview receives custom output");auto*cameraBytes=virtualCamera.Process(out,w,h);Require(cameraBytes&&!std::memcmp(cameraBytes,got.data(),got.size()),"common camera output receives custom output");auto*shot=av_frame_clone(out);Require(shot&&Bytes(shot)==got,"snapshot receives clean custom output");av_frame_free(&shot);}
static void RealLightingOnly(bool gpu){DesktopStudioPipeline p(gpu);Frame f(320,180);auto s=Custom();p.ConfigureStudio(s);p.Process(f.f,7); // Warm shader before any real mask's 150ms lifetime.
    s.lightingSubjectOnly=true;s.background=DesktopBackgroundMode::Off;p.ConfigureStudio(s);bool applied=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(25);
    while(std::chrono::steady_clock::now()<deadline){f.f->pts+=40000;auto*out=p.Process(f.f,7);auto state=p.GetSegmentationState();if(state.phase==P::SegmentationPhase::Failed)throw std::runtime_error(state.message);
        if(p.GetLastReport().lightingStatus==S::StageStatus::Applied){Require(state.currentCompletedFrames>0,"actual model completed in production lighting-only routing");Require(state.activeProvider==(gpu?P::ExecutionProviderType::DirectML:P::ExecutionProviderType::CPU)&&!state.fallbackOccurred,"actual requested real inference backend");Require(p.GetLastReport().backgroundStatus==S::StageStatus::Bypassed,"real lighting-only leaves background off");Require(Bytes(out)!=Bytes(f.f),"real person inference changes current lighting output");Applied(p,gpu);applied=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    Require(applied,"real lighting with background off applied in bounded window");std::cout<<"REAL_LIGHTING_PROVIDER "<<(gpu?"DirectML":"CPU")<<" completed="<<p.GetSegmentationState().currentCompletedFrames<<'\n';
    s.lightingSubjectOnly=false;p.ConfigureStudio(s);p.Process(f.f,7);Require(p.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"whole-frame toggle stops real intake");
    s.lightingSubjectOnly=true;p.ConfigureStudio(s);DesktopColorSettings c;c.bypassed=true;p.Configure(c);p.Process(f.f,7);Require(p.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"master bypass disables subject intake");}
int main(){try{
#define RUN(name,call) ++groups;std::cout<<"GROUP " name<<std::endl;call;std::cout<<"PASS " name<<std::endl;
    RUN("independent custom two-light spatial and Kelvin pixel oracle CPU/hardware",OraclePixels());
    RUN("zero disabled custom lights and zero strength avoid analysis and transfers",InactiveCustomLights());
    RUN("independent key/fill position intensity enabled and Kelvin controls",IndependentLights());
    RUN("subject foreground protected background zero mask and retention",SubjectProtection());
    RUN("pending future stale geometry source masks and independent look",MissingStaleAndOtherEffects());
    RUN("master bypass custom restore current geometry source and output metadata",BypassAndSourceMetadata());
    RUN("all finite control bounds and atomic concurrent snapshots",SanitizationAndSnapshots());
    RUN("actual hardware fallback recovery common preview virtual camera snapshot",GpuFallbackAndConsumers());
    RUN("actual pinned CPU person mask drives lighting with background off",RealLightingOnly(false));
    RUN("actual pinned DirectML person mask drives lighting with background off",RealLightingOnly(true));
    std::cout<<"CUSTOM_LIGHTING_PASS groups="<<groups<<" assertions="<<assertions<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"CUSTOM_LIGHTING_FAIL groups="<<groups<<" assertions="<<assertions<<" reason="<<e.what()<<std::endl;return 1;}}
