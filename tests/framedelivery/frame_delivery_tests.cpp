#include "video/latestframedelivery.h"
#include "capture/inputsession.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Delivery = PrismForge::Video::LatestFrameDelivery;
using namespace std::chrono_literals;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct FrameDelete { void operator()(AVFrame* frame) const { av_frame_free(&frame); } };
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
Frame MakeFrame(int64_t pts = 1, AVPixelFormat format = AV_PIX_FMT_BGR24) {
    Frame frame(av_frame_alloc()); Require(bool(frame), "frame allocation");
    frame->width = 7; frame->height = 5; frame->format = format;
    Require(av_frame_get_buffer(frame.get(), 32) == 0, "frame buffers");
    frame->pts = pts; frame->time_base = {1, 60000};
    frame->color_range = AVCOL_RANGE_JPEG; frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709; frame->color_trc = AVCOL_TRC_BT709;
    for (int p = 0; p < av_pix_fmt_count_planes(format); ++p) {
        auto* buf = av_frame_get_plane_buffer(frame.get(), p);
        if (buf) std::memset(buf->data, 37, buf->size);
    }
    return frame;
}
class Event {
public:
    void Signal() { std::lock_guard<std::mutex> lock(mutex); signaled = true; ready.notify_all(); }
    bool Wait(std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, timeout, [&] { return signaled; });
    }
