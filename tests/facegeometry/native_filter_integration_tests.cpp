// Reuse the existing native photographic fixtures and metadata/ownership oracles.
// Its standalone baseline entry point remains unchanged in its original source.
#define wmain AccessoryBaselineEntryPoint
#include "face_accessory_tests.cpp"
#undef wmain
#include "video/guipreviewscaler.h"
#include "video/directshowscaler.h"

namespace {
AVFrame* WaitFilter(DesktopStudioPipeline& pipeline,AVFrame* input,uint64_t generation,bool gpu,bool fallback=false) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do {
        input->pts+=16667;
        auto* out=pipeline.Process(input,generation);
        const auto report=pipeline.GetLastReport();const auto state=pipeline.GetFaceAnalysisState();
        if(state.phase==P::FaceAnalysisPhase::Failed)throw std::runtime_error("Native filter analysis: "+state.lastError);
        if(report.overlayLayers.faceFilter==0) {
            Require(out&&out!=input&&state.ready&&state.lastDetected,"native filter applied to real detected face");
            Require(state.sourceGeneration==generation&&state.lastCompletedSequence==report.frameSequence&&state.lastCompletedPts==input->pts,"filter uses this exact native source frame");
            Require(report.overlayLayers.glasses==1&&report.overlayLayers.hat==1,"mask suppresses saved accessory rendering without erasing settings");
            Require(report.gpuFallbackUsed==fallback&&report.imageUploadCount==(gpu?1:0),"truthful native filter GPU/CPU/fallback path");
            Metadata(out,input);return out;
        }
        Require(out==input&&report.overlayLayers.faceFilter==2&&report.imageUploadCount==0,"pending face keeps source moving without stale filter");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("Native face filter did not apply before photographic fixture deadline");
}
void ConsumerParity(const AVFrame* out) {
    const auto expected=Bytes(out);int w=0,h=0;
    Video::GuiPreviewScaler preview(out->width,out->height);Video::DirectShowScaler camera(out->width,out->height);
    auto* pixels=preview.Process(out,w,h);
    Require(pixels&&w==out->width&&h==out->height&&std::memcmp(pixels,expected.data(),expected.size())==0,"actual preview consumer receives model pixels");
    pixels=camera.Process(out,w,h);
    Require(pixels&&w==out->width&&h==out->height&&std::memcmp(pixels,expected.data(),expected.size())==0,"actual virtual-camera scaler receives same model pixels");
    Frame snapshot(av_frame_clone(out));Require(snapshot&&Bytes(snapshot.get())==expected,"snapshot owns same filtered pixels");
}
void ActualFilterCollection(const wchar_t* fixture) {
    auto input=Decode(fixture);auto original=Bytes(input.get());SavePng(input.get(),L"native-filter-source.png");
    DesktopStudioPipeline cpu(false),gpu(true);DesktopStudioSettings settings;
    settings.glasses=settings.cap=true; // Retained accessory choices deliberately conflict with masks.
    Frame retained;std::vector<uint8_t> retainedPixels,previousModelPixels;
    for(int style=1;style<=4;++style) {
        if(style==4){std::string error;settings.faceFilterCustomModel=Overlays::Realistic::LoadFaceFilter(1,error);Require(bool(settings.faceFilterCustomModel),"local immutable model pointer acquired on setup, not frame processing");settings.faceFilterCustomFit.xCm=.2f;settings.faceFilterCustomFit.yCm=.3f;settings.faceFilterCustomFit.zCm=.5f;settings.faceFilterCustomFit.turnDegrees=5.f;}
        settings.faceFilterStyle=style;cpu.ConfigureStudio(settings);gpu.ConfigureStudio(settings);
        auto* cpuOut=WaitFilter(cpu,input.get(),100+style,false);
        auto cpuPixels=Bytes(cpuOut);auto cpuName=L"native-filter-"+std::to_wstring(style)+L"-cpu.png";SavePng(cpuOut,cpuName.c_str());
        auto* gpuOut=WaitFilter(gpu,input.get(),100+style,true);
        ConsumerParity(gpuOut);auto gpuPixels=Bytes(gpuOut);if(style==4){Require(gpuPixels!=previousModelPixels,"local pointer and fit replace the prior bundled model instead of retaining a rejected preset");}previousModelPixels=gpuPixels;auto gpuName=L"native-filter-"+std::to_wstring(style)+L"-gpu.png";SavePng(gpuOut,gpuName.c_str());
        size_t changed=0;double sumDifference=0;
        for(size_t at=0;at<gpuPixels.size();++at) {changed+=gpuPixels[at]!=original[at];sumDifference+=std::abs(int(gpuPixels[at])-int(cpuPixels[at]));}
        Require(changed>200&&changed<gpuPixels.size()/2,"real full-resolution native model pixels, confined to head region");
        Require(sumDifference/gpuPixels.size()<3.0,"real photographic CPU/GPU collection agrees within bounded image-average raster difference");
        Require(Bytes(input.get())==original,"original camera pixels remain immutable");
        Require(gpu.GetStudioSettings().glasses&&gpu.GetStudioSettings().cap,"mask selection retains saved accessories");
        Require(gpu.GetSegmentationState().phase==P::SegmentationPhase::Disabled,"filter-only route does not request a person model");
        if(style==1) {retained.reset(av_frame_clone(gpuOut));Require(bool(retained),"retain common processed fanout frame");retainedPixels=Bytes(retained.get());}
    }
    gpu.InjectGpuReadbackFailureForTesting(true);WaitFilter(gpu,input.get(),109,true,true);gpu.InjectGpuReadbackFailureForTesting(false);
    Require(Bytes(retained.get())==retainedPixels,"retained preview/virtual/snapshot fanout pixels survive filter switches and real fallback");
    settings.studioFrame=true;gpu.ConfigureStudio(settings);
    bool combined=false;auto borderDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do {input->pts+=16667;auto* out=gpu.Process(input.get(),110);auto r=gpu.GetLastReport();
        Require(out&&r.overlayLayers.frame==0,"independent border remains applied while native mask tracks or waits");
        Require(r.overlayLayers.faceFilter==0||r.overlayLayers.faceFilter==2,"border does not misreport missing mask as applied");
        if(r.overlayLayers.faceFilter==0){combined=true;ConsumerParity(out);break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<borderDeadline);
    Require(combined,"model and border compose on real exact-frame face");settings.studioFrame=false;
    settings.touchupStrength=.7f;settings.touchupDetail=.3f;settings.light=DesktopLightMode::Custom;settings.faceLighting=true;settings.strength=.4f;
    gpu.ConfigureStudio(settings);WaitFilter(gpu,input.get(),110,true);
    Require(gpu.GetLastReport().appearanceStatus==S::StageStatus::Applied&&gpu.GetLastReport().lightingStatus==S::StageStatus::Applied,"native model coexists with touch-up and facial lighting");
    auto submitted=gpu.GetFaceAnalysisState().submittedFrames;input->pts+=16667;gpu.Process(input.get(),110);
    Require(gpu.GetFaceAnalysisState().submittedFrames==submitted+1,"all facial effects submit only one analysis per original frame");
    settings.touchupStrength=0;settings.faceLighting=false;settings.light=DesktopLightMode::Off;gpu.ConfigureStudio(settings);
    auto blank=MakeFrame(96,64,true);bool observedNoFace=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do {blank->pts+=16667;auto* out=gpu.Process(blank.get(),111);const auto r=gpu.GetLastReport();const auto state=gpu.GetFaceAnalysisState();
        Require(out==blank.get()&&r.overlayLayers.faceFilter==2&&r.imageUploadCount==0&&r.imageReadbackCount==0,"no-face/source-resolution reset removes filter while keeping original frame");
        if(state.ready&&state.lastCompletedPts==blank->pts&&!state.lastDetected){observedNoFace=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<deadline);
    Require(observedNoFace,"actual native worker completes blank-frame no-face result");
    DesktopColorSettings color;color.bypassed=true;gpu.Configure(color);auto before=gpu.GetFaceAnalysisState().submittedFrames;
    Require(gpu.Process(input.get(),112)==input.get()&&gpu.GetFaceAnalysisState().submittedFrames==before&&gpu.GetStudioSettings().faceFilterStyle==4&&gpu.GetStudioSettings().faceFilterCustomModel==settings.faceFilterCustomModel&&gpu.GetStudioSettings().faceFilterCustomFit.turnDegrees==5.f,"master bypass stops inference and preserves filter selection");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2){std::cerr<<"Usage: native_filter_integration_tests supplied-photo.png\n";return 2;}
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);struct Guard{bool own;~Guard(){if(own)CoUninitialize();}}guard{SUCCEEDED(hr)};
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)return 2;
    try {ActualFilterCollection(argv[1]);std::cout<<"NATIVE_FILTER_INTEGRATION_PASS assertions="<<checks<<"; real native worker, CPU/GPU, photographed fixture only\n";return 0;}
    catch(const std::exception& e){std::cerr<<"NATIVE_FILTER_INTEGRATION_FAIL assertions="<<checks<<" reason="<<e.what()<<"\n";return 1;}
}
