#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mfsource/MFSharedMemWriter.h"
#include "mfsource/FrameServer.h"
#include "mfsource/MediaStream.h"
#include "effects/desktopcolorpipeline.h"
#include <mfapi.h>
#include <mferror.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

using namespace PrismForge::CameraTransport;
using Microsoft::WRL::ComPtr;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; return false; } } while(false)

Names Unique() {
    static std::atomic<int> count{0};
    const auto base = L"Local\\PrismForge_Test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++count);
    Names names; names.primaryMapping.clear(); names.primaryMutex.clear();
    names.fallbackMapping = base + L"_map"; names.fallbackMutex = base + L"_mutex";
    return names;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
    void Reset() { if (value) CloseHandle(value); value = nullptr; }
};
struct HeldMutex {
    Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<bool> released{false};
    std::thread owner;
    explicit HeldMutex(HANDLE mutex) : owner([this, mutex] {
        const DWORD result = WaitForSingleObject(mutex, 2000);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            SetEvent(ready.value); WaitForSingleObject(done.value, 10000);
            released = ReleaseMutex(mutex) != FALSE;
        }
    }) {
        if (WaitForSingleObject(ready.value, 2000) != WAIT_OBJECT_0) throw std::runtime_error("Mutex owner barrier failed");
    }
    void Finish() { SetEvent(done.value); if (owner.joinable()) owner.join(); }
    ~HeldMutex() { Finish(); }
};
struct Frame {
    AVFrame* value = av_frame_alloc();
    Frame(int w = 16, int h = 8, AVPixelFormat format = AV_PIX_FMT_BGR24) {
        if (!value) throw std::bad_alloc();
        value->width = w; value->height = h; value->format = format;
        if (av_frame_get_buffer(value, 64) < 0) throw std::bad_alloc();
        value->color_range = AVCOL_RANGE_JPEG; value->colorspace = AVCOL_SPC_RGB;
        value->color_primaries = AVCOL_PRI_BT709; value->color_trc = AVCOL_TRC_BT709;
        Fill(255,255,255);
    }
    ~Frame() { av_frame_free(&value); }
    void Fill(int b, int g, int r) {
        if (value->format != AV_PIX_FMT_BGR24) return;
        for (int y = 0; y < value->height; ++y) {
            std::memset(value->data[0] + y * value->linesize[0], 0xcd, value->linesize[0]);
            for (int x = 0; x < value->width; ++x) {
                auto* p = value->data[0] + y * value->linesize[0] + x * 3;
                p[0] = b; p[1] = g; p[2] = r;
            }
        }
    }
    void Yuv(int y, int u, int v) {
        value->color_range = AVCOL_RANGE_MPEG; value->colorspace = AVCOL_SPC_BT709;
        for (int row = 0; row < value->height; ++row) std::memset(value->data[0] + row * value->linesize[0], y, value->width);
        if (value->format == AV_PIX_FMT_NV12) {
            for (int row = 0; row < (value->height + 1)/2; ++row) for (int x = 0; x < (value->width + 1)/2; ++x) {
                value->data[1][row * value->linesize[1] + x * 2] = u;
                value->data[1][row * value->linesize[1] + x * 2 + 1] = v;
            }
        } else for (int p = 1; p <= 2; ++p) for (int row = 0; row < (value->height + 1)/2; ++row)
            std::memset(value->data[p] + row * value->linesize[p], p == 1 ? u : v, (value->width + 1)/2);
    }
};
struct Fixture {
    Names names = Unique();
    MFSharedMemWriter writer{names};
    FrameServer reader{names};
    Mapping inspect;
    SharedFrameSnapshot snapshot;
    Fixture(int w = 16, int h = 8) {
        if (!writer.Initialize(w,h) || FAILED(reader.Initialize()) ||
            !inspect.Open(names.fallbackMapping,names.fallbackMutex,false)) throw std::runtime_error("IPC fixture initialization failed");
    }
};
bool Uniform(const SharedFrameSnapshot& s, int y, int u = 128, int v = 128, int tolerance = 1) {
    if (s.data.size() != size_t(s.width) * s.height * 3 / 2) return false;
    for (size_t i = 0; i < size_t(s.width) * s.height; ++i) if (std::abs(int(s.data[i]) - y) > tolerance) return false;
    for (size_t i = size_t(s.width) * s.height; i < s.data.size(); i += 2)
        if (std::abs(int(s.data[i]) - u) > tolerance || std::abs(int(s.data[i+1]) - v) > tolerance) return false;
    return true;
}
std::vector<BYTE> State(Mapping& mapping) {
    MutexLock lock(mapping.mutex);
    if (!lock.Owned()) throw std::runtime_error("Snapshot lock failed");
    const size_t bytes = offsetof(SharedMemHeader,data) + mapping.header->frameSize;
    const auto* start = reinterpret_cast<const BYTE*>(mapping.header);
    return {start,start+bytes};
}
bool LazyStart() {
    const auto names = Unique(); MFSharedMemWriter writer(names); Frame frame;
    CHECK(writer.WriteFrame(frame.value)); // previously deadlocked recursively in Initialize
    FrameServer reader(names); CHECK(reader.Initialize() == S_OK);
    SharedFrameSnapshot snapshot; CHECK(reader.GetLatestFrame(snapshot) == S_OK);
    CHECK(snapshot.width == 1920 && snapshot.height == 1080 && snapshot.frameIndex == 1);
    return true;
}
bool OutputBounds() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); const auto before = State(f.inspect);
    for (const auto [w,h] : {std::pair<int,int>{0,8}, {-1,8}, {3,8}, {8,3}, {3842,2160}, {3840,2162}, {INT_MAX,INT_MAX}})
        CHECK(!f.writer.Initialize(w,h));
    CHECK(before == State(f.inspect)); CHECK(f.writer.Initialize(3840,2160));
    CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    CHECK(f.snapshot.data.size() == PRISMFORGE_MAX_FRAME_SIZE);
    return true;
}
bool InvalidInputs() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); const auto before = State(f.inspect);
    CHECK(!f.writer.WriteFrame(nullptr));
    AVFrame invalid = *frame.value;
    invalid.data[0] = nullptr; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.linesize[0] = 1; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.linesize[0] = -64; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.width = INT_MAX; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.format = AV_PIX_FMT_D3D11; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.format = AV_PIX_FMT_P010LE; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.color_trc = AVCOL_TRC_SMPTE2084; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.color_trc = AVCOL_TRC_ARIB_STD_B67; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.color_primaries = AVCOL_PRI_BT2020; CHECK(!f.writer.WriteFrame(&invalid)); invalid = *frame.value;
    invalid.data[0] = frame.value->buf[0]->data + frame.value->buf[0]->size - 1; CHECK(!f.writer.WriteFrame(&invalid));
    Frame yuv(16,8,AV_PIX_FMT_NV12); yuv.Yuv(90,128,128); invalid = *yuv.value;
    invalid.data[1] = nullptr; CHECK(!f.writer.WriteFrame(&invalid));
    CHECK(before == State(f.inspect)); CHECK(f.writer.WriteFrame(frame.value));
    return true;
}
bool OwnedSnapshot() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    const auto retained = f.snapshot.data; const auto index = f.snapshot.frameIndex;
    frame.Fill(0,0,0); CHECK(f.writer.WriteFrame(frame.value));
    CHECK(f.snapshot.data == retained && f.snapshot.frameIndex == index && Uniform(f.snapshot,235));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,16));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == E_PENDING && f.snapshot.isStreaming && Uniform(f.snapshot,16));
    return true;
}
bool GeometryTransaction() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); const auto oldIndex = f.inspect.header->frameIndex;
    CHECK(f.writer.Initialize(8,16)); CHECK(f.inspect.header->width == 16 && f.inspect.header->height == 8);
    CHECK(!f.inspect.header->isStreaming && f.inspect.header->frameIndex == oldIndex);
    Frame portrait(8,16); CHECK(f.writer.WriteFrame(portrait.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    CHECK(f.snapshot.width == 8 && f.snapshot.height == 16 && Uniform(f.snapshot,235));
    return true;
}
bool WriterContention() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); const auto before = State(f.inspect);
    HeldMutex held(f.inspect.mutex); frame.Fill(0,0,0);
    CHECK(!f.writer.WriteFrame(frame.value));
    CHECK(std::memcmp(before.data(),f.inspect.header,before.size()) == 0);
    CHECK(!f.writer.Initialize(8,16)); CHECK(std::memcmp(before.data(),f.inspect.header,before.size()) == 0);
    held.Finish(); CHECK(held.released); CHECK(f.writer.WriteFrame(frame.value));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,16));
    return true;
}
bool ReaderContention() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    const auto retained = f.snapshot.data;
    HeldMutex held(f.inspect.mutex); CHECK(f.reader.GetLatestFrame(f.snapshot) == E_PENDING);
    CHECK(!f.snapshot.isStreaming && f.snapshot.data == retained);
    held.Finish(); CHECK(held.released);
    CHECK(f.reader.GetLatestFrame(f.snapshot) == E_PENDING && f.snapshot.isStreaming);
    return true;
}
bool PauseResume() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    f.writer.SetStreaming(false); CHECK(!f.inspect.header->isStreaming);
    f.writer.SetStreaming(true); CHECK(!f.inspect.header->isStreaming);
    CHECK(f.reader.GetLatestFrame(f.snapshot) == E_PENDING && !f.snapshot.isStreaming);
    CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && f.snapshot.isStreaming);
    return true;
}
bool ContendedStopFreshness() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); const auto before = State(f.inspect);
    HeldMutex held(f.inspect.mutex); f.writer.SetStreaming(false);
    CHECK(std::memcmp(before.data(),f.inspect.header,before.size()) == 0);
    f.writer.Close(); CHECK(std::memcmp(before.data(),f.inspect.header,before.size()) == 0);
    held.Finish(); CHECK(held.released);
    { MutexLock lock(f.inspect.mutex); CHECK(lock.Owned());
      const auto expired = GetTickCount64() - MaxFrameAgeMs - 1;
      f.inspect.header->reserved[1] = UINT32(expired); f.inspect.header->reserved[2] = UINT32(expired >> 32); }
    CHECK(f.reader.GetLatestFrame(f.snapshot) == E_PENDING && !f.snapshot.isStreaming);
    CHECK(f.writer.Initialize(16,8) && f.writer.WriteFrame(frame.value));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && f.snapshot.isStreaming);
    return true;
}
void Abandon(HANDLE mutex) { std::thread([mutex] { WaitForSingleObject(mutex,2000); }).join(); }
bool AbandonedWriter() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); Abandon(f.inspect.mutex);
    frame.Fill(0,0,0); CHECK(f.writer.WriteFrame(frame.value));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,16));
    return true;
}
bool AbandonedReader() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); Abandon(f.inspect.mutex);
    CHECK(f.reader.GetLatestFrame(f.snapshot) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    CHECK(!f.snapshot.isStreaming && !f.inspect.header->isStreaming);
    CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,235));
    return true;
}
bool TruncatedMapping() {
    auto names = Unique();
    Handle tiny{CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,names.fallbackMapping.c_str())};
    CHECK(tiny.value);
    MFSharedMemWriter writer(names); FrameServer reader(names);
    CHECK(!writer.Initialize(16,8)); CHECK(FAILED(reader.Initialize()));
    return true;
}
bool WrongMutexObject() {
    auto names = Unique(); Handle event{CreateEventW(nullptr,TRUE,FALSE,names.fallbackMutex.c_str())}; CHECK(event.value);
    MFSharedMemWriter writer(names); FrameServer reader(names);
    CHECK(!writer.Initialize(16,8)); CHECK(FAILED(reader.Initialize()));
    event.Reset(); CHECK(writer.Initialize(16,8)); CHECK(reader.Initialize() == S_OK);
    Frame frame; CHECK(writer.WriteFrame(frame.value)); SharedFrameSnapshot snapshot; CHECK(reader.GetLatestFrame(snapshot) == S_OK);
    return true;
}
Names UpgradeNames() {
    auto names = Unique(); names.primaryMapping = names.fallbackMapping + L"_primary";
    names.primaryMutex = names.fallbackMutex + L"_primary"; return names;
}
bool UpgradeSequence() {
    auto names = UpgradeNames(); Handle block{CreateEventW(nullptr,TRUE,FALSE,names.primaryMutex.c_str())}; CHECK(block.value);
    MFSharedMemWriter writer(names); CHECK(writer.Initialize(16,8)); Mapping old; CHECK(old.Open(names.fallbackMapping,names.fallbackMutex,false));
    Frame frame; CHECK(writer.WriteFrame(frame.value)); block.Reset();
    FrameServer reader(names); CHECK(reader.Initialize() == S_OK); Mapping primary; CHECK(primary.Open(names.primaryMapping,names.primaryMutex,false));
    { MutexLock lock(primary.mutex); CHECK(lock.Owned()); primary.header->frameIndex = 1000; }
    for (int i = 0; i < 89; ++i) CHECK(writer.WriteFrame(frame.value));
    SharedFrameSnapshot snapshot; CHECK(reader.GetLatestFrame(snapshot) == S_OK);
    CHECK(snapshot.frameIndex == 1001 && Uniform(snapshot,235) && !old.header->isStreaming);
    CHECK(primary.header->width == 16 && primary.header->height == 8 && primary.header->frameSize == 192);
    return true;
}
bool UpgradeMissingMutex() {
    auto names = UpgradeNames(); Handle block{CreateEventW(nullptr,TRUE,FALSE,names.primaryMutex.c_str())};
    MFSharedMemWriter writer(names); CHECK(writer.Initialize(16,8)); Mapping old; CHECK(old.Open(names.fallbackMapping,names.fallbackMutex,false));
    block.Reset(); Handle map{CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,MappingSize,names.primaryMapping.c_str())}; CHECK(map.value);
    Frame frame; for (int i = 0; i < 90; ++i) CHECK(writer.WriteFrame(frame.value));
    CHECK(old.header->frameIndex == 90 && old.header->isStreaming);
    CHECK(!OpenMutexW(SYNCHRONIZE,FALSE,names.primaryMutex.c_str()));
    return true;
}
bool UpgradeContention() {
    auto names = UpgradeNames(); Handle block{CreateEventW(nullptr,TRUE,FALSE,names.primaryMutex.c_str())};
    MFSharedMemWriter writer(names); CHECK(writer.Initialize(16,8)); Mapping old; CHECK(old.Open(names.fallbackMapping,names.fallbackMutex,false));
    block.Reset(); FrameServer reader(names); CHECK(reader.Initialize() == S_OK);
    Mapping primary; CHECK(primary.Open(names.primaryMapping,names.primaryMutex,false)); HeldMutex held(primary.mutex);
    Frame frame; for (int i = 0; i < 90; ++i) CHECK(writer.WriteFrame(frame.value));
    CHECK(old.header->frameIndex == 90 && old.header->isStreaming && !primary.header->frameIndex && !primary.header->isStreaming);
    held.Finish(); CHECK(held.released);
    for (int i = 0; i < 90; ++i) CHECK(writer.WriteFrame(frame.value));
    SharedFrameSnapshot snapshot; CHECK(reader.GetLatestFrame(snapshot) == S_OK && snapshot.frameIndex == 180);
    CHECK(!old.header->isStreaming && Uniform(snapshot,235));
    return true;
}
bool InvalidHeader() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    const auto retained = f.snapshot.data; const SharedMemHeader good = *f.inspect.header;
    const std::function<void(SharedMemHeader&)> corruptions[]{
        [](auto& h){h.width=3;}, [](auto& h){h.height=UINT_MAX;}, [](auto& h){h.stride+=8;},
        [](auto& h){h.frameSize=UINT_MAX;}, [](auto& h){h.frameIndex=0;}, [](auto& h){h.isStreaming=2;},
        [](auto& h){h.reserved[0]=123;}, [](auto& h){h.reserved[1]=UINT_MAX; h.reserved[2]=UINT_MAX;}
    };
    for (const auto& corrupt : corruptions) {
        { MutexLock lock(f.inspect.mutex); CHECK(lock.Owned()); *f.inspect.header = good; corrupt(*f.inspect.header); }
        CHECK(f.reader.GetLatestFrame(f.snapshot) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        CHECK(!f.snapshot.isStreaming && f.snapshot.data == retained);
    }
    { MutexLock lock(f.inspect.mutex); CHECK(lock.Owned()); *f.inspect.header = good; }
    CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    return true;
}
bool Encoded709() {
    Fixture f; Frame frame;
    for (const auto [gray,y] : {std::pair<int,int>{0,16},{255,235},{128,126}}) {
        frame.Fill(gray,gray,gray); CHECK(f.writer.WriteFrame(frame.value));
        CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,y));
    }
    frame.Fill(0,0,255); CHECK(f.writer.WriteFrame(frame.value));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,63,102,240,2));
    frame.Fill(255,0,0); CHECK(f.writer.WriteFrame(frame.value));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,32,240,118,2));
    return true;
}
bool RangeMatrixRefresh() {
    Fixture f; Frame input(16,8,AV_PIX_FMT_NV12); input.Yuv(80,128,128);
    CHECK(f.writer.WriteFrame(input.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,80));
    input.value->color_range = AVCOL_RANGE_JPEG;
    CHECK(f.writer.WriteFrame(input.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,85,128,128,2));
    input.Yuv(120,80,170);
    int lastY = -1;
    for (const auto matrix : {AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709}) {
        input.value->colorspace = matrix;
        CHECK(f.writer.WriteFrame(input.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
        const double y = 1.16438356 * (120-16), u = 80-128, v = 170-128;
        const bool hd = matrix == AVCOL_SPC_BT709;
        const double r = std::clamp(y + (hd ? 1.792741 : 1.596027)*v,0.0,255.0);
        const double g = std::clamp(y - (hd ? .213249 : .391762)*u - (hd ? .532909 : .812968)*v,0.0,255.0);
        const double b = std::clamp(y + (hd ? 2.112402 : 2.017232)*u,0.0,255.0);
        const double luma = .2126*r + .7152*g + .0722*b;
        const int expectedY = int(std::lround(16+219*luma/255));
        const int expectedU = int(std::lround(128+224*(b-luma)/(255*2*(1-.0722))));
        const int expectedV = int(std::lround(128+224*(r-luma)/(255*2*(1-.2126))));
        CHECK(Uniform(f.snapshot,expectedY,expectedU,expectedV,2));
        if (lastY >= 0) CHECK(lastY != int(f.snapshot.data[0]));
        lastY = f.snapshot.data[0];
    }
    // Planar decoder output and swapped-chroma camera input use the same advertised encoding.
    Frame planar(16,8,AV_PIX_FMT_YUV420P); planar.Yuv(91,128,128);
    CHECK(f.writer.WriteFrame(planar.value));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && Uniform(f.snapshot,91));
    input.Yuv(120,80,170); input.value->colorspace = AVCOL_SPC_BT709;
    CHECK(f.writer.WriteFrame(input.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    const auto expected = f.snapshot.data;
    input.value->format = AV_PIX_FMT_NV21;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 16; x += 2)
        std::swap(input.value->data[1][y*input.value->linesize[1]+x], input.value->data[1][y*input.value->linesize[1]+x+1]);
    CHECK(f.writer.WriteFrame(input.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && f.snapshot.data == expected);
    return true;
}
// Non-neutral samples and poisoned, differently offset source rows ensure that
// transport output contains active pixels only, with the NV12 U/V order intact.
std::vector<BYTE> Pattern420(AVFrame* frame, int seed) {
    for (const auto* buffer : frame->buf) if (buffer) std::memset(buffer->data,0xcd,buffer->size);
    const int w=frame->width,h=frame->height;
    std::vector<BYTE> packed(size_t(w)*h*3/2);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const BYTE value=BYTE(16+(x*7+y*13+seed)%220);
        frame->data[0][y*frame->linesize[0]+x]=value;packed[size_t(y)*w+x]=value;
    }
    for(int y=0;y<h/2;++y)for(int x=0;x<w/2;++x) {
        const BYTE u=BYTE(16+(x*11+y*17+seed*3)%225),v=BYTE(16+(x*19+y*5+seed*7)%225);
        if(frame->format==AV_PIX_FMT_NV12) {
            frame->data[1][y*frame->linesize[1]+2*x]=u;frame->data[1][y*frame->linesize[1]+2*x+1]=v;
        } else {
            frame->data[1][y*frame->linesize[1]+x]=u;frame->data[2][y*frame->linesize[2]+x]=v;
        }
        packed[size_t(w)*h+size_t(y)*w+2*x]=u;packed[size_t(w)*h+size_t(y)*w+2*x+1]=v;
    }
    frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_BT709;
    return packed;
}
std::vector<std::vector<BYTE>> BufferBytes(const AVFrame* frame) {
    std::vector<std::vector<BYTE>> result;
    for(const auto* buffer:frame->buf)if(buffer)result.emplace_back(buffer->data,buffer->data+buffer->size);
    return result;
}
bool Exact420Strides(AVPixelFormat format) {
    Fixture f(34,10);Frame source(34,10,format);
    CHECK(source.value->linesize[0]>34 && source.value->linesize[1]>(format==AV_PIX_FMT_NV12?34:17));
    source.value->data[0]+=3;source.value->data[1]+=5;
    if(format==AV_PIX_FMT_YUV420P)source.value->data[2]+=7;
    for(int seed:{3,27,101}) {
        const auto expected=Pattern420(source.value,seed);const auto before=BufferBytes(source.value);
        CHECK(f.writer.WriteFrame(source.value));CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK);
        CHECK(f.snapshot.width==34&&f.snapshot.height==10&&f.snapshot.data==expected);
        CHECK(BufferBytes(source.value)==before);
    }
    const auto previous=State(f.inspect);
    AVFrame invalid=*source.value;
    const auto* owner=av_frame_get_plane_buffer(source.value,1);CHECK(owner);
    invalid.data[1]=owner->data+owner->size-1;
    CHECK(!f.writer.WriteFrame(&invalid));CHECK(State(f.inspect)==previous);
    if(format==AV_PIX_FMT_YUV420P) {
        invalid=*source.value;invalid.linesize[2]=16;
        CHECK(!f.writer.WriteFrame(&invalid));CHECK(State(f.inspect)==previous);
    }
    return true;
}
bool ExactNv12Strides(){return Exact420Strides(AV_PIX_FMT_NV12);}
bool ExactPlanarStrides(){return Exact420Strides(AV_PIX_FMT_YUV420P);}
// Independent YUV->RGB->limited BT.709 equations, rather than an expectation
// derived from the writer's packing/conversion implementation.
bool ExpectedYuv709(const SharedFrameSnapshot& snapshot,int yCode,int uCode,int vCode,bool full,bool hd) {
    const double y=full?yCode:255.0*(yCode-16)/219.0;
    const double u=(uCode-128)*(full?1.0:255.0/224.0),v=(vCode-128)*(full?1.0:255.0/224.0);
    const double r=std::clamp(y+(hd?1.5748:1.402)*v,0.0,255.0);
    const double g=std::clamp(y-(hd?.187324:.344136)*u-(hd?.468124:.714136)*v,0.0,255.0);
    const double b=std::clamp(y+(hd?1.8556:1.772)*u,0.0,255.0);
    const double luma=.2126*r+.7152*g+.0722*b;
    return Uniform(snapshot,int(std::lround(16+219*luma/255)),
        int(std::lround(128+224*(b-luma)/(255*2*(1-.0722)))),
        int(std::lround(128+224*(r-luma)/(255*2*(1-.2126)))),2);
}
bool Alternating420Metadata() {
    Fixture f;Frame nv12(16,8,AV_PIX_FMT_NV12),planar(16,8,AV_PIX_FMT_YUV420P),rgb;
    // Re-enter each exact-copy path after generic conversion and the other
    // source format, testing current-frame semantics rather than cached gates.
    for(int iteration=0;iteration<3;++iteration) {
        for(auto* frame:{nv12.value,planar.value}) {
            for(const auto semantics:{std::pair<bool,bool>{false,true},{true,true},{false,false},{true,false},{false,true}}) {
                (frame==nv12.value?nv12:planar).Yuv(120,80,170);
                frame->color_range=semantics.first?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG;
                frame->colorspace=semantics.second?AVCOL_SPC_BT709:AVCOL_SPC_SMPTE170M;
                CHECK(f.writer.WriteFrame(frame));CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK);
                CHECK(ExpectedYuv709(f.snapshot,120,80,170,semantics.first,semantics.second));
            }
        }
        rgb.Fill(0,0,255);CHECK(f.writer.WriteFrame(rgb.value));
        CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK&&Uniform(f.snapshot,63,102,240,2));
        const auto expected=Pattern420(nv12.value,iteration+1);
        CHECK(f.writer.WriteFrame(nv12.value));CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK&&f.snapshot.data==expected);
    }
    return true;
}
bool ExactCopyToAspectBars() {
    Fixture f(34,10);Frame exact(34,10,AV_PIX_FMT_NV12),portrait(10,34);
    for(int seed:{4,29,117}) {
        const auto expected=Pattern420(exact.value,seed);
        CHECK(f.writer.WriteFrame(exact.value));CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK&&f.snapshot.data==expected);
        portrait.Fill(255,255,255);CHECK(f.writer.WriteFrame(portrait.value));
        CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK);
        // Fit is 2x10 centered at even x=16, leaving independently checked bars.
        for(int y=0;y<10;++y)for(int x=0;x<34;++x)
            if(std::abs(int(f.snapshot.data[y*34+x])-(x>=16&&x<18?235:16))>1){std::cerr<<"Bar mismatch x="<<x<<" y="<<y<<" value="<<int(f.snapshot.data[y*34+x])<<"\n";return false;}
        for(size_t i=340;i<f.snapshot.data.size();++i)CHECK(f.snapshot.data[i]==128);
        CHECK(f.writer.WriteFrame(exact.value));CHECK(f.reader.GetLatestFrame(f.snapshot)==S_OK&&f.snapshot.data==expected);
    }
    return true;
}
bool OddSourceFit() {
    Fixture f(8,8); Frame frame(5,3); const auto bytes = std::vector<BYTE>(frame.value->buf[0]->data,frame.value->buf[0]->data+frame.value->buf[0]->size);
    CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    for (int row = 0; row < 8; ++row) for (int x = 0; x < 8; ++x)
        CHECK(std::abs(int(f.snapshot.data[row*8+x]) - (row >= 2 && row < 6 ? 235 : 16)) <= 1);
    CHECK(std::memcmp(frame.value->buf[0]->data,bytes.data(),bytes.size()) == 0);
    return true;
}
bool ConcurrentWholeFrames() {
    Fixture f; Frame white; Frame black(8,16); black.Fill(0,0,0);
    CHECK(f.writer.WriteFrame(white.value));
    std::atomic<bool> stop{false}, valid{true}; std::atomic<UINT64> observed{0};
    Handle ready{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::thread consumer([&] {
        SharedFrameSnapshot snapshot; UINT64 last = 0;
        while (!stop) {
            const auto hr = f.reader.GetLatestFrame(snapshot);
            if (hr == S_OK) {
                if (snapshot.frameIndex <= last || !snapshot.isStreaming ||
                    !((snapshot.width == 16 && snapshot.height == 8 && Uniform(snapshot,235)) ||
                      (snapshot.width == 8 && snapshot.height == 16 && Uniform(snapshot,16)))) valid=false;
                last = snapshot.frameIndex; observed=last; SetEvent(ready.value);
            } else if (hr != E_PENDING) valid=false;
            std::this_thread::yield();
        }
    });
    bool success = WaitForSingleObject(ready.value,2000) == WAIT_OBJECT_0;
    for (int i = 1; success && i <= 200; ++i) {
        auto* frame = (i & 1) ? black.value : white.value;
        success = f.writer.Initialize(frame->width,frame->height) && f.writer.WriteFrame(frame);
        if (i % 20 == 0) {
            const auto until = GetTickCount64()+2000;
            while (observed < UINT64(i+1) && GetTickCount64() < until) std::this_thread::yield();
            success = observed >= UINT64(i+1);
        }
    }
    stop=true; consumer.join(); CHECK(success && valid && observed == 201);
    return true;
}
bool MonotonicRestartOverflow() {
    Fixture f; Frame frame; CHECK(f.writer.WriteFrame(frame.value));
    f.writer.Close(); CHECK(f.writer.Initialize(16,8)); CHECK(f.writer.WriteFrame(frame.value));
    CHECK(f.inspect.header->frameIndex == 2);
    { MutexLock lock(f.inspect.mutex); CHECK(lock.Owned()); f.inspect.header->frameIndex=UINT64_MAX; }
    const auto before = State(f.inspect); CHECK(!f.writer.WriteFrame(frame.value)); CHECK(before == State(f.inspect));
    { MutexLock lock(f.inspect.mutex); CHECK(lock.Owned()); f.inspect.header->frameIndex=40; }
    CHECK(f.writer.WriteFrame(frame.value)); CHECK(f.inspect.header->frameIndex == 41);
    return true;
}
bool SampleBytes(VirtualCamMediaStream* stream, std::vector<BYTE>& bytes) {
    CHECK(stream->RequestSample(nullptr) == S_OK); ComPtr<IMFMediaEvent> event;
    CHECK(stream->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event) == S_OK);
    MediaEventType type; CHECK(event->GetType(&type) == S_OK && type == MEMediaSample);
    PROPVARIANT value; PropVariantInit(&value); CHECK(event->GetValue(&value) == S_OK);
    ComPtr<IMFSample> sample; const auto hr = value.punkVal->QueryInterface(IID_PPV_ARGS(&sample)); PropVariantClear(&value); CHECK(hr == S_OK);
    ComPtr<IMFMediaBuffer> buffer; CHECK(sample->ConvertToContiguousBuffer(&buffer) == S_OK);
    BYTE* data; DWORD size; CHECK(buffer->Lock(&data,nullptr,&size) == S_OK);
    bytes.assign(data,data+size); CHECK(buffer->Unlock() == S_OK); return true;
}
bool ActualMfSamples() {
    auto names=Unique(); MFSharedMemWriter writer(names); CHECK(writer.Initialize(16,8)); Frame frame;
    ComPtr<VirtualCamMediaStream> stream;
    CHECK(Microsoft::WRL::MakeAndInitialize<VirtualCamMediaStream>(&stream,static_cast<VirtualCamMediaSource*>(nullptr),names) == S_OK);
    ComPtr<IMFStreamDescriptor> descriptor; CHECK(stream->GetStreamDescriptor(&descriptor) == S_OK);
    ComPtr<IMFMediaTypeHandler> handler; CHECK(descriptor->GetMediaTypeHandler(&handler) == S_OK);
    ComPtr<IMFMediaType> type; CHECK(handler->GetMediaTypeByIndex(6,&type) == S_OK && handler->SetCurrentMediaType(type.Get()) == S_OK);
    UINT32 range=0,matrix=0; CHECK(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,&range) == S_OK && range == MFNominalRange_16_235);
    CHECK(type->GetUINT32(MF_MT_YUV_MATRIX,&matrix) == S_OK && matrix == MFVideoTransferMatrix_BT709);
    stream->SetActive(true); CHECK(writer.WriteFrame(frame.value)); std::vector<BYTE> bytes;
    CHECK(SampleBytes(stream.Get(),bytes)); CHECK(bytes.size() == 640*480*3/2);
    CHECK(bytes[0] == 16 && bytes[79*640+300] == 16 && bytes[80*640+300] == 235 && bytes[400*640+300] == 16);
    const auto retained=bytes; CHECK(SampleBytes(stream.Get(),bytes) && bytes == retained);
    // Renegotiation must rescale the owned current frame even without a new camera index.
    CHECK(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,1280,720) == S_OK);
    CHECK(SampleBytes(stream.Get(),bytes) && bytes.size() == 1280*720*3/2);
    CHECK(bytes[0] == 16 && bytes[200*1280+300] == 235);
    CHECK(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,640,480) == S_OK);
    writer.SetStreaming(false); CHECK(SampleBytes(stream.Get(),bytes) && bytes[0] == 32);
    writer.SetStreaming(true); CHECK(SampleBytes(stream.Get(),bytes) && bytes[0] == 32);
    frame.Fill(0,0,0); CHECK(writer.WriteFrame(frame.value)); CHECK(SampleBytes(stream.Get(),bytes) && bytes[200*640+300] == 16);
    CHECK(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,3,3) == S_OK);
    CHECK(stream->RequestSample(nullptr) == MF_E_INVALIDMEDIATYPE);
    CHECK(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,640,480) == S_OK);
    CHECK(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32) == S_OK);
    CHECK(stream->RequestSample(nullptr) == MF_E_INVALIDMEDIATYPE);
    CHECK(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12) == S_OK);
    CHECK(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_0_255) == S_OK);
    CHECK(stream->RequestSample(nullptr) == MF_E_INVALIDMEDIATYPE);
    CHECK(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235) == S_OK);
    stream->Shutdown(); CHECK(stream->RequestSample(nullptr) == MF_E_MEDIA_SOURCE_WRONGSTATE);
    return true;
}
bool SharedEffectsReachMf() {
    Fixture f; Frame source; source.Fill(40,80,160);
    PrismForge::Effects::DesktopColorPipeline effects;
    PrismForge::Effects::DesktopColorSettings settings; settings.look = PrismForge::Effects::Look::Mono; effects.Configure(settings);
    auto* output = effects.Process(source.value); CHECK(output != source.value && f.writer.WriteFrame(output));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    CHECK(std::abs(int(f.snapshot.data[128])-128) <= 1 && std::abs(int(f.snapshot.data[129])-128) <= 1);
    settings.look = PrismForge::Effects::Look::Original; effects.Configure(settings);
    CHECK(f.writer.WriteFrame(effects.Process(source.value))); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    CHECK(std::abs(int(f.snapshot.data[128])-128) > 5 || std::abs(int(f.snapshot.data[129])-128) > 5);
    const auto original = f.snapshot.data;
    std::string cube = "LUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b) for (int g = 0; g < 2; ++g) for (int r = 0; r < 2; ++r)
        cube += std::to_string(b)+" "+std::to_string(g)+" "+std::to_string(r)+"\n";
    settings.lut = PrismForge::Effects::CubeLut::Parse(cube); settings.intensity = 1;
    CHECK(settings.lut); effects.Configure(settings);
    CHECK(f.writer.WriteFrame(effects.Process(source.value))); CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK);
    const double luma = .2126*40 + .7152*80 + .0722*160;
    const int y = int(std::lround(16+219*luma/255));
    const int u = int(std::lround(128+224*(160-luma)/(255*2*(1-.0722))));
    const int v = int(std::lround(128+224*(40-luma)/(255*2*(1-.2126))));
    CHECK(Uniform(f.snapshot,y,u,v,2) && f.snapshot.data != original);
    settings.bypassed = true; effects.Configure(settings);
    CHECK(f.writer.WriteFrame(effects.Process(source.value)));
    CHECK(f.reader.GetLatestFrame(f.snapshot) == S_OK && f.snapshot.data == original);
    return true;
}
int main() {
    const auto co = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(co) || FAILED(MFStartup(MF_VERSION))) return 2;
    const std::pair<const char*,bool(*)()> tests[]{
        {"lazy startup",LazyStart}, {"bounded target geometry",OutputBounds}, {"invalid input leaves publication untouched",InvalidInputs},
        {"owned snapshot and repeated frame",OwnedSnapshot}, {"transactional geometry",GeometryTransaction},
        {"writer timeout and mutex ownership",WriterContention}, {"reader timeout",ReaderContention}, {"pause waits for fresh resume",PauseResume},
        {"contended stop/close and freshness",ContendedStopFreshness}, {"abandoned writer recovery",AbandonedWriter},
        {"abandoned reader discards partial frame",AbandonedReader}, {"truncated mapping",TruncatedMapping}, {"wrong mutex object cleanup/retry",WrongMutexObject},
        {"transactional namespace upgrade and sequence",UpgradeSequence}, {"missing upgrade mutex",UpgradeMissingMutex}, {"contended upgrade",UpgradeContention},
        {"invalid shared metadata",InvalidHeader}, {"independent limited BT709 encoding",Encoded709}, {"same-format range/matrix refresh",RangeMatrixRefresh},
        {"exact NV12 varied pixels/strides and immutable input",ExactNv12Strides},
        {"exact planar420 varied pixels/strides and immutable input",ExactPlanarStrides},
        {"alternating420 formats/ranges/matrices",Alternating420Metadata},
        {"exact-copy to aspect bars and back",ExactCopyToAspectBars},
        {"odd padded source and aspect fit",OddSourceFit}, {"concurrent whole-frame snapshots",ConcurrentWholeFrames},
        {"restart monotonic index and overflow",MonotonicRestartOverflow}, {"actual MF RequestSample and negotiated geometry",ActualMfSamples},
        {"shared desktop effects reach MF publication",SharedEffectsReachMf}
    };
    int passed=0;
    for (const auto& [name,test] : tests) {
        try { if (test()) { ++passed; std::cout << "PASS " << name << "\n"; } else std::cout << "FAIL " << name << "\n"; }
        catch (const std::exception& e) { std::cerr << "ERROR " << name << ": " << e.what() << "\n"; }
    }
    MFShutdown(); CoUninitialize(); std::cout << passed << "/" << std::size(tests) << " passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
