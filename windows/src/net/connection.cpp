#include "net/connection.h"
#include "logger.h"

Connection::Connection(tcp::socket socket,DeviceDescriptor& descriptor,
    OnDisconnectedListener onDisconnectedListener,OnBytesReceived onBytesReceived)
    : socket(std::move(socket)),descriptor(descriptor),
      onDisconnectedListener(std::move(onDisconnectedListener)),onBytesReceived(std::move(onBytesReceived)) {
    byteBuffer=new unsigned char[4096];
    active=true;
}
Connection::~Connection() { delete[] byteBuffer; }
void Connection::Read() {
    if (!active) return;
    const auto self=shared_from_this();
    socket.async_read_some(asio::buffer(byteBuffer,4096),[self](asio::error_code ec,size_t size) {
        if (!self->active) return;
        if (ec) { self->Close();return; }
        try { self->onBytesReceived(self,self->byteBuffer,size); } catch (...) {}
        self->Read();
    });
}
void Connection::Send(std::string message) { Send(reinterpret_cast<const unsigned char*>(message.data()),message.size()); }
void Connection::Send(const unsigned char* bytes,size_t size) {
    if (!active || !bytes || !size || size>65536 || writes.size()>=64) return;
    writes.emplace_back(bytes,bytes+size);
    if (writes.size()==1) Write();
}
void Connection::Write() {
    if (!active || writes.empty()) return;
    const auto self=shared_from_this();
    asio::async_write(socket,asio::buffer(writes.front()),[self](asio::error_code ec,size_t) {
        if (!self->active) return;
        if (ec) { self->Close();return; }
        self->writes.pop_front();self->Write();
    });
}
void Connection::Close(bool stoppedByServer) {
    if (!active) return;
    active=false;
    asio::error_code ignored;socket.cancel(ignored);socket.close(ignored);
    // Pending writes retain their buffers until cancellation handlers have returned.
    if (!stoppedByServer) onDisconnectedListener(shared_from_this());
}
