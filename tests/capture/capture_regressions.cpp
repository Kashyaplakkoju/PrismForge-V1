#include <winsock2.h>
#include <condition_variable>
#include "rtsp/receiver.h"
#include "logger.h"
Logger logger(Logger::DISABLED);
#include "capture/systemcamera.h"
#include "capture/inputsession.h"
#include "capture/sample_converter.h"
#include "capture/mf_async_reader.h"
#include "capture/mf_sample_lock.h"
#include <cstring>
#include "effects/framepipeline.h"
#include "video/directshowscaler.h"
#include <future>
#include <iostream>
#include <climits>
#include <chrono>
#include <stdexcept>
#include <thread>
using namespace PrismForge::Capture;
using namespace std::chrono_literals;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Wait(F predicate) {
    auto limit = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(1ms);
    Require(predicate(), "event deadline exceeded");
}
CaptureDevice Device(bool dual = false) {
    CaptureDevice d; d.id = "fake_camera"; d.displayName = "Test camera";
    d.routes.push_back({CaptureBackend::MediaFoundation, "fake-mf", "MF"});
    if (dual) d.routes.push_back({CaptureBackend::DirectShow, "fake-ds", "DS"});
    return d;
}
class Fake final : public SystemCameraSource::ICaptureBackend {
    SystemCameraSource::FrameCallback frame;
    SystemCameraSource::StatusCallback status;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    bool earlyFailure;
    CaptureErrorCategory earlyCategory;
public:
    Fake(SystemCameraSource::FrameCallback f, SystemCameraSource::StatusCallback s, bool early = false, CaptureErrorCategory category = CaptureErrorCategory::UnsupportedFormat)
        : frame(std::move(f)), status(std::move(s)), earlyFailure(early), earlyCategory(category) {}
    ~Fake() override { Close(); }
    bool Open(const DeviceRoute&, const CaptureMode*) override {
        if (earlyFailure) {
            CaptureStatus error; error.state = CaptureState::Error;
            error.errorCategory = earlyCategory;
            status(error); // Before Open returns, not after an arbitrary sleep.
            return true;
        }
        worker = std::thread([this] {
            CaptureStatus s; s.state = CaptureState::Running;
            s.nativeMode = s.deliveredMode = {8, 4, 30, 1, "RGB24", false}; status(s);
            AVFrame* f = av_frame_alloc();
            SampleConverter::EnsureFrameBuffer(f, 8, 4, AV_PIX_FMT_BGR24);
            int sequence = 0;
            while (!cancelled) {
                ++sequence;
                for (int y=0;y<4;++y) for (int x=0;x<8;++x) {
                    uint8_t* p=f->data[0]+y*f->linesize[0]+3*x;
                    p[0]=40+sequence%50; p[1]=80; p[2]=120;
                }
                f->pts=sequence*333333; f->time_base={1,10000000}; f->color_range=AVCOL_RANGE_JPEG;
                frame(f); std::this_thread::sleep_for(1ms);
            }
            av_frame_free(&f);
        });
        return true;
    }
    void Cancel() override { cancelled = true; }
    void Close() override { Cancel(); if (worker.joinable()) worker.join(); }
    CaptureMode GetNativeMode() const override { return {8, 4, 30, 1, "RGB24", false}; }
    CaptureMode GetDeliveredMode() const override { return GetNativeMode(); }
    bool IsWorkerThread() const override { return worker.get_id() == std::this_thread::get_id(); }
};
void Factory(SystemCameraSource& source) {
    source.SetBackendFactoryForTesting([](const DeviceRoute&, const CaptureMode*, auto f, auto s) {
        return std::make_unique<Fake>(f, s);
    });
}
int main() {
    int passed = 0;
    auto test = [&](const char* name, auto run) {
        run(); ++passed; std::cout << "PASS " << name << std::endl;
    };
    try {
        test("catalogue identity and output exclusion", [] {
            Require(SystemCameraCatalog::NormalizeDevicePath("root#media#0002") == "root#media#0002", "instance lost");
            Require(SystemCameraCatalog::IsPrismForgeOutput("{D732B8C1-03D7-4D59-AF8E-4CB72B58B940}"), "DirectShow feedback exclusion");
            Require(SystemCameraCatalog::IsPrismForgeOutput("@device:sw:{2141c943-dd09-4915-bd14-264724dba955}"), "MF identifier feedback exclusion");
            for (const auto* name : {"PrismForge Virtual Camera", "PrismForge Virtual Camera (Windows Virtual Camera)",
                                    "pRiSmFoRgE vIrTuAl CaMeRa", "PRISMFORGE VIRTUAL CAMERA (WINDOWS VIRTUAL CAMERA)"}) {
                Require(SystemCameraCatalog::IsPrismForgeOutput(name), "friendly-name feedback exclusion");
            }
            for (const auto* name : {"", "PrismForge named real webcam", "PrismForge USB Camera", "PrismForge Virtual Camera Pro",
                                    "My PrismForge Virtual Camera", "PrismForge Virtual Camera (Windows Virtual Camera) Pro",
                                    "PrismForge Virtual Camera ", "VCamdroid", "VCamdroid Virtual Camera", "DroidCam", "DroidCam Video",
                                    "OBS Virtual Camera"}) {
                Require(!SystemCameraCatalog::IsPrismForgeOutput(name), "friendly-name false exclusion");
            }
        });
        test("signed padded RGB, immutable input, integer bounds", [] {
            uint8_t data[24] = {1,2,3,4,5,6,0,0,7,8,9,10,11,12,0,0};
            AVFrame* f = av_frame_alloc();
            Require(SampleConverter::ConvertRgb24ToFrame(data, 16, 2, 2, -8, false, f), "negative pitch failed");
            Require(f->data[0][0] == 7 && f->data[0][f->linesize[0]] == 1 && data[0] == 1, "row orientation");
            Require(!SampleConverter::ConvertRgb24ToFrame(data, 16, 2, 2, INT_MIN, false, f), "INT_MIN pitch accepted");
            Require(!SampleConverter::ConvertRgb32ToFrame(data, 16, INT_MAX, 2, 0, false, f), "overflow dimensions accepted");
            Require(!SampleConverter::ConvertRgb24ToFrame(data, 4, 2, 2, 8, false, f), "undersized sample accepted");
            Require(f->colorspace == AVCOL_SPC_UNSPECIFIED, "unknown matrix invented");
            av_frame_free(&f);
        });
        test("MF actual async callback cancellation", [] {
            auto state = std::make_shared<MfReadState>();
            auto* callback = new MfReaderCallback(state);
            HRESULT hr = S_OK; DWORD flags = 0; LONGLONG pts = 0; IMFSample* sample = nullptr;
            callback->OnReadSample(E_ACCESSDENIED, 0, 0, 123, nullptr);
            Require(state->Take(hr, flags, pts, sample) && hr == E_ACCESSDENIED && pts == 123, "read result not preserved");
            auto pending = std::async(std::launch::async, [&] { return state->Take(hr, flags, pts, sample); });
            state->Cancel();
            Require(pending.wait_for(500ms) == std::future_status::ready && !pending.get(), "pending read not cancelled");
            callback->OnReadSample(S_OK, 0, 0, 999, nullptr);
            Require(!state->available, "late read revived cancelled state"); callback->Release();
        });
        test("early route failure and output processing", [] {
            std::atomic<int> frames{0}, routes{0};
            SystemCameraSource source;
            PrismForge::Effects::FramePipeline pipeline;
            pipeline.SetLook(PrismForge::Effects::Look::Warm);
            Video::DirectShowScaler scaler(8, 4);
            source.SetFrameCallback([&](AVFrame* f) {
                const int blue = f->data[0][0]; const auto pts=f->pts;
                AVFrame* out = pipeline.Process(f); int w = 0, h = 0;
                Require(out && out->data[0][0]==blue-12 && out->data[0][1]==84 && out->data[0][2]==134,
                    "camera warm effect pixels incorrect");
                Require(f->data[0][0]==blue && f->data[0][1]==80 && f->data[0][2]==120 && out->pts==pts,
                    "input modified or timing metadata lost");
                const uint8_t* delivered = scaler.Process(out, w, h);
                Require(delivered && w == 8 && h == 4 && delivered[0]==blue-12 && delivered[1]==84 && delivered[2]==134,
                    "processed output pixels differ"); ++frames;
            });
            source.SetBackendFactoryForTesting([&](const DeviceRoute& route, const CaptureMode*, auto f, auto s) {
                ++routes; return std::make_unique<Fake>(f, s, route.backend == CaptureBackend::MediaFoundation);
            });
            Require(source.Start(Device(true)), "start rejected"); Wait([&] { return frames >= 3; });
            Require(source.GetStatus().nativeMode.width == 8 && source.GetStatus().deliveredMode.height == 4, "negotiated mode lost by coalescing");
            source.Stop(); const int count = frames; std::this_thread::sleep_for(10ms);
            Require(frames == count && routes == 2 && source.GetState() == CaptureState::Idle, "fallback or drain failed");
        });
        test("frame callback stop and rejected restart", [] {
            std::atomic<bool> called{false}, restart{true};
            SystemCameraSource source; Factory(source);
            source.SetFrameCallback([&](AVFrame*) { if (!called.exchange(true)) { restart = source.Start(Device()); source.Stop(); } });
            Require(source.Start(Device()), "start failed"); Wait([&] { return called && source.GetState() == CaptureState::Idle; });
            Require(!restart, "callback restart accepted"); source.Stop();
        });
        test("status stop reentrancy and idempotence", [] {
            std::atomic<int> callbacks{0}; SystemCameraSource source; Factory(source);
            source.SetStatusCallback([&](const CaptureStatus& s) { ++callbacks; if (s.state == CaptureState::Starting || s.state == CaptureState::Idle) source.Stop(); });
            source.Start(Device()); source.Stop();
            Require(source.GetState() == CaptureState::Idle && callbacks < 10, "recursive status loop");
        });
        test("concurrent start/stop serialized and drained", [] {
            std::atomic<int> frames{0}; SystemCameraSource source; Factory(source);
            source.SetFrameCallback([&](AVFrame*) { ++frames; });
            auto a = std::async(std::launch::async, [&] { for (int i=0; i<30; ++i) source.Start(Device()); });
            auto b = std::async(std::launch::async, [&] { for (int i=0; i<30; ++i) source.Stop(); });
            a.get(); b.get(); source.Stop(); int before = frames; std::this_thread::sleep_for(10ms);
            Require(source.GetState() == CaptureState::Idle && frames == before, "late delivery after stop");
        });
        test("consumer exception is a terminal diagnostic", [] {
            SystemCameraSource source; Factory(source);
            source.SetFrameCallback([](AVFrame*) { throw std::runtime_error("fixture"); });
            Require(source.Start(Device()), "start failed"); Wait([&] { return source.GetState() == CaptureState::Error; });
            Require(source.GetStatus().errorMessage.find("consumer") != std::string::npos, "exception swallowed"); source.Stop();
        });
        test("bounded 2D scanline origins and minimal padding", [] {
            uint8_t bytes[32] = {}; MfSampleView view;
            CaptureMode mode{2,2,30,1,"RGB24",false};
            bytes[3] = 10; bytes[11] = 20;
            Require(MakeMfSampleView(bytes+11, bytes, 17, -8, mode, view), "valid negative 2D view rejected");
            Require(view.data == bytes+3 && view.size == 14 && view.pitch == -8, "incorrect lowest row normalization");
            AVFrame* frame = av_frame_alloc();
            Require(SampleConverter::ConvertRgb24ToFrame(view.data, view.size, 2,2,view.pitch,false,frame), "minimal last-row padding rejected");
            Require(frame->data[0][0] == 20 && frame->data[0][frame->linesize[0]] == 10, "incorrect 2D orientation");
            Require(!MakeMfSampleView(bytes+3, bytes,32,-8,mode,view), "negative origin underflow accepted");
            Require(!MakeMfSampleView(bytes+11,bytes,16,-8,mode,view), "2D short allocation accepted");
            Require(!MakeMfSampleView(bytes,bytes,32,INT_MIN,mode,view), "2D pitch overflow accepted");
            mode.pixelFormat="NV12";
            Require(!MakeMfSampleView(bytes+8,bytes,32,-8,mode,view), "unsupported negative NV12 layout accepted");
            Require(MakeMfSampleView(bytes+2,bytes,12,4,mode,view), "valid padded NV12 rejected");
            Require(SampleConverter::ConvertNv12ToFrame(view.data,view.size,2,2,4,4,frame), "NV12 minimal padding failed");
            Require(!SampleConverter::ConvertYuy2ToFrame(bytes,32,3,2,8,frame), "odd YUY2 accepted");
            av_frame_free(&frame);
        });
        test("native Windows MF top-down and bottom-up 2D buffers", [] {
            Require(SUCCEEDED(MFStartup(MF_VERSION)), "MFStartup failed");
            for (BOOL bottomUp : {FALSE, TRUE}) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
                Require(SUCCEEDED(MFCreate2DMediaBuffer(2,2,MFVideoFormat_RGB32.Data1,bottomUp,buffer.GetAddressOf())), "MFCreate2DMediaBuffer failed");
                Microsoft::WRL::ComPtr<IMF2DBuffer> surface;
                Require(SUCCEEDED(buffer.As(&surface)), "native 2D interface missing");
                BYTE* row = nullptr; LONG pitch = 0;
                Require(SUCCEEDED(surface->Lock2D(&row,&pitch)), "native write lock failed");
                row[0]=41; (row+pitch)[0]=82;
                surface->Unlock2D();
                AVFrame* frame = av_frame_alloc();
                {
                    MfSampleLock lock;
                    Require(lock.Lock(buffer.Get(),{2,2,30,1,"RGB32",false},8,false), "native bounded read lock failed");
                    Require(SampleConverter::ConvertRgb32ToFrame(lock.view.data,lock.view.size,2,2,lock.view.pitch,false,frame), "native buffer conversion failed");
                    Require(frame->data[0][0]==41 && frame->data[0][frame->linesize[0]]==82, "native top/bottom orientation changed");
                }
                Require(SUCCEEDED(surface->Lock2D(&row,&pitch)), "matching unlock missing"); surface->Unlock2D();
                av_frame_free(&frame);
            }
            MFShutdown();
        });
        test("native linear MF signed stride and short-buffer rejection", [] {
            Require(SUCCEEDED(MFStartup(MF_VERSION)), "MFStartup failed");
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            Require(SUCCEEDED(MFCreateMemoryBuffer(16,buffer.GetAddressOf())), "memory buffer failed");
            BYTE* data=nullptr; DWORD maximum=0,current=0;
            buffer->Lock(&data,&maximum,&current); std::memset(data,0,16); data[0]=12; data[8]=24; buffer->Unlock(); buffer->SetCurrentLength(14);
            AVFrame* frame=av_frame_alloc();
            {
                MfSampleLock lock; Require(lock.Lock(buffer.Get(),{2,2,30,1,"RGB24",false},-8,false), "linear stride failed");
                Require(SampleConverter::ConvertRgb24ToFrame(lock.view.data,lock.view.size,2,2,lock.view.pitch,false,frame), "linear sample failed");
                Require(frame->data[0][0]==24 && frame->data[0][frame->linesize[0]]==12, "linear rows changed");
                lock.Unlock();
            }
            buffer->SetCurrentLength(13);
            { MfSampleLock lock; Require(!lock.Lock(buffer.Get(),{2,2,30,1,"RGB24",false},-8,false), "short native linear sample accepted"); }
            Require(SUCCEEDED(buffer->Lock(&data,&maximum,&current)), "failed validation leaked lock"); buffer->Unlock();
            av_frame_free(&frame); buffer.Reset(); MFShutdown();
        });
        test("privacy and removal do not trigger route fallback", [] {
            for (auto category : {CaptureErrorCategory::PrivacyAccessDenied, CaptureErrorCategory::DeviceRemoved}) {
                SystemCameraSource source; std::atomic<int> routes{0}, frames{0};
                source.SetFrameCallback([&](AVFrame*) { ++frames; });
                source.SetBackendFactoryForTesting([&](const DeviceRoute&,const CaptureMode*,auto f,auto s) {
                    ++routes; return std::make_unique<Fake>(f,s,true,category);
                });
                Require(source.Start(Device(true)), "async activation rejected");
                Wait([&] {return source.GetState()==CaptureState::Error;});
                Require(source.GetStatus().errorCategory==category && routes==1 && frames==0, "terminal route error was bypassed");
                source.Stop();
            }
        });
        test("catalogue seam reentrancy and case-insensitive identity", [] {
            Require(SystemCameraCatalog::NormalizeDevicePath(R"(@DEVICE:PNP:\\?\USB#VID_1234#UNIT2#{1234})")=="usb#vid_1234#unit2", "case-sensitive identity prefix");
            SystemCameraCatalog::SetCustomEnumeratorForTesting([] {
                SystemCameraCatalog::SetCustomEnumeratorForTesting(nullptr);
                EnumerationReport report; report.devices.push_back(Device()); return report;
            });
            Require(SystemCameraCatalog::Enumerate().devices.size()==1, "catalogue callback under lock");
        });
        test("starting-state stop never opens a backend", [] {
            SystemCameraSource source; std::atomic<int> routes{0};
            source.SetBackendFactoryForTesting([&](const DeviceRoute&,const CaptureMode*,auto f,auto s) {
                ++routes; return std::make_unique<Fake>(f,s);
            });
            source.SetStatusCallback([&](const CaptureStatus& s) { if (s.state==CaptureState::Starting) source.Stop(); });
            Require(!source.Start(Device(true)), "cancelled activation accepted"); source.Stop();
            Require(routes==0 && source.GetState()==CaptureState::Idle, "driver activated after starting cancellation");
        });
        test("source switch drains in-flight output and suppresses delayed frames", [] {
            PrismForge::Input::SessionGate gate;
            const auto oldToken=gate.Current();
            std::promise<void> entered, release, switchEntered;
            auto releaseSignal=release.get_future().share();
            int oldFrames=0, newFrames=0;
            auto delivery=std::async(std::launch::async,[&] {
                return gate.Deliver(oldToken,[&] { entered.set_value(); releaseSignal.wait(); ++oldFrames; });
            });
            entered.get_future().wait();
            auto switching=std::async(std::launch::async,[&] { switchEntered.set_value(); return gate.Advance(); });
            switchEntered.get_future().wait();
            const bool drained=switching.wait_for(0ms)!=std::future_status::ready;
            release.set_value();
            const auto newToken=switching.get(); const bool delivered=delivery.get();
            Require(drained && delivered && oldFrames==1 && newToken!=oldToken,"source switch raced old output");
            Require(!gate.Deliver(oldToken,[&] {++oldFrames;}) && oldFrames==1,"late source output delivered");
            Require(gate.Deliver(newToken,[&] {++newFrames;}) && newFrames==1,"replacement source output rejected");
            Require(!gate.IsCurrent(oldToken) && gate.IsCurrent(newToken),"old statistics generation retained");
        });
        test("single output owner under simultaneous camera deliveries", [] {
            PrismForge::Input::SessionGate gate;
            PrismForge::Effects::FramePipeline pipeline; pipeline.SetLook(PrismForge::Effects::Look::Warm);
            Video::DirectShowScaler scaler(8,4);
            std::promise<void> start; auto startSignal=start.get_future().share();
            std::vector<std::future<void>> workers;
            int outputs=0;
            for (int camera=0;camera<8;++camera) workers.push_back(std::async(std::launch::async,[&,camera] {
                AVFrame* f=av_frame_alloc(); Require(f!=nullptr,"frame allocation failed");
                Require(SampleConverter::EnsureFrameBuffer(f,8,4,AV_PIX_FMT_BGR24),"frame buffer failed");
                startSignal.wait();
                for (int sequence=0;sequence<32;++sequence) {
                    const uint8_t blue=40+camera+sequence;
                    for (int y=0;y<4;++y) for (int x=0;x<8;++x) {
                        auto p=f->data[0]+y*f->linesize[0]+3*x; p[0]=blue; p[1]=80; p[2]=120;
                    }
                    f->pts=camera*1000+sequence; f->color_range=AVCOL_RANGE_JPEG;
                    Require(gate.Deliver(gate.Current(),[&] {
                        auto out=pipeline.Process(f); int width=0,height=0;
                        auto data=scaler.Process(out,width,height);
                        Require(data && width==8 && height==4 && data[0]==blue-12 && data[1]==84 && data[2]==134 && out->pts==f->pts,
                            "concurrent sources corrupted shared output");
                        ++outputs;
                    }),"current delivery rejected");
                }
                av_frame_free(&f);
            }));
            start.set_value(); for (auto& worker:workers) worker.get();
            Require(outputs==8*32,"missing concurrent output frames");
        });
        test("stable selection survives reordered and identically named camera rows", [] {
            using namespace PrismForge::Input;
            const auto usb=MobileKey("Camera","rtsp://127.0.0.1:7968");
            const auto wifi=MobileKey("Camera","rtsp://192.0.2.10:7968");
            Require(usb!=wifi,"duplicate phone names collapsed");
            std::vector<Choice> rows={{Kind::None,"","None"},{Kind::Heading,"","Mobile cameras"},
                {Kind::Mobile,usb,"Camera"},{Kind::Heading,"","System cameras"},
                {Kind::System,"system:first","Camera"},{Kind::System,"system:second","Camera"}};
            Require(FindChoice(rows,"system:second")==5,"system selection uses name or phone index");
            rows.insert(rows.begin()+2,{Kind::Mobile,wifi,"Camera"});
            Require(FindChoice(rows,"system:second")==6 && FindChoice(rows,usb)==3,"new phone stole stable selection");
            rows.erase(rows.begin()+2);
            Require(FindChoice(rows,"system:second")==5 && FindChoice(rows,"")==0 && FindChoice(rows,"system:removed")==0,
                "removed source or headings became selected");
        });
        test("phone receiver stop interrupts a stalled local RTSP handshake", [] {
            WSADATA wsa{};
            Require(WSAStartup(MAKEWORD(2,2),&wsa)==0,"Winsock startup failed");
            struct Peer {
                SOCKET listener=INVALID_SOCKET;
                std::mutex mutex; std::condition_variable cv; bool done=false;
                std::thread worker;
                ~Peer() {
                    { std::lock_guard<std::mutex> lock(mutex); done=true; } cv.notify_all();
                    if (worker.joinable()) worker.join();
                    if (listener!=INVALID_SOCKET) closesocket(listener);
                    WSACleanup();
                }
            } peer;
            peer.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            Require(peer.listener!=INVALID_SOCKET,"local test socket failed");
            sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            Require(bind(peer.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(peer.listener,1)==0,"local test listener failed");
            int length=sizeof(address);
            Require(getsockname(peer.listener,reinterpret_cast<sockaddr*>(&address),&length)==0,"local test endpoint failed");
            std::promise<bool> request; auto requested=request.get_future();
            peer.worker=std::thread([&] {
                fd_set readable; FD_ZERO(&readable); FD_SET(peer.listener,&readable); timeval timeout{3,0};
                if (select(0,&readable,nullptr,nullptr,&timeout)<=0) {request.set_value(false); return;}
                auto client=accept(peer.listener,nullptr,nullptr);
                if (client==INVALID_SOCKET) {request.set_value(false); return;}
                DWORD deadline=3000; setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&deadline),sizeof(deadline));
                char bytes[4096]; const int received=recv(client,bytes,sizeof(bytes),0);
                request.set_value(received>0);
                if (received>0) {
                    // Hold a real accepted request without returning a response.
                    std::unique_lock<std::mutex> lock(peer.mutex); peer.cv.wait(lock,[&] {return peer.done;});
                }
                closesocket(client);
            });
            std::atomic<int> frames{0}, stats{0};
            RTSP::Receiver receiver([&](AVFrame*) {++frames;},[&](const auto&) {++stats;});
            receiver.Start("rtsp://127.0.0.1:"+std::to_string(ntohs(address.sin_port))+"/owned-cancellation-test","tcp",8,4);
            Require(requested.wait_for(4s)==std::future_status::ready && requested.get(),"RTSP request never reached owned listener");
            const auto started=std::chrono::steady_clock::now(); receiver.Stop();
            Require(std::chrono::steady_clock::now()-started<1500ms,"stalled phone receiver prevented source switch");
            Require(frames==0 && stats==0,"stalled handshake emitted source output");
        });
        test("phone receiver immediate stop and restart always joins", [] {
            RTSP::Receiver receiver({},{});
            const auto started=std::chrono::steady_clock::now();
            for (int attempt=0;attempt<16;++attempt) {
                // Unknown protocol: no external endpoint or camera is opened.
                receiver.Start("prismforge-invalid-protocol:owned-test","tcp",8,4);
                receiver.Stop(); receiver.Stop();
            }
            Require(std::chrono::steady_clock::now()-started<3s,"immediate stop/restart retained a worker");
        });
        test("native MF/DirectShow enumeration without opening cameras", [] {
            const auto report = SystemCameraCatalog::Enumerate();
            Require(report.backendResults.size()==2, "native enumeration did not cover both backends");
            for (const auto& result : report.backendResults) {
                std::cout << "ENUM " << CaptureBackendToString(result.backend) << ": " << result.deviceCount << " success=" << result.success << std::endl;
                Require(result.success, "native enumeration failed");
            }
            for (const auto& d : report.devices) {
                std::cout << "DEVICE " << d.displayName << " routes=" << d.routes.size() << std::endl;
                Require(!SystemCameraCatalog::IsPrismForgeOutput(d.displayName), "recursive output friendly name");
                Require(!SystemCameraCatalog::IsPrismForgeOutput(d.devicePath), "recursive output device path");
                for (const auto& r : d.routes) Require(!SystemCameraCatalog::IsPrismForgeOutput(r.routeId), "recursive output input");
            }
            std::cout << "LIVE CAPTURE NOT RUN: enumeration is not streaming verification" << std::endl;
        });
        std::cout << "RESULT " << passed << "/20 PASS" << std::endl;
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
