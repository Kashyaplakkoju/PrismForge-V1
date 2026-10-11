// Offline end-to-end test: real pinned FaceLandmarker inference and canonical
// face geometry feed the production CPU and D3D11 lighting/shadow renderers.
// Renderer pixels are synthetic neutral gray to expose shading independently
// of the screenshot's baked illumination. The screenshot is decoded in memory;
// no facial image or model output image is written or transmitted.
// These are approximate mesh/depth shadows, not intrinsic image reconstruction.
#include "providers/face_landmarker_runtime.h"
#include "providers/face_geometry.h"
#include "effects/lighting/studiolighting.h"
#include "effects/lighting/gpu/d3d11_studiolighting.h"
#include "effects/relighting/approximate_shadows.h"
#include "effects/relighting/gpu/d3d11_approximate_shadows.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using Microsoft::WRL::ComPtr;
namespace P = PrismForge::Providers;
namespace L = PrismForge::Effects::Lighting;
namespace R = PrismForge::Effects::Relighting;
namespace A = PrismForge::Effects::AnalysisBridge;
static int groups=0, assertions=0;
static void Check(bool condition, const char* description) {
    ++assertions; if(!condition) throw std::runtime_error(description);
}
static void Hr(HRESULT result,const char* description) { Check(SUCCEEDED(result),description); }
struct ComSession {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ComSession(){Hr(result,"initialize local WIC COM session");}
    ~ComSession(){if(SUCCEEDED(result))CoUninitialize();}
};
struct Image { int width=0,height=0; std::vector<uint8_t> rgb; };
static Image DecodeFixture(const wchar_t* path) {
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC imaging factory");
    ComPtr<IWICBitmapDecoder> decoder;
    Hr(factory->CreateDecoderFromFilename(path,nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"read local supplied screenshot");
    ComPtr<IWICBitmapFrameDecode> frame; Hr(decoder->GetFrame(0,&frame),"WIC screenshot frame");
    UINT width=0,height=0; Hr(frame->GetSize(&width,&height),"WIC dimensions");
    Check(width>0&&height>0&&width<=8192&&height<=8192,"bounded supplied screenshot");
    ComPtr<IWICBitmapSource> source; Hr(frame.As(&source),"WIC source interface");
    // Only this exact known reference image uses a crop. Others remain whole.
    if((width==1376||width==1377)&&(height==915||height==917)) {
        ComPtr<IWICBitmapClipper> clipper; Hr(factory->CreateBitmapClipper(&clipper),"local preview clipper");
        WICRect region{126,287,792,443}; Hr(clipper->Initialize(source.Get(),&region),"clip known camera preview in memory");
        Hr(clipper.As(&source),"cropped WIC source"); width=792; height=443;
    }
    ComPtr<IWICFormatConverter> converter; Hr(factory->CreateFormatConverter(&converter),"WIC RGB converter");
    Hr(converter->Initialize(source.Get(),GUID_WICPixelFormat24bppRGB,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"convert reference to RGB");
    Image image; image.width=static_cast<int>(width);image.height=static_cast<int>(height);
    const uint64_t count=uint64_t(width)*height*3;
    Check(count<=32*1024*1024&&count<=UINT_MAX,"bounded WIC owned RGB bytes");
    image.rgb.resize(static_cast<size_t>(count));
    Hr(converter->CopyPixels(nullptr,width*3,static_cast<UINT>(count),image.rgb.data()),"copy owned RGB fixture");
    std::cout<<"fixture_in_memory="<<width<<"x"<<height<<"\n";
    return image;
}
struct Frame {
    AVFrame* frame=av_frame_alloc();
    Frame(int width,int height) {
        Check(frame!=nullptr,"allocate deterministic renderer input");
        frame->width=width;frame->height=height;frame->format=AV_PIX_FMT_BGR24;
        frame->pts=1000000;frame->time_base={1,1000000};frame->color_range=AVCOL_RANGE_JPEG;
        Check(av_frame_get_buffer(frame,64)>=0,"allocate neutral-gray pixel buffer");
        for(int y=0;y<height;++y)std::memset(frame->data[0]+y*frame->linesize[0],92,static_cast<size_t>(width)*3);
    }
    ~Frame(){av_frame_free(&frame);}
};
static std::vector<uint8_t> Bytes(const AVFrame* frame) {
    Check(frame&&frame->format==AV_PIX_FMT_BGR24,"renderer returns BGR24");
    std::vector<uint8_t> bytes(static_cast<size_t>(frame->width)*frame->height*3);
    for(int y=0;y<frame->height;++y)std::memcpy(bytes.data()+static_cast<size_t>(y)*frame->width*3,
        frame->data[0]+y*frame->linesize[0],static_cast<size_t>(frame->width)*3);
    return bytes;
}
static L::AlphaMask Mask(const P::FaceGeometry& geometry) {
    return {geometry.coverage.data.data(),geometry.coverage.width,geometry.coverage.height,geometry.coverage.strideFloats};
}
static L::NormalField Normals(const P::FaceGeometry& geometry) {
    return {geometry.normals.data.data(),geometry.normals.width,geometry.normals.height,geometry.normals.strideFloats};
}
static L::DepthField Depth(const P::FaceGeometry& geometry) {
    return {geometry.depth.data.data(),geometry.depth.width,geometry.depth.height,geometry.depth.strideFloats};
}
static std::vector<uint8_t> Render(L::StudioLighting& cpu,const Frame& input,const P::FaceGeometry& geometry,const L::LightingConfig& config) {
    auto mask=Mask(geometry);auto normals=Normals(geometry);auto depth=Depth(geometry);L::LightingOutcome status{};
    auto* frame=cpu.ProcessFrame(input.frame,&mask,&normals,&depth,config,status);
    Check(status==L::LightingOutcome::Success,"production CPU geometry lighting actually applied");
    return Bytes(frame);
}
static void OutsideExact(const std::vector<uint8_t>& bytes,const P::FaceGeometry& geometry) {
    size_t unsupported=0;
    for(size_t i=0;i<geometry.coverage.data.size();++i)if(geometry.coverage.data[i]==0) {
        ++unsupported;Check(bytes[i*3]==92&&bytes[i*3+1]==92&&bytes[i*3+2]==92,"unsupported geometry is preserved bit exactly");
    }
    Check(unsupported>0,"fixture has unsupported background");
}
static double ToLinear(double value) {return value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4);}
static int ToByte(double value) {
    value=std::clamp(value,0.,1.);
    const double s=value<=.0031308?value*12.92:1.055*std::pow(value,1./2.4)-.055;
    return std::clamp(static_cast<int>(s*255+.5),0,255);
}
// Independent double-precision Lambert evaluator. It does not call renderer
// coordinate, color conversion, diffuse, or shadow helper functions.
static double Diffuse(const L::StudioLight& light,const P::FaceGeometry& geometry,int x,int y) {
    if(!light.enabled||light.intensity<=0)return 0;
    const size_t at=(static_cast<size_t>(y)*geometry.normals.strideFloats)+x*3;
    double nx=geometry.normals.data[at],ny=geometry.normals.data[at+1],nz=geometry.normals.data[at+2];
    const double norm=std::sqrt(nx*nx+ny*ny+nz*nz);Check(norm>0,"oracle gets supported finite normal");
    nx/=norm;ny/=norm;nz/=norm;
    const double u=2.*(x+.5)/geometry.coverage.width-1.,v=2.*(y+.5)/geometry.coverage.height-1.;
    const double dx=(light.posX-u)*geometry.coverage.width/geometry.coverage.height;
    const double dy=light.posY-v,dz=light.posZ,dist=std::sqrt(dx*dx+dy*dy+dz*dz);
    return std::max(0.,(nx*dx+ny*dy+nz*dz)/dist)/(1.+.4*std::max(0.,dist-dz))*light.intensity;
}
static void LambertOracle(const std::vector<uint8_t>& bytes,const P::FaceGeometry& geometry,const L::LightingConfig& config) {
    Check(!config.shadowConfig.enabled&&config.light1.colorTemperature==0&&config.light2.colorTemperature==0,"independent oracle explicit linear colors without shadows");
    const std::array<double,3> one={config.light1.colorB,config.light1.colorG,config.light1.colorR};
    const std::array<double,3> two={config.light2.colorB,config.light2.colorG,config.light2.colorR};
    const double source=ToLinear(92./255.);int maximum=0;size_t changed=0;
    for(int y=0;y<geometry.coverage.height;++y)for(int x=0;x<geometry.coverage.width;++x) {
        const size_t at=static_cast<size_t>(y)*geometry.coverage.width+x;if(!geometry.coverage.data[at])continue;
        const double first=Diffuse(config.light1,geometry,x,y)*config.globalStrength;
        const double second=Diffuse(config.light2,geometry,x,y)*config.globalStrength;
        for(size_t c=0;c<3;++c) {
            const int expected=ToByte(source*(config.ambientIntensity+config.fillIntensity+one[c]*first+two[c]*second));
            maximum=std::max(maximum,std::abs(expected-static_cast<int>(bytes[at*3+c])));
            if(bytes[at*3+c]!=92)++changed;
        }
    }
    Check(maximum<=1,"independent double precision Lambert pixel oracle within one byte");
    Check(changed>0,"supported face is materially relit");
    std::cout<<"lambert_oracle_max_byte_error="<<maximum<<"\n";
}
static L::LightingConfig Config(float x,float y) {
    auto config=L::LightingConfig::DefaultTwoLight();config.lightingMode=L::LightingMode::NormalGuided;
    config.ambientIntensity=.55f;config.fillIntensity=.05f;config.globalStrength=.8f;
    config.light1.posX=x;config.light1.posY=y;config.light1.posZ=.10f;config.light1.intensity=.9f;
    config.light1.colorR=1;config.light1.colorG=.8f;config.light1.colorB=.65f;
    config.light2.posX=-x;config.light2.posY=-y;config.light2.posZ=.35f;config.light2.intensity=.25f;
    config.light2.colorR=.65f;config.light2.colorG=.8f;config.light2.colorB=1;
    config.shadowConfig.enabled=false;config.shadowConfig.maxSteps=32;config.shadowConfig.maxDistance=.5f;
    config.shadowConfig.rayElevation=.01f;config.shadowConfig.shadowIntensity=.8f;config.shadowConfig.shadowSoftness=.1f;
    return config;
}
static int ByteDifference(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b) {
    Check(a.size()==b.size(),"compare equally sized actual outputs");int maximum=0;
    for(size_t i=0;i<a.size();++i)maximum=std::max(maximum,std::abs(static_cast<int>(a[i])-static_cast<int>(b[i])));
    return maximum;
}
static void GpuParity(ID3D11Device* device,ID3D11DeviceContext* context,const Frame& input,
    const P::FaceGeometry& geometry,const L::LightingConfig& config,const std::vector<uint8_t>& expected) {
    auto mask=Mask(geometry);auto normals=Normals(geometry);auto depth=Depth(geometry);L::LightingOutcome status{};
    L::Gpu::D3D11StudioLightingAdapter adapter(device,context);L::Gpu::D3D11StudioLightingAdapter::TimingStats timing;
    auto* result=adapter.ProcessFrame(input.frame,&mask,&normals,&depth,config,status,&timing);
    Check(status==L::LightingOutcome::Success&&!timing.fallbackUsed,"real D3D11 lighting pass without CPU fallback");
    auto actual=Bytes(result);const int maximum=ByteDifference(actual,expected);
    Check(maximum<=2,"actual GPU face-light output matches CPU within two bytes");
    OutsideExact(actual,geometry);std::cout<<"gpu_lighting_max_byte_error="<<maximum<<" shadows="<<config.shadowConfig.enabled<<"\n";
}
int wmain(int argc,wchar_t** argv) {
    try {
        Check(argc==5,"usage: face_lighting_tests runtimeDLL task canonicalOBJ fixturePNG");ComSession com;
        Image image=DecodeFixture(argv[4]);P::FaceLandmarkerRuntime runtime;std::string error;
        Check(runtime.Initialize(argv[1],argv[2],error),error.c_str());P::FaceObservation observation;
        const P::FaceFrameTags tags{19,1,1000000};
        Check(runtime.DetectRGB(image.rgb.data(),image.rgb.size(),image.width,image.height,image.width*3,tags,observation,error),error.c_str());
        Check(observation.detected&&observation.vertices.size()==478,"actual native fixture inference detects all 478 landmarks");
        P::FaceTopology topology;Check(P::LoadFaceTopology(argv[3],topology,error),error.c_str());
        const int width=512,height=std::max(1,static_cast<int>(std::lround(512.*image.height/image.width)));
        P::FaceGeometry geometry;Check(P::BuildFaceGeometry(observation,topology,width,height,geometry,error),error.c_str());
        Check(geometry.tags.generation==tags.generation&&geometry.tags.sequence==tags.sequence&&geometry.tags.microPts==tags.microPts,"source geometry tags remain exact");
        size_t supported=0;
        for(size_t i=0;i<geometry.coverage.data.size();++i) {
            const float coverage=geometry.coverage.data[i];Check(coverage==0||coverage==1,"visible surface support is explicit binary coverage");
            if(coverage) {
                ++supported;const auto* n=&geometry.normals.data[i*3];const float length=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
                Check(std::isfinite(length)&&std::abs(length-1)<1e-4f&&n[2]>0,"supported geometry has finite unit camera-facing normals");
                Check(std::isfinite(geometry.depth.data[i])&&geometry.depth.data[i]>=0&&geometry.depth.data[i]<=1,"supported relative depth is finite and bounded");
            } else Check(geometry.depth.data[i]==A::DepthBuffer::kDefaultInvalidSentinel,"unsupported geometry retains invalid depth sentinel");
        }
        Check(supported>50,"actual canonical face surface has substantive coverage");
        ++groups;std::cout<<"PASS actual_native_face_geometry supported="<<supported<<" raster="<<width<<"x"<<height<<"\n";
        Frame input(width,height);L::StudioLighting cpu;
        auto left=Config(-.9f,0);auto right=Config(.9f,0);
        auto leftPixels=Render(cpu,input,geometry,left);auto rightPixels=Render(cpu,input,geometry,right);
        OutsideExact(leftPixels,geometry);OutsideExact(rightPixels,geometry);
        LambertOracle(leftPixels,geometry,left);LambertOracle(rightPixels,geometry,right);
        Check(ByteDifference(leftPixels,rightPixels)>=8,"moving real-face key light changes directional shading materially");
        auto flatNormals=geometry;
        for(size_t i=0;i<geometry.coverage.data.size();++i)if(geometry.coverage.data[i]) {
            flatNormals.normals.data[i*3]=flatNormals.normals.data[i*3+1]=0;flatNormals.normals.data[i*3+2]=1;
        }
        Check(ByteDifference(leftPixels,Render(cpu,input,flatNormals,left))>=8,"actual curved face normals are used rather than fabricated flat normals");
        ++groups;std::cout<<"PASS CPU_two_light_direction_and_independent_oracle\n";
        auto dropout=geometry;size_t hole=geometry.coverage.data.size();
        for(size_t i=0;i<geometry.coverage.data.size();++i)if(geometry.coverage.data[i]) {hole=i;break;}
        Check(hole<geometry.coverage.data.size(),"choose real supported surface point for explicit support dropout");
        dropout.coverage.data[hole]=dropout.depth.confidence[hole]=0;dropout.depth.data[hole]=-1;
        dropout.normals.data[hole*3]=dropout.normals.data[hole*3+1]=dropout.normals.data[hole*3+2]=0;
        auto holePixels=Render(cpu,input,dropout,left);OutsideExact(holePixels,dropout);
        Check(holePixels[hole*3]==92&&holePixels[hole*3+1]==92&&holePixels[hole*3+2]==92,"explicit geometry hole is never invented or shaded");
        ++groups;std::cout<<"PASS unsupported_and_explicit_geometry_dropout_preserved\n";
        R::ApproximateShadows shadowCpu;R::ShadowMapBuffer selectedShadows;L::LightingConfig selectedConfig;
        size_t maximumDepthOcclusion=0, maximumVisibleDepthOcclusion=0;int renderedShadowMax=0;
        for(auto position:{std::array<float,2>{-1,0},std::array<float,2>{1,0},std::array<float,2>{0,-1},std::array<float,2>{0,1},
                          std::array<float,2>{-1,-1},std::array<float,2>{1,-1},std::array<float,2>{-1,1},std::array<float,2>{1,1}}) {
            auto config=Config(position[0],position[1]);config.shadowConfig.enabled=true;
            auto mask=Mask(geometry);R::ShadowMapBuffer actual;
            Check(shadowCpu.ComputeShadows(geometry.depth,&geometry.normals,&mask,config,actual),"actual geometry CPU depth-shadow pass");
            auto flatDepth=geometry.depth;for(size_t i=0;i<flatDepth.data.size();++i)if(geometry.coverage.data[i])flatDepth.data[i]=.5f;
            R::ShadowMapBuffer flat;Check(shadowCpu.ComputeShadows(flatDepth,&geometry.normals,&mask,config,flat),"flat-depth comparison preserves same normals");
            size_t depthOcclusion=0;
            for(size_t i=0;i<actual.data.size();++i) {
                Check(std::isfinite(actual.data[i])&&actual.data[i]>=0&&actual.data[i]<=1,"real-face shadow visibility is finite and bounded");
                if(!geometry.coverage.data[i/2])Check(actual.data[i]==1,"unsupported shadow visibility stays neutral");
                if(actual.data[i]+1e-4f<flat.data[i])++depthOcclusion;
            }
            auto withShadow=Render(cpu,input,geometry,config);OutsideExact(withShadow,geometry);
            auto without=config;without.shadowConfig.enabled=false;
            const int changed=ByteDifference(withShadow,Render(cpu,input,geometry,without));
            maximumDepthOcclusion=std::max(maximumDepthOcclusion,depthOcclusion);
            if(changed>0&&depthOcclusion>maximumVisibleDepthOcclusion) {maximumVisibleDepthOcclusion=depthOcclusion;selectedShadows=std::move(actual);selectedConfig=config;renderedShadowMax=changed;}
        }
        Check(maximumDepthOcclusion>0,"actual relative facial depth produces occlusion beyond backface darkening");
        Check(renderedShadowMax>0,"actual face-depth shadow changes rendered supported pixels");
        ++groups;std::cout<<"PASS CPU_approximate_mesh_depth_shadows depth_occluded_channels="<<maximumDepthOcclusion<<" rendered_max_byte_change="<<renderedShadowMax<<"\n";
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        HRESULT create=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,D3D11_SDK_VERSION,&device,&level,&context);
        if(create==E_INVALIDARG)create=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels+1,1,D3D11_SDK_VERSION,&device,&level,&context);
        if(FAILED(create)) {
            std::cout<<"SKIP hardware_D3D11_unavailable HRESULT="<<static_cast<unsigned long>(create)<<"; CPU result only\n";
        } else {
            ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
            Hr(device.As(&dxgi),"actual hardware DXGI device");Hr(dxgi->GetAdapter(&adapter),"actual hardware adapter");Hr(adapter->GetDesc(&desc),"hardware adapter description");
            std::wcout<<L"hardware_adapter="<<desc.Description<<L"\n";
            GpuParity(device.Get(),context.Get(),input,geometry,left,leftPixels);
            GpuParity(device.Get(),context.Get(),input,geometry,right,rightPixels);
            GpuParity(device.Get(),context.Get(),input,dropout,left,holePixels);
            auto cpuShadow=Render(cpu,input,geometry,selectedConfig);
            GpuParity(device.Get(),context.Get(),input,geometry,selectedConfig,cpuShadow);
            R::Gpu::D3D11ApproximateShadows shadowGpu(device.Get(),context.Get());R::ShadowMapBuffer gpuShadows;auto mask=Mask(geometry);
            Check(shadowGpu.UploadDepthAndCompute(geometry.depth,&geometry.normals,&mask,selectedConfig,gpuShadows),"real D3D11 depth shadows read back successfully");
            Check(gpuShadows.data.size()==selectedShadows.data.size(),"GPU shadow data matches CPU geometry dimensions");float maximum=0;
            for(size_t i=0;i<gpuShadows.data.size();++i) {
                Check(std::isfinite(gpuShadows.data[i]),"GPU shadow visibility is finite");
                maximum=std::max(maximum,std::abs(gpuShadows.data[i]-selectedShadows.data[i]));
                if(!geometry.coverage.data[i/2])Check(gpuShadows.data[i]==1,"GPU unsupported visibility bit exactly neutral");
            }
            Check(maximum<=1e-4f,"actual GPU shadow values match CPU within 0.0001");
            ++groups;std::cout<<"PASS hardware_GPU_lighting_and_depth_shadow_parity shadow_max_error="<<maximum<<"\n";
        }
        runtime.Stop();
        std::cout<<"FACE_LIGHTING_RESULT groups="<<groups<<" assertions="<<assertions<<" failures=0; offline_fixture_not_live_performance\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FACE_LIGHTING_RESULT failures=1 assertions="<<assertions<<" error="<<e.what()<<"\n";return 1;
    }
}
