#include "providers/face_landmarker_runtime.h"
#include "providers/face_geometry.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace P=PrismForge::Providers;
namespace fs=std::filesystem;
struct Suite {
    int checks=0,groups=0,failures=0;
    void Check(bool value,const std::string& message){++checks;if(!value){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
    void Require(bool value,const std::string& message){Check(value,message);if(!value)throw std::runtime_error(message);}
    template<class Function> void Group(const char* name,Function fn){++groups;std::cout<<"GROUP "<<name<<'\n';try{fn();}catch(const std::exception& e){std::cerr<<"GROUP STOP: "<<e.what()<<'\n';}}
};
struct RgbImage {int width=0,height=0;std::vector<uint8_t> bytes;};
RgbImage Decode(const wchar_t* filename) {
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> convert;ComPtr<IWICBitmapClipper> clip;
    auto checked=[](HRESULT hr){if(FAILED(hr))throw std::runtime_error("WIC fixture decode failed");};
    checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    checked(factory->CreateDecoderFromFilename(filename,nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder));
    checked(decoder->GetFrame(0,&frame));UINT width=0,height=0;checked(frame->GetSize(&width,&height));
    if(!width||!height||width>8192||height>8192||uint64_t(width)*height*3>32ULL*1024*1024)throw std::runtime_error("Fixture dimensions out of bounds");
    ComPtr<IWICBitmapSource> source=frame;
    // This user-supplied screenshot includes app chrome. Analyse only its camera
    // preview in memory; do not save, upload or alter the source image.
    if((width==1376||width==1377)&&(height==915||height==917)) {
        checked(factory->CreateBitmapClipper(&clip));WICRect region{126,287,792,443};
        checked(clip->Initialize(frame.Get(),&region));source=clip;width=792;height=443;
    }
    checked(factory->CreateFormatConverter(&convert));
    checked(convert->Initialize(source.Get(),GUID_WICPixelFormat24bppRGB,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    RgbImage result;result.width=int(width);result.height=int(height);result.bytes.resize(size_t(width)*height*3);
    checked(convert->CopyPixels(nullptr,width*3,UINT(result.bytes.size()),result.bytes.data()));return result;
}
int wmain(int argc,wchar_t** argv) {
    if(argc<4){std::cerr<<"Usage: face_runtime_tests runtime.dll face_landmarker.task canonical.obj [fixture.png]\n";return 2;}
    Suite suite;P::FaceLandmarkerRuntime runtime;std::string error;P::FaceObservation observation;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct ComOwner {bool owned;~ComOwner(){if(owned)CoUninitialize();}} comOwner{SUCCEEDED(com)};
    if(FAILED(com)&&com!=RPC_E_CHANGED_MODE){std::cerr<<"COM initialization failed\n";return 2;}
    suite.Group("uninitialized and local-path boundary",[&]{
        std::vector<uint8_t> bytes(192*192*3,0);
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,192*3,{1,1,1000},observation,error)&&!error.empty(),"uninitialized model fails closed");
        suite.Check(!runtime.Initialize(L"relative.dll",argv[2],error)&&!error.empty(),"relative runtime rejected before load");
        suite.Check(!runtime.Initialize(argv[1],L"relative.task",error)&&!error.empty(),"relative model rejected");
        suite.Check(!runtime.Initialize(argv[1],argv[3],error)&&error.find("model hash mismatch")!=std::string::npos,"unpinned model rejected before inference");
        suite.Check(!runtime.Initialize(argv[3],argv[2],error)&&error.find("runtime hash mismatch")!=std::string::npos,"unpinned runtime rejected before load");
    });
    bool ready=false;
    suite.Group("actual pinned native runtime and blank-frame behavior",[&]{
        const auto begin=std::chrono::steady_clock::now();
        ready=runtime.Initialize(argv[1],argv[2],error);suite.Require(ready,"actual reviewed native runtime initializes: "+error);
        std::cout<<"NATIVE_INITIALIZE_MS "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<'\n';
        std::vector<uint8_t> bytes(192*192*3,0);
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size()-1,192,192,192*3,{1,1,1000},observation,error),"short RGB capacity rejected");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,575,{1,1,1000},observation,error),"short stride rejected");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{0,1,1000},observation,error),"zero generation rejected");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{1,0,1000},observation,error),"zero sequence rejected");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{1,1,-1},observation,error),"negative PTS rejected");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),std::numeric_limits<int>::max(),192,576,{1,1,1000},observation,error),"dimensions bounded before multiplication");
        suite.Require(runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{1,1,1000},observation,error),"native blank frame completes: "+error);
        suite.Check(!observation.detected&&observation.vertices.empty()&&!observation.hasBlendshapes&&!observation.hasTransform,"blank frame produces actual no-face output");
        suite.Check(observation.tags.generation==1&&observation.tags.sequence==1&&observation.tags.microPts==1000,"no-face output retains exact tags");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{1,1,1000},observation,error)&&error=="Out-of-order face frame","duplicate sequence rejected");
        suite.Check(!runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{1,2,999},observation,error),"backward PTS rejected");
        suite.Require(runtime.DetectRGB(bytes.data(),bytes.size(),192,192,576,{1,2,1000},observation,error),"equal original PTS accepted with increasing internal VIDEO timestamp");
        suite.Check(observation.tags.microPts==1000,"internal VIDEO timestamp never retags original microPTS");
        const int paddedStride=583;std::vector<uint8_t> padded(size_t(paddedStride)*192,0xD3);
        for(int y=0;y<192;++y)std::fill_n(padded.data()+size_t(y)*paddedStride,576,uint8_t(0));
        suite.Require(runtime.DetectRGB(padded.data(),padded.size(),192,192,paddedStride,{1,3,2000},observation,error),"padded RGB buffer supported");
        suite.Check(!observation.detected,"padding is excluded from native blank image");
    });
    if(ready&&argc>4)suite.Group("real user camera image to478 landmarks and owned mesh geometry",[&]{
        RgbImage image=Decode(argv[4]);runtime.Reset();
        const auto begin=std::chrono::steady_clock::now();
        suite.Require(runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{7,1,100000},observation,error),"actual face image inference: "+error);
        std::cout<<"NATIVE_FIRST_FACE_MS "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<" IMAGE "<<image.width<<'x'<<image.height<<'\n';
        suite.Require(observation.detected,"supplied real camera frame contains a detected face");
        suite.Check(observation.vertices.size()==478&&observation.hasBlendshapes&&observation.hasTransform,"real478 landmarks plus52 expression scores and4x4 transform");
        for(const auto& p:observation.vertices)suite.Check(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z),"real coordinates finite");
        P::FaceTopology topology;P::FaceGeometry geometry;
        suite.Require(P::LoadFaceTopology(argv[3],topology,error),"canonical topology loads: "+error);
        suite.Require(P::BuildFaceGeometry(observation,topology,320,180,geometry,error),"actual face geometry rasterizes: "+error);
        size_t covered=0,holes=0;
        for(size_t i=0;i<geometry.coverage.data.size();++i){
            const float c=geometry.coverage.data[i];const float d=geometry.depth.data[i];
            const float x=geometry.normals.data[i*3],y=geometry.normals.data[i*3+1],z=geometry.normals.data[i*3+2];
            if(c>0){++covered;suite.Check(std::isfinite(d)&&d>=0&&d<=1,"real covered depth bounded");
                suite.Check(std::abs(x*x+y*y+z*z-1)<.001f&&z>=0,"real covered normal normalized and view-facing");}
            else{++holes;suite.Check(d==-1&&x==0&&y==0&&z==0,"uncovered region has no invented geometry");}
        }
        suite.Check(covered>100&&holes>covered,"geometry covers face while retaining outside invalid support");
        suite.Check(geometry.tags.generation==7&&geometry.tags.sequence==1&&geometry.tags.microPts==100000&&geometry.depth.sourceGeneration==7&&geometry.depth.sequenceId==1&&geometry.depth.timestampPts==100000,"source/sequence/PTS exact throughout depth geometry");
        std::cout<<"REAL_GEOMETRY_COVERED "<<covered<<" UNCOVERED "<<holes<<'\n';
        // Moving original timestamps while the same image is repeated measures
        // native offline service cost, not camera throughput or live gesture delay.
        std::vector<double> times;
        for(uint64_t seq=2;seq<=13;++seq){const auto start=std::chrono::steady_clock::now();
            suite.Require(runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{7,seq,int64_t(100000+seq*16667)},observation,error),"native repeated tracked face succeeds");
            suite.Check(observation.detected&&observation.tags.sequence==seq,"tracked output belongs to requested exact frame");
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
        std::sort(times.begin(),times.end());std::cout<<"NATIVE_OFFLINE_TRACKED_P50_MS "<<times[times.size()/2]<<" P95_MS "<<times.back()<<" count="<<times.size()<<" NOT_LIVE_FPS\n";
        suite.Check(!runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{6,20,500000},observation,error),"old source generation rejected");
        suite.Require(runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{8,1,0},observation,error),"new source resets native tracking and accepts its own PTS origin");
        suite.Check(observation.detected&&observation.tags.generation==8&&observation.tags.sequence==1&&observation.tags.microPts==0,"new source exact ownership");
        runtime.Reset();suite.Require(runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{2,1,0},observation,error),"explicit Reset clears generation history");
        runtime.Stop();suite.Check(!runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{2,2,16667},observation,error),"Stop disables inference");
        suite.Require(runtime.Initialize(argv[1],argv[2],error),"native runtime can reinitialize after stop");
        suite.Require(runtime.DetectRGB(image.bytes.data(),image.bytes.size(),image.width,image.height,image.width*3,{9,1,0},observation,error)&&observation.detected,"reinitialized native model detects real face");
    });
    else if(argc<=4)std::cout<<"SKIP real face image: no optional user fixture supplied\n";
    runtime.Stop();
    std::cout<<"NATIVE_RUNTIME checks="<<suite.checks<<" groups="<<suite.groups<<" failures="<<suite.failures<<"; actual native CPU inference, offline image only; no live camera or product FPS claim\n";
    return suite.failures?1:0;
}