private:
    std::mutex mutex; std::condition_variable ready; bool signaled = false;
};
// Declare this after Delivery: on assertion failure its destructor releases the
// callback before Delivery's destructor joins. No test leaves a held consumer.
class Gate {
public:
    ~Gate() { Release(); }
    void Block() { entered.Signal(); Require(released.Wait(5000ms), "held callback watchdog"); }
    void WaitEntered() { Require(entered.Wait(), "callback never entered"); }
    void Release() { released.Signal(); }
private:
    Event entered, released;
};
template<class Predicate> void Await(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::yield();
    }
}
void OwnedFrameAndMetadata() {
    Delivery queue; Gate gate; Event done; bool correct = false;
    auto source = MakeFrame(123);
    av_dict_set(&source->metadata, "current", "original", 0);
    auto* side = av_frame_new_side_data(source.get(), AV_FRAME_DATA_A53_CC, 3);
    Require(side != nullptr, "side data allocation"); side->data[0] = 61;
    Require(queue.Start(7, [&](uint64_t token, AVFrame* current, double wait) {
        gate.Block();
        auto* metadata = av_dict_get(current->metadata, "current", nullptr, 0);
        auto* capturedSide = av_frame_get_side_data(current, AV_FRAME_DATA_A53_CC);
        correct = token == 7 && current->pts == 123 && current->time_base.num == 1 &&
            current->time_base.den == 60000 && current->width == 7 && current->height == 5 &&
            current->data[0][0] == 37 && current->color_range == AVCOL_RANGE_JPEG &&
            current->colorspace == AVCOL_SPC_BT709 && current->color_primaries == AVCOL_PRI_BT709 &&
            metadata && std::string(metadata->value) == "original" && capturedSide &&
            capturedSide->data[0] == 61 && std::isfinite(wait) && wait >= 0;
        done.Signal();
    }), "start");
    Require(queue.Submit(7, source.get()), "submit"); gate.WaitEntered();
    Require(av_frame_make_writable(source.get()) == 0, "producer copy-on-write");
    source->data[0][0] = 0; av_dict_set(&source->metadata, "current", "new", 0);
    av_frame_remove_side_data(source.get(), AV_FRAME_DATA_A53_CC);
    av_frame_unref(source.get());
    gate.Release(); Require(done.Wait(), "owned callback completion"); queue.Stop();
    Require(correct, "pixels/current properties survived reuse/unref");
}
void BorrowedFrameIsCopied() {
    Delivery queue; Gate gate; Event done; bool correct = false;
    std::vector<uint8_t> bytes(32 * 5, 42);
    Frame source(av_frame_alloc()); source->width = 7; source->height = 5;
    source->format = AV_PIX_FMT_BGR24; source->data[0] = bytes.data(); source->linesize[0] = 32; source->pts = 9;
    Require(queue.Start(1, [&](uint64_t, AVFrame* current, double) {
        gate.Block(); correct = current->data[0][0] == 42 && current->buf[0] && current->pts == 9; done.Signal();
    }), "start");
    Require(queue.Submit(1, source.get()), "borrowed submit"); gate.WaitEntered();
    std::fill(bytes.begin(), bytes.end(), uint8_t(0)); source.reset();
    gate.Release(); Require(done.Wait(), "borrowed completion"); queue.Stop();
    Require(correct, "borrowed pixels copied into owned buffer");
}
void LatestPendingAndNonblockingProducer() {
    Delivery queue; Gate gate; Event done; std::vector<int64_t> delivered;
    Require(queue.Start(4, [&](uint64_t, AVFrame* current, double wait) {
        delivered.push_back(current->pts);
        if (current->pts == 1) gate.Block(); else { Require(wait >= 0, "queue wait"); done.Signal(); }
    }), "start");
    auto source = MakeFrame(); Require(queue.Submit(4, source.get()), "first"); gate.WaitEntered();
    auto producer = std::async(std::launch::async, [&] {
        for (int seq = 2; seq <= 5001; ++seq) { source->pts = seq; Require(queue.Submit(4, source.get()), "producer submit"); }
    });
    const bool completedWhileHeld = producer.wait_for(500ms) == std::future_status::ready;
    if (!completedWhileHeld) gate.Release();
    producer.get(); Require(completedWhileHeld, "producer waited behind held callback");
    auto stats = queue.GetStats();
    Require(stats.submitted == 5001 && stats.replaced == 4999 && stats.pending == 1 &&
        stats.inFlight == 1 && stats.maxOutstanding == 2, "exact single pending bound and replacement counters");
    gate.Release(); Require(done.Wait(), "latest pending completion"); queue.Stop();
    Require(delivered == std::vector<int64_t>({1, 5001}), "FIFO backlog discarded; newest pending follows active frame");
    Require(queue.GetStats().delivered == 2, "two deliveries");
}
void SourceGenerationPurgesAndRejects() {
    Delivery queue; Gate gate; Event done; std::vector<std::pair<uint64_t, int64_t>> delivered;
    Require(queue.Start(7, [&](uint64_t token, AVFrame* current, double) {
        delivered.emplace_back(token, current->pts);
        if (current->pts == 1) gate.Block(); else done.Signal();
    }), "start");
    auto source = MakeFrame(); Require(queue.Submit(7, source.get()), "first"); gate.WaitEntered();
    source->pts = 2; Require(queue.Submit(7, source.get()), "old pending"); queue.SetGeneration(8);
    Require(!queue.Submit(7, source.get()), "old source rejected"); source->pts = 99;
    Require(queue.Submit(8, source.get()), "new source accepted"); gate.Release();
    Require(done.Wait(), "new source completion"); queue.Stop();
    Require(delivered == std::vector<std::pair<uint64_t, int64_t>>({{7, 1}, {8, 99}}), "pending old source purged");
    const auto stats = queue.GetStats(); Require(stats.purged == 1 && stats.rejected == 1, "purge/reject counters");
}
void StopWaitsForActiveAndDropsPending() {
    Delivery queue; Gate gate; std::atomic<unsigned> called{0};
    Require(queue.Start(1, [&](uint64_t, AVFrame*, double) { ++called; gate.Block(); }), "start");
    auto source = MakeFrame(); Require(queue.Submit(1, source.get()), "first"); gate.WaitEntered();
    source->pts = 2; Require(queue.Submit(1, source.get()), "pending");
    auto stop = std::async(std::launch::async, [&] { queue.Stop(); });
    Await([&] { return queue.GetStats().purged == 1; }, "Stop did not cancel pending");
    Require(!queue.Submit(1, source.get()), "no accepting after Stop cancellation");
    const bool joinedEarly = stop.wait_for(30ms) == std::future_status::ready;
    gate.Release(); Require(stop.wait_for(3000ms) == std::future_status::ready, "Stop did not join"); stop.get();
    Require(!joinedEarly && called == 1, "Stop joins active callback and discards pending");
    Require(queue.GetStats().pending == 0 && queue.GetStats().inFlight == 0, "no work after Stop returns");
    queue.Stop();
}
void RestartResetsAndRejectsOldToken() {
    Delivery queue; Event first, second; unsigned value = 0;
    Require(queue.Start(1, [&](uint64_t, AVFrame*, double) { first.Signal(); }), "first start");
    auto source = MakeFrame(); Require(queue.Submit(1, source.get()), "first"); Require(first.Wait(), "first done");
    queue.Stop();
    Require(queue.Start(2, [&](uint64_t token, AVFrame* current, double) { value = unsigned(token + current->pts); second.Signal(); }), "restart");
    Require(queue.GetStats().submitted == 0, "stats reset on restart");
    Require(!queue.Submit(1, source.get()), "old token rejected after restart");
    source->pts = 40; Require(queue.Submit(2, source.get()), "new frame"); Require(second.Wait(), "new done"); queue.Stop();
    Require(value == 42 && queue.GetStats().delivered == 1 && queue.GetStats().rejected == 1, "restarted callback/current source");
}
void CallbackExceptionRecovery() {
    Delivery queue; Event done;
    Require(queue.Start(1, [&](uint64_t, AVFrame* current, double) {
        if (current->pts == 1) throw std::runtime_error("expected callback failure"); done.Signal();
    }), "start");
    auto source = MakeFrame(); Require(queue.Submit(1, source.get()), "first");
    Await([&] { return queue.GetStats().callbackErrors == 1; }, "exception not recorded");
    source->pts = 2; Require(queue.Submit(1, source.get()), "second"); Require(done.Wait(), "worker did not recover"); queue.Stop();
    Require(queue.GetStats().delivered == 2 && queue.GetStats().callbackErrors == 1, "exception contained and later frame delivered");
}
void SelfStopDefersExternalJoin() {
    Delivery queue; Event done; bool stopped = false, restartRejected = false;
    auto source = MakeFrame();
    Require(queue.Start(5, [&](uint64_t, AVFrame*, double) {
        queue.Stop(); stopped = !queue.Submit(5, source.get());
        restartRejected = !queue.Start(8, [](uint64_t, AVFrame*, double) {}); done.Signal();
    }), "start");
    Require(queue.Submit(5, source.get()), "first"); Require(done.Wait(), "callback Stop deadlocked"); queue.Stop();
    Require(stopped && restartRejected && queue.GetStats().inFlight == 0, "self Stop cancellation and external drain");
}
void ExternalAndSelfStopRace() {
    Delivery queue; Gate gate; Event done;
    Require(queue.Start(1, [&](uint64_t, AVFrame*, double) { gate.Block(); queue.Stop(); done.Signal(); }), "start");
    auto source = MakeFrame(); Require(queue.Submit(1, source.get()), "first"); gate.WaitEntered();
    source->pts = 2; Require(queue.Submit(1, source.get()), "pending");
    auto stop = std::async(std::launch::async, [&] { queue.Stop(); });
    Await([&] { return queue.GetStats().purged == 1; }, "external Stop request"); gate.Release();
    Require(done.Wait(), "self Stop blocked behind external join");
    Require(stop.wait_for(3000ms) == std::future_status::ready, "external/self Stop join deadlock"); stop.get();
    Require(queue.GetStats().delivered == 1, "single active delivered");
}
void InvalidFramesRejected() {
    Delivery queue; std::atomic<unsigned> called{0};
    Require(queue.Start(1, [&](uint64_t, AVFrame*, double) { ++called; }), "start");
    Require(!queue.Submit(1, nullptr), "null rejected");
    auto source = MakeFrame(); const int width = source->width;
    source->width = 0; Require(!queue.Submit(1, source.get()), "zero width rejected");
    source->width = 20000; Require(!queue.Submit(1, source.get()), "unbounded dimensions rejected"); source->width = width;
    const int pitch = source->linesize[0]; source->linesize[0] = 1;
    Require(!queue.Submit(1, source.get()), "small pitch rejected"); source->linesize[0] = pitch;
    source->height = 500; Require(!queue.Submit(1, source.get()), "owned plane span rejected"); source->height = 5;
    source->format = AV_PIX_FMT_D3D11; Require(!queue.Submit(1, source.get()), "GPU handle rejected"); source->format = AV_PIX_FMT_BGR24;
    auto* data = source->data[0]; source->data[0] = nullptr;
    Require(!queue.Submit(1, source.get()), "missing plane rejected"); source->data[0] = data;
    queue.Stop(); Require(called == 0 && queue.GetStats().rejected == 7, "invalid input cannot reach callback");
}
void NegativeStrideOwnedAndBorrowed() {
    for (bool borrowed : {false, true}) {
        Delivery queue; Gate gate; Event done; bool correct = false;
        auto source = MakeFrame();
        for (int y = 0; y < 5; ++y) std::memset(source->data[0] + y * source->linesize[0], y + 20, 21);
        Frame raw(av_frame_alloc()); AVFrame* submitted = source.get();
        if (borrowed) {
            raw->width = source->width; raw->height = source->height; raw->format = source->format;
            raw->data[0] = source->data[0]; raw->linesize[0] = source->linesize[0]; submitted = raw.get();
        }
        submitted->data[0] += 4 * submitted->linesize[0]; submitted->linesize[0] = -submitted->linesize[0];
        Require(queue.Start(1, [&](uint64_t, AVFrame* current, double) {
            gate.Block(); correct = current->data[0][0] == 24 && current->data[0][4 * current->linesize[0]] == 20; done.Signal();
        }), "start");
        Require(queue.Submit(1, submitted), "negative stride accepted"); gate.WaitEntered(); source.reset(); raw.reset();
        gate.Release(); Require(done.Wait(), "negative stride completion"); queue.Stop();
        Require(correct, "negative-stride row pixels safely retained/copied");
    }
}
void ConcurrentProducersBounded() {
    Delivery queue; Gate gate; Event done; std::vector<int64_t> delivered;
    Require(queue.Start(1, [&](uint64_t, AVFrame* current, double) {
        delivered.push_back(current->pts); if (current->pts == 1) gate.Block(); else done.Signal();
    }), "start");
    auto first = MakeFrame(); Require(queue.Submit(1, first.get()), "first"); gate.WaitEntered();
    std::vector<std::future<void>> producers;
    for (int p = 0; p < 8; ++p) producers.push_back(std::async(std::launch::async, [&, p] {
        auto source = MakeFrame();
        for (int i = 0; i < 500; ++i) { source->pts = 2 + p * 500 + i; queue.Submit(1, source.get()); }
    }));
    for (auto& producer : producers) producer.get();
    first->pts = 99999; Require(queue.Submit(1, first.get()), "last frame");
    const auto held = queue.GetStats();
    Require(held.maxOutstanding == 2 && held.pending == 1 && held.inFlight == 1 &&
        held.submitted + held.rejected == 4002 && held.replaced == held.submitted - 2,
        "concurrent submit storage/accounting bound");
    gate.Release(); Require(done.Wait(), "latest concurrent frame"); queue.Stop();
    Require(delivered == std::vector<int64_t>({1, 99999}), "latest final submission wins concurrent producers");
}
void DestructorJoinsAndPurges() {
    auto queue = std::make_unique<Delivery>(); Gate gate; std::atomic<unsigned> called{0};
    Require(queue->Start(1, [&](uint64_t, AVFrame*, double) { ++called; gate.Block(); }), "start");
    auto source = MakeFrame(); Require(queue->Submit(1, source.get()), "first"); gate.WaitEntered();
    source->pts = 2; Require(queue->Submit(1, source.get()), "pending");
    auto destroy = std::async(std::launch::async, [owner = std::move(queue)]() mutable { owner.reset(); });
    const bool destroyedEarly = destroy.wait_for(30ms) == std::future_status::ready;
    gate.Release(); Require(destroy.wait_for(3000ms) == std::future_status::ready, "destructor failed to join"); destroy.get();
    Require(!destroyedEarly && called == 1, "destructor waits active and purges pending");
}
void CallbackReentrancyAndGeneration() {
    Delivery queue; Event done; unsigned calls = 0; auto source = MakeFrame(22);
    Require(queue.Start(1, [&](uint64_t token, AVFrame*, double) {
        ++calls; auto stats = queue.GetStats(); Require(stats.inFlight == 1, "callback stats reentrant");
        if (token == 1) { queue.SetGeneration(2); Require(queue.Submit(2, source.get()), "callback submit reentrant"); }
        else done.Signal();
    }), "start");
    Require(queue.Submit(1, source.get()), "first"); Require(done.Wait(), "callback generation reentry deadlocked"); queue.Stop();
    Require(calls == 2 && queue.GetStats().delivered == 2, "callback reentrant generation+submit completes");
}
void RetainedCloneSurvivesStopAndOwner() {
    Frame retained; Event done;
    {
        Delivery queue;
        Require(queue.Start(0, [&](uint64_t token, AVFrame* current, double) {
            Require(token == 0, "zero token supported"); retained.reset(av_frame_clone(current)); done.Signal();
        }), "start");
        auto source = MakeFrame(87); Require(queue.Submit(0, source.get()), "first"); source.reset();
        Require(done.Wait(), "retained completion"); queue.Stop();
    }
    Require(retained && retained->pts == 87 && retained->data[0][0] == 37, "consumer retained clone owns pixels");
}
void MixedBorrowedPlanesRejected() {
    Delivery queue; Require(queue.Start(1, [](uint64_t, AVFrame*, double) {}), "start");
    auto source = MakeFrame(1, AV_PIX_FMT_YUV420P); std::vector<uint8_t> borrowed(32 * 3, 128);
    source->data[1] = borrowed.data(); source->linesize[1] = 32;
    Require(!queue.Submit(1, source.get()), "partially borrowed planes cannot shallow-reference dangling data"); queue.Stop();
    Require(queue.GetStats().rejected == 1, "mixed-plane invalid count");
}
void EmptyCallbackAndInactiveSubmission() {
    Delivery queue; auto source = MakeFrame(); Require(!queue.Submit(1, source.get()), "inactive rejected");
    Require(!queue.Start(1, {}), "empty callback rejected"); Require(!queue.Submit(1, source.get()), "failed Start remains inactive");
    Event done; Require(queue.Start(1, [&](uint64_t, AVFrame*, double) { done.Signal(); }), "later valid Start");
    Require(queue.Submit(1, source.get()), "later submit"); Require(done.Wait(), "later callback"); queue.Stop();
}
void SessionGateIntegrationSourceSwitch() {
    PrismForge::Input::SessionGate session;
    Delivery queue; Gate oldActive, oldDequeued; Event newPublished, switchingEntered;
    const auto oldToken = session.Current();
    std::vector<std::pair<uint64_t, int64_t>> published;
    Frame currentOutput;
    Require(queue.Start(oldToken, [&](uint64_t token, AVFrame* frame, double) {
        // An old pending frame may be dequeued just as Advance wins the source
        // gate. Hold that race outside the gate; application gate rejects it.
        if (token == oldToken && frame->pts == 2) oldDequeued.Block();
        session.Deliver(token, [&] {
            if (frame->pts == 1) oldActive.Block();
            published.emplace_back(token, frame->pts);
            currentOutput.reset(av_frame_clone(frame));
            if (frame->pts == 99) newPublished.Signal();
        });
    }), "integration start");
    auto source = MakeFrame(1); Require(queue.Submit(oldToken, source.get()), "old active submit"); oldActive.WaitEntered();
    source->pts = 2; Require(queue.Submit(oldToken, source.get()), "old pending submit");
    auto switching = std::async(std::launch::async, [&] {
        switchingEntered.Signal();
        const auto token = session.Advance();
        queue.SetGeneration(token);
        return token;
    });
    Require(switchingEntered.Wait(), "switch worker entry");
    const bool waitsForOld = switching.wait_for(0ms) != std::future_status::ready;
    oldActive.Release();
    Require(switching.wait_for(3000ms) == std::future_status::ready, "source gate did not drain old active frame");
    const auto newToken = switching.get();
    Require(waitsForOld && newToken != oldToken, "Advance drains current publication before switch returns");
    Require(!queue.Submit(oldToken, source.get()), "late old source cannot requeue after switch");
    Require(!session.Deliver(oldToken, [&] { published.emplace_back(oldToken, 777); }), "already dequeued old source cannot publish");
    auto next = MakeFrame(99); next->data[0][0] = 99;
    Require(queue.Submit(newToken, next.get()), "new source submit"); next.reset();
    oldDequeued.Release(); Require(newPublished.Wait(), "new current source failed to publish"); queue.Stop();
    Require(published == std::vector<std::pair<uint64_t, int64_t>>({{oldToken, 1}, {newToken, 99}}),
        "pending/late old frames cannot publish after switch");
    Require(currentOutput && currentOutput->pts == 99 && currentOutput->data[0][0] == 99,
        "owned current new-source output reaches shared publisher");
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--force-failure") { std::cerr << "Expected exit propagation probe\n"; return 19; }
    if (argc > 1 && std::string(argv[1]) == "--watchdog-probe") { std::this_thread::sleep_for(60s); return 0; }
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"Owned pixels/current metadata survive COW+reuse+unref", OwnedFrameAndMetadata},
        {"Fully borrowed pixels copied into owned frame", BorrowedFrameIsCopied},
        {"Held consumer cannot block producer; newest pending replaces FIFO", LatestPendingAndNonblockingProducer},
        {"Source generation purges old pending and rejects old input", SourceGenerationPurgesAndRejects},
        {"Stop joins active callback, drops pending, rejects new input", StopWaitsForActiveAndDropsPending},
        {"Restart resets stats and rejects prior token", RestartResetsAndRejectsOldToken},
        {"Callback exception counted, worker recovers on next frame", CallbackExceptionRecovery},
        {"Callback Stop defers join, rejects self restart", SelfStopDefersExternalJoin},
        {"Concurrent external and callback Stop cannot self-join/deadlock", ExternalAndSelfStopRace},
        {"Malformed/oversized/hardware/unsafe plane inputs rejected", InvalidFramesRejected},
        {"Negative stride owned and borrowed rows retained correctly", NegativeStrideOwnedAndBorrowed},
        {"Eight concurrent producers maintain one pending and exact counters", ConcurrentProducersBounded},
        {"Destructor joins active callback and drops pending", DestructorJoinsAndPurges},
        {"Callback stats/generation/Submit run outside queue lock", CallbackReentrancyAndGeneration},
        {"Retained consumer clone survives zero-token queue destruction", RetainedCloneSurvivesStopAndOwner},
        {"Mixed reference/borrowed planes rejected before shallow clone", MixedBorrowedPlanesRejected},
        {"Inactive/empty callback rejected; later Start works", EmptyCallbackAndInactiveSubmission},
        {"Actual SessionGate+queue drains old source and publishes only current output", SessionGateIntegrationSourceSwitch}
    };
    unsigned passed = 0;
    for (const auto& test : tests) {
        try { test.run(); ++passed; std::cout << "[PASS] " << test.name << '\n'; }
        catch (const std::exception& error) { std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n'; }
        catch (...) { std::cerr << "[FAIL] " << test.name << ": unknown exception\n"; }
    }
    std::cout << passed << '/' << std::size(tests) << " frame delivery regressions passed\n";
    return passed == std::size(tests) ? 0 : 1;
}