#define wmain ExistingAccessoryFixtureEntryPoint
#include "../facegeometry/face_accessory_tests.cpp"
#undef wmain
#include "effects/studiostack/gpu/d3d11_face_accessories.h"
#include <fstream>
namespace {
std::filesystem::path TestRoot() {
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr,executable,32768)>0,"test resource root");
    auto root=std::filesystem::path(executable).parent_path()/L"empty-filter-assets";
    std::filesystem::create_directories(root);
    Require(R::SetFaceFilterAssetRootForTesting(root.wstring()),"set real empty asset directory");
    return root;
}
Presets::StudioPreset FilterBorder(int style=1) {
    auto p=RealPreset(false,false);p.faceFilterStyle=style;p.overlays.studioFrame.enabled=true;
    return p;
}
void FailureReport(const S::StackExecutionReport& r) {
    Require(r.overlayStatus==S::StageStatus::Failed&&r.overlayLayers.faceFilter==3,
        "missing model remains failed, never tracking or not-ready");
    Require(r.overlayLayers.frame==0&&r.overlayLayers.glasses==1&&r.overlayLayers.hat==1,
        "independent border applied while facial accessory choices remain bypassed");
    Require(!r.errorDescription.empty(),"model failure message preserved across border composition");
}
void DirectCpu() {
    auto root=TestRoot();auto input=MakeFrame();auto geometry=RealGeometry();const auto original=Bytes(input.get());
    S::StudioStack oracle;auto plain=FilterBorder(0);Configure(oracle,plain);S::StackExecutionReport r;
    const auto expected=Bytes(oracle.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,geometry));
    Require(expected!=original&&r.overlayLayers.frame==0,"actual independently configured frame changes pixels");
    for(int style=1;style<=3;++style) {
        S::StudioStack stack;Configure(stack,FilterBorder(style));
        auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,geometry);
        FailureReport(r);Require(out&&Bytes(out)==expected,"missing model preserves exact CPU decorative border pixels");
        Metadata(out,input.get());Require(Bytes(input.get())==original,"caller frame unchanged");
        Frame retained(av_frame_clone(out));Require(bool(retained),"retained output clone");
        out=stack.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,std::make_shared<A::AnalysisBundle>());
        Require(out&&Bytes(out)==expected&&r.overlayLayers.faceFilter==2&&r.overlayLayers.frame==0&&
            r.overlayStatus==S::StageStatus::Applied,"actual missing-face policy remains independent of model failure");
        Require(Bytes(retained.get())==expected,"retained fanout image survives state change");
    }
    // The real loader rejects malformed bytes, not only a non-existent filename.
    {std::ofstream file(root/L"aurora-fox-mask.pfmodel",std::ios::binary);file<<"PFM1 truncated invalid payload";}
    Require(R::SetFaceFilterAssetRootForTesting(root.wstring()),"clear loader cache for malformed file");
    S::StudioStack malformed;Configure(malformed,FilterBorder());
    auto* out=malformed.ProcessFrame(input.get(),Seq,Pts,&r,Gen,nullptr,nullptr,geometry);
    FailureReport(r);Require(out&&Bytes(out)==expected,"malformed real file preserves independent border");
    // SetPreset protects the production stack from malformed model fit settings.
    auto invalid=FilterBorder();invalid.faceFilterScale=4;malformed.SetPreset(invalid);
    Require(malformed.GetPreset().faceFilterScale==1,"invalid model fit cannot enter configured production stack");
    // Invalid planes do not get mistaken for recoverable missing assets.
    R::FaceAccessory3DRenderer renderer;Overlays::OverlayOutcome status;Overlays::LayerReport layers;
    AVFrame shortPlane=*input;AVBufferRef owner=*input->buf[0];owner.size=1;shortPlane.buf[0]=&owner;
    Require(renderer.ProcessFrame(&shortPlane,&geometry->faceLandmarks,false,false,1,&status,&layers,0,0,1,0,1)==nullptr&&
        status==Overlays::OverlayOutcome::InvalidInput&&layers.faceFilter==3,"bad owned plane remains hard failure with no input recovery");
}
void MissingAssetGpu(bool hardware) {
    auto root=TestRoot();Gpu::D3D11Context context;
    Require(context.Initialize(hardware)&&context.IsHardwareAdapter()==hardware,"actual requested hardware/WARP device");
    std::cout<<"ADAPTER "<<context.GetAdapterName()<<std::endl;
    auto input=MakeFrame();auto geometry=RealGeometry(.2f);const auto original=Bytes(input.get());
    S::StudioStack oracle;Configure(oracle,FilterBorder(0));S::StackExecutionReport cr,gr;
    const auto expected=Bytes(oracle.ProcessFrame(input.get(),Seq,Pts,&cr,Gen,nullptr,nullptr,geometry));
    for(int style=1;style<=3;++style) {
        S::StudioStack stack;Configure(stack,FilterBorder(style));
        stack.SetExecutionBackend(S::ExecutionBackend::GpuWithCpuFallback);
        Require(stack.InitializeGpu(context.GetDevice(),context.GetContext()),"real GPU stack initialize");
        auto* out=stack.ProcessFrame(input.get(),Seq,Pts,&gr,Gen,nullptr,nullptr,geometry);
        FailureReport(gr);Require(out&&!gr.gpuFallbackUsed&&gr.imageUploadCount==1&&gr.imageReadbackCount==1,
            "missing asset is rejected before raster writes and independent border completes on actual GPU");
        const auto actual=Bytes(out);int maximum=0;
        for(size_t i=0;i<actual.size();++i)maximum=std::max(maximum,std::abs(int(actual[i])-int(expected[i])));
        Require(maximum<=1,"hardware/WARP border agrees with independent CPU oracle within one code");
        Metadata(out,input.get());Require(Bytes(input.get())==original,"GPU path preserves input");
        out=stack.ProcessFrame(input.get(),Seq,Pts,&gr,Gen,nullptr,nullptr,std::make_shared<A::AnalysisBundle>());
        Require(out&&!gr.gpuFallbackUsed&&gr.overlayLayers.faceFilter==2&&gr.overlayLayers.frame==0&&
            gr.overlayStatus==S::StageStatus::Applied,"missing face remains not-ready while border renders on GPU");
        for(auto fault:{S::GpuFailureTarget::OverlayStage,S::GpuFailureTarget::Readback}) {
            stack.SetGpuFailureTarget(fault);out=stack.ProcessFrame(input.get(),Seq,Pts,&gr,Gen,nullptr,nullptr,geometry);
            FailureReport(gr);Require(out&&gr.gpuFallbackUsed&&Bytes(out)==expected,
                "ordinary overlay/readback faults retain actual same-frame CPU fallback");
        }
    }
    // Exercise the new discriminator on the real GPU renderer with valid resources.
    D3D11_TEXTURE2D_DESC desc{};desc.Width=160;desc.Height=120;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> inputTexture,outputTexture;ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11UnorderedAccessView> target;
    Require(SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc,nullptr,&inputTexture))&&
        SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc,nullptr,&outputTexture))&&
        SUCCEEDED(context.GetDevice()->CreateShaderResourceView(inputTexture.Get(),nullptr,&view))&&
        SUCCEEDED(context.GetDevice()->CreateUnorderedAccessView(outputTexture.Get(),nullptr,&target)),"actual bounded GPU views");
    S::Gpu::D3D11FaceAccessories renderer(context.GetDevice(),context.GetContext());Overlays::LayerReport layers;std::string error;
    auto run=[&](ID3D11ShaderResourceView* source,const A::FaceLandmarkBundle* face,int style,float size=1) {
        return renderer.Process(source,target.Get(),160,120,face,false,false,1,&layers,&error,0,0,1,0,style,size);
    };
    auto status=run(view.Get(),&geometry->faceLandmarks,1);
    Require(status==S::Gpu::GpuOverlayOutcome::AllocationFailed&&layers.faceFilter==3&&
        renderer.GetLastFilterBuildFailure()==R::FilterBuildFailure::AssetUnavailable,"missing asset explicitly classified before real shader stage");
    status=run(view.Get(),&geometry->faceLandmarks,1,4);
    Require(status==S::Gpu::GpuOverlayOutcome::InvalidInput&&layers.faceFilter==3&&
        renderer.GetLastFilterBuildFailure()==R::FilterBuildFailure::InvalidRequest,"invalid fit explicitly classified as model-build failure");
    status=run(nullptr,&geometry->faceLandmarks,1);
    Require(status==S::Gpu::GpuOverlayOutcome::AllocationFailed&&layers.faceFilter==3&&
        renderer.GetLastFilterBuildFailure()==R::FilterBuildFailure::None,"ordinary invalid target resets model-failure classification");
    status=run(view.Get(),nullptr,1);
    Require(status==S::Gpu::GpuOverlayOutcome::NotReady&&layers.faceFilter==2&&
        renderer.GetLastFilterBuildFailure()==R::FilterBuildFailure::None,"no-face classification distinct and no stale model-error reuse");
    status=run(view.Get(),nullptr,0);
    Require(status==S::Gpu::GpuOverlayOutcome::Bypass&&renderer.GetLastFilterBuildFailure()==R::FilterBuildFailure::None,
        "bypass clears model-failure classification");
}
}
int wmain() {
    try {
        DirectCpu();++groups;std::cout<<"PASS real missing/malformed model CPU border and ownership\n";
        MissingAssetGpu(true);++groups;std::cout<<"PASS real hardware missing model border and ordinary-fault fallback\n";
        MissingAssetGpu(false);++groups;std::cout<<"PASS real WARP missing model border and ordinary-fault fallback\n";
        Require(R::SetFaceFilterAssetRootForTesting(L""),"restore executable-relative model root");
        std::cout<<"MISSING_FILTER_BORDER_PASS groups="<<groups<<" assertions="<<checks<<std::endl;return 0;
    }catch(const std::exception& e){std::cerr<<"MISSING_FILTER_BORDER_FAIL groups="<<groups<<" assertions="<<checks<<" reason="<<e.what()<<std::endl;return 1;}
}
