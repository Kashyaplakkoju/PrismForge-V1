#include "net/server.h"
#include "net/serializer.h"
#include "logger.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>
Logger logger(Logger::DISABLED);
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while(false)
struct Listener : Server::ConnectionListener {
    mutable std::mutex mutex;
    mutable std::condition_variable wake;
    mutable unsigned connected=0,disconnected=0;
    void OnDeviceConnected(DeviceDescriptor& descriptor) const override {
        CHECK(descriptor.name()=="test-phone");CHECK(descriptor.protocol()=="tcp");
        { std::lock_guard<std::mutex> lock(mutex);++connected; } wake.notify_all();
    }
    void OnDeviceDisconnected(DeviceDescriptor&) const override {
        { std::lock_guard<std::mutex> lock(mutex);++disconnected; } wake.notify_all();
    }
    void OnDeviceErrorReported(DeviceDescriptor&,const Connection::ErrorReport&) const override {}
    bool Wait(unsigned target) const {
        std::unique_lock<std::mutex> lock(mutex);
        return wake.wait_for(lock,std::chrono::seconds(2),[&] { return connected>=target; });
    }
};
PrismForge::Usb::CommandResult Offline(const PrismForge::Usb::Command&,const std::atomic_bool&) {
    return {false,"device offline",false};
}
std::vector<uint8_t> Descriptor() {
    std::vector<uint8_t> result;
    auto word=[&](unsigned value) { result.push_back(uint8_t(value>>8));result.push_back(uint8_t(value)); };
    auto string=[&](const std::string& value) { word(unsigned(value.size()));result.insert(result.end(),value.begin(),value.end()); };
    string("test-phone");string("rtsp://127.0.0.1:8555/live");
    word(1);word(1920);word(1080);word(0);word(1);string("Warm");result.push_back(Video::Filter::EFFECT);
    return result;
}
struct Fixture {
    Listener listener;
    Server server{0,listener,Offline};
    asio::io_context clientContext;
    asio::ip::tcp::socket Connect() {
        asio::ip::tcp::socket socket(clientContext);
        socket.connect({asio::ip::make_address("127.0.0.1"),server.BoundPort()});return socket;
    }
    Fixture() { server.Start(); }
};
void ParserPrefixes() {
    const auto bytes=Descriptor();
    for (size_t size=0;size<bytes.size();++size) {
        std::optional<DeviceDescriptor> result;size_t consumed=99;
        CHECK(Serializer::ReadDescriptor(bytes.data(),size,result,consumed)==Serializer::DescriptorStatus::Partial);
        CHECK(!result && consumed==0);
    }
    std::optional<DeviceDescriptor> result;size_t consumed=0;
    CHECK(Serializer::ReadDescriptor(bytes.data(),bytes.size(),result,consumed)==Serializer::DescriptorStatus::Complete);
    CHECK(result->name()=="test-phone" && consumed==bytes.size());
}
void ParserMalformed() {
    std::optional<DeviceDescriptor> result;size_t consumed=0;
    uint8_t tooLong[]={0xff,0xff};
    CHECK(Serializer::ReadDescriptor(tooLong,2,result,consumed)==Serializer::DescriptorStatus::Invalid);
    CHECK(Serializer::ReadDescriptor(nullptr,1,result,consumed)==Serializer::DescriptorStatus::Invalid);
    std::vector<uint8_t> huge(16385);
    CHECK(Serializer::ReadDescriptor(huge.data(),huge.size(),result,consumed)==Serializer::DescriptorStatus::Invalid);
    auto bytes=Descriptor();bytes.back()=255;
    CHECK(Serializer::ReadDescriptor(bytes.data(),bytes.size(),result,consumed)==Serializer::DescriptorStatus::Invalid);
}
void StallClose() {
    Fixture f;auto socket=f.Connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto begin=std::chrono::steady_clock::now();f.server.Close();
    CHECK(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1));
    f.server.Close();
}
void EofRecovery() {
    Fixture f;
    { auto socket=f.Connect();socket.close(); }
    auto socket=f.Connect();const auto bytes=Descriptor();asio::write(socket,asio::buffer(bytes));CHECK(f.listener.Wait(1));
}
void Fragmented() {
    Fixture f;auto socket=f.Connect();const auto bytes=Descriptor();
    for (const auto byte:bytes) { asio::write(socket,asio::buffer(&byte,1));std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    CHECK(f.listener.Wait(1));
}
void MalformedRecovery() {
    Fixture f;
    { auto socket=f.Connect();const uint8_t bytes[]={0xff,0xff};asio::write(socket,asio::buffer(bytes)); }
    { auto socket=f.Connect();const uint8_t bytes[]={0,5,'x'};asio::write(socket,asio::buffer(bytes)); }
    auto socket=f.Connect();const auto bytes=Descriptor();asio::write(socket,asio::buffer(bytes));CHECK(f.listener.Wait(1));
}
void CommandOutputOwned() {
    Fixture f;auto socket=f.Connect();const auto bytes=Descriptor();asio::write(socket,asio::buffer(bytes));CHECK(f.listener.Wait(1));
    std::vector<unsigned char> outgoing(2048,73);f.server.Send(0,outgoing.data(),outgoing.size());std::fill(outgoing.begin(),outgoing.end(),255);
    socket.non_blocking(true);
    std::vector<unsigned char> received(2048);size_t total=0;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (total<received.size() && std::chrono::steady_clock::now()<end) {
        asio::error_code error;total+=socket.read_some(asio::buffer(received.data()+total,received.size()-total),error);
        if (error && error!=asio::error::would_block && error!=asio::error::try_again) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(total==2048);for (auto byte:received) CHECK(byte==73);
}
void ConcurrentStalls() {
    Fixture f;std::vector<asio::ip::tcp::socket> stalls;
    for (unsigned i=0;i<8;++i) stalls.emplace_back(f.Connect());
    auto socket=f.Connect();const auto bytes=Descriptor();asio::write(socket,asio::buffer(bytes));CHECK(f.listener.Wait(1));
    const auto begin=std::chrono::steady_clock::now();f.server.Close();CHECK(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1));
}
int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--force-failure") return 19;
    unsigned passed=0;
    for (const auto& test:std::vector<std::pair<const char*,void(*)()>>{
        {"every partial descriptor prefix is bounded",ParserPrefixes},
        {"invalid sizes and categories rejected",ParserMalformed},
        {"empty handshake shutdown bounded and idempotent",StallClose},
        {"EOF before descriptor does not stop accept",EofRecovery},
        {"one-byte fragmented descriptor accepted",Fragmented},
        {"invalid and truncated clients preserve next accept",MalformedRecovery},
        {"asynchronous command write owns current bytes",CommandOutputOwned},
        {"parallel stalled clients do not block valid phone or shutdown",ConcurrentStalls}}) {
        try { test.second();++passed;std::cout<<"PASS "<<test.first<<"\n"; }
        catch (const std::exception& error) { std::cerr<<"FAIL "<<test.first<<": "<<error.what()<<"\n"; }
    }
    std::cout<<passed<<"/8 passed\n";return passed==8?0:1;
}
