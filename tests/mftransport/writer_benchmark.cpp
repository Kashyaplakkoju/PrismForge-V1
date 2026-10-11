#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MFSharedMemWriter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef MF_REFERENCE_WRITER
#include "benchmark_names.h"
#else
using namespace PrismForge::CameraTransport;
#endif

const WCHAR* MapName() {
#ifdef MF_REFERENCE_WRITER
    return BenchmarkMapName();
#else
    static std::wstring value=L"Local\\PrismForge_WriterBench_"+std::to_wstring(GetCurrentProcessId())+L"_map";
    return value.c_str();
#endif
}
const WCHAR* MutexName() {
#ifdef MF_REFERENCE_WRITER
    return BenchmarkMutexName();
#else
    static std::wstring value=L"Local\\PrismForge_WriterBench_"+std::to_wstring(GetCurrentProcessId())+L"_mutex";
    return value.c_str();
#endif
}
struct Input {
    AVFrame* frame=av_frame_alloc();
    std::vector<BYTE> packed;
    Input(AVPixelFormat format) {
        if(!frame)throw std::bad_alloc();frame->width=1920;frame->height=1080;frame->format=format;
        if(av_frame_get_buffer(frame,64)<0)throw std::bad_alloc();
        frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_BT709;
        frame->color_primaries=AVCOL_PRI_BT709;frame->color_trc=AVCOL_TRC_BT709;
        for(const auto* b:frame->buf)if(b)std::memset(b->data,0xcd,b->size);
        packed.resize(size_t(frame->width)*frame->height*3/2);
        for(int y=0;y<1080;++y)for(int x=0;x<1920;++x) {
            if(format==AV_PIX_FMT_BGR24) {
                auto* pixel=frame->data[0]+y*frame->linesize[0]+x*3;
                pixel[0]=BYTE((x*3+y*7)%256);pixel[1]=BYTE((x+y*11)%256);pixel[2]=BYTE((x*5+y)%256);
            } else {
                const BYTE value=BYTE(16+(x*7+y*13)%220);
                frame->data[0][y*frame->linesize[0]+x]=value;packed[size_t(y)*1920+x]=value;
            }
        }
        if(format!=AV_PIX_FMT_BGR24)for(int y=0;y<540;++y)for(int x=0;x<960;++x) {
            const BYTE u=BYTE(16+(x*11+y*17)%225),v=BYTE(16+(x*19+y*5)%225);
            if(format==AV_PIX_FMT_NV12) {frame->data[1][y*frame->linesize[1]+2*x]=u;frame->data[1][y*frame->linesize[1]+2*x+1]=v;}
            else {frame->data[1][y*frame->linesize[1]+x]=u;frame->data[2][y*frame->linesize[2]+x]=v;}
            packed[size_t(1920)*1080+size_t(y)*1920+2*x]=u;packed[size_t(1920)*1080+size_t(y)*1920+2*x+1]=v;
        }
    }
    ~Input(){av_frame_free(&frame);}
};
void Write(MFSharedMemWriter& writer,AVFrame* frame) {
#ifdef MF_REFERENCE_WRITER
    writer.WriteFrame(frame);
#else
    if(!writer.WriteFrame(frame))throw std::runtime_error("Incomplete benchmark publication");
#endif
}
uint64_t Hash(const BYTE* bytes,size_t length) {
    uint64_t value=1469598103934665603ULL;
    for(size_t i=0;i<length;++i){value^=bytes[i];value*=1099511628211ULL;}return value;
}
void Measure(const char* label,AVPixelFormat format,AVColorRange range,AVColorSpace matrix) {
    Input input(format);input.frame->color_range=range;input.frame->colorspace=matrix;
#ifdef MF_REFERENCE_WRITER
    MFSharedMemWriter writer;
#else
    Names names;names.primaryMapping.clear();names.primaryMutex.clear();names.fallbackMapping=MapName();names.fallbackMutex=MutexName();
    MFSharedMemWriter writer(names);
#endif
    if(!writer.Initialize(1920,1080))throw std::runtime_error("Benchmark initialization failed");
    constexpr int warmup=24,samples=360;
    for(int i=0;i<warmup;++i)Write(writer,input.frame);
    std::vector<double> times;times.reserve(samples);
    for(int i=0;i<samples;++i) {
        input.frame->data[0][0]=BYTE(16+i%220); // A changing current-frame pixel, outside timed preparation.
        const auto begin=std::chrono::steady_clock::now();Write(writer,input.frame);
        times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
    }
    std::sort(times.begin(),times.end());
    HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,MapName());
    HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,MutexName());
    auto* shared=map?static_cast<SharedMemHeader*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0)):nullptr;
    if(!shared||!mutex||WaitForSingleObject(mutex,2000)!=WAIT_OBJECT_0)throw std::runtime_error("Benchmark probe failed");
    const size_t bytes=size_t(1920)*1080*3/2;
    if(shared->width!=1920||shared->height!=1080||shared->frameSize!=bytes||shared->frameIndex!=warmup+samples)
        throw std::runtime_error("Benchmark output geometry/index mismatch");
    input.packed[0]=BYTE(16+(samples-1)%220);
    const bool identity=range==AVCOL_RANGE_MPEG&&matrix==AVCOL_SPC_BT709&&format!=AV_PIX_FMT_BGR24;
    const bool exact=identity&&std::memcmp(input.packed.data(),shared->data,bytes)==0;
    std::cout<<std::fixed<<std::setprecision(6)<<"BENCH "<<label<<" samples="<<samples<<" p50_ms="<<times[179]
        <<" p95_ms="<<times[341]<<" hash64="<<Hash(shared->data,bytes)<<" identity_expected="<<identity<<" identity_pass="<<exact
        <<" publication_index="<<shared->frameIndex<<"\n";
    if(identity&&!exact)throw std::runtime_error("Exact colour sample identity failed");
    ReleaseMutex(mutex);UnmapViewOfFile(shared);CloseHandle(mutex);CloseHandle(map);
}
void ColourProbe() {
    Input input(AV_PIX_FMT_BGR24);
    for(int y=0;y<1080;++y)for(int x=0;x<1920;++x){auto* p=input.frame->data[0]+y*input.frame->linesize[0]+3*x;p[0]=0;p[1]=0;p[2]=255;}
    input.frame->color_range=AVCOL_RANGE_JPEG;input.frame->colorspace=AVCOL_SPC_RGB;
#ifdef MF_REFERENCE_WRITER
    MFSharedMemWriter writer;
#else
    Names names;names.primaryMapping.clear();names.primaryMutex.clear();names.fallbackMapping=MapName();names.fallbackMutex=MutexName();
    MFSharedMemWriter writer(names);
#endif
    if(!writer.Initialize(1920,1080))throw std::runtime_error("Probe initialization failed");Write(writer,input.frame);
    HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,MapName());auto* shared=map?static_cast<SharedMemHeader*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0)):nullptr;
    if(!shared)throw std::runtime_error("Colour probe mapping failed");
    const int y=shared->data[0],u=shared->data[1920*1080],v=shared->data[1920*1080+1];
    const bool correct=std::abs(y-63)<=2&&std::abs(u-102)<=2&&std::abs(v-240)<=2;
    std::cout<<"PROBE RGB_red advertised_limited_BT709 y="<<y<<" u="<<u<<" v="<<v<<" correct="<<correct<<"\n";
    UnmapViewOfFile(shared);CloseHandle(map);
#ifndef MF_REFERENCE_WRITER
    if(!correct)throw std::runtime_error("Limited BT709 colour probe failed");
#endif
}
int main() {
    try {
        Measure("NV12_MPEG_BT709",AV_PIX_FMT_NV12,AVCOL_RANGE_MPEG,AVCOL_SPC_BT709);
        Measure("YUV420P_MPEG_BT709",AV_PIX_FMT_YUV420P,AVCOL_RANGE_MPEG,AVCOL_SPC_BT709);
        Measure("NV12_JPEG_BT709",AV_PIX_FMT_NV12,AVCOL_RANGE_JPEG,AVCOL_SPC_BT709);
        Measure("NV12_MPEG_BT601",AV_PIX_FMT_NV12,AVCOL_RANGE_MPEG,AVCOL_SPC_SMPTE170M);
        Measure("BGR24_JPEG_RGB",AV_PIX_FMT_BGR24,AVCOL_RANGE_JPEG,AVCOL_SPC_RGB);
        ColourProbe();return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
