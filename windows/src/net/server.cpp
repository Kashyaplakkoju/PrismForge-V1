#include "net/server.h"

#include "logger.h"
#include "adb.h"
#include "brand.h"
#include "net/serializer.h"

Server::Server(int port, const ConnectionListener& connectionListener, PrismForge::Usb::Runner runner) :
	port(port),
	usbBridge(port,std::move(runner)),
	connectionListener(connectionListener),
	acceptor(context)
{
	try {
		tcp::endpoint endpoint(tcp::v4(), port);
		acceptor.open(endpoint.protocol());

		// Use an exclusive listener; another instance must not share this port.
		acceptor.set_option(tcp::acceptor::reuse_address(false));

		acceptor.bind(endpoint);
		acceptor.listen();

		logger << "[SERVER] Initialized on port " << port << std::endl;

	}
	catch(std::exception& e)
	{
		logger << "[SERVER] CRITICAL INIT ERROR: " << e.what() << std::endl;
		throw;
	}
}

Server::~Server() { Close(); }

void Server::RetryUsb() { usbBridge.RequestRetry(); }

Server::HostInfo Server::GetHostInfo()
{
	std::string name = asio::ip::host_name();
	std::string ip_str = "127.0.0.1"; // Default fallback

	try {
		// Get the active network adapter by creating a dummy UDP connection
		// and let the OS determine what network adapter will be used
		udp::resolver resolver(context);
		udp::socket socket(context);

		// This might throw if there is NO network adapter enabled
		socket.connect(udp::endpoint(asio::ip::address::from_string("8.8.8.8"), 80));
		ip_str = socket.local_endpoint().address().to_string();
		socket.close();
	}
	catch (std::exception& e)
	{
		logger << "[SERVER] Warning: Could not detect local IP (" << e.what() << "). Defaulting to localhost.\n";
	}

	return { name, ip_str, std::to_string(port) };
}

unsigned short Server::BoundPort() const { return acceptor.local_endpoint().port(); }

void Server::Send(int id,const unsigned char* bytes,size_t size) const {
    if (closing.load() || !bytes || !size || size>65536) return;
    std::vector<unsigned char> owned(bytes,bytes+size);
    auto* self=const_cast<Server*>(this);
    asio::post(self->context,[self,id,owned=std::move(owned)] {
        if (!self->closing.load() && id>=0 && static_cast<size_t>(id)<self->connections.size())
            self->connections[id]->Send(owned.data(),owned.size());
    });
}
void Server::Start() {
    if (thread.joinable() || !acceptor.is_open()) return;
    closing=false;
    guard.emplace(context.get_executor());
    TCPDoAccept();
    thread=std::thread([this] {
        for (;;) {
            try { context.run();break; }
            catch (const std::exception& error) { logger<<"[SERVER] IO callback error: "<<error.what()<<std::endl; }
            catch (...) { logger<<"[SERVER] Unknown IO callback error"<<std::endl; }
        }
    });
    usbBridge.Start([](const std::string& status) { logger<<"[USB] "<<status<<std::endl; });
    logger<<"[SERVER] Started"<<std::endl;
}
void Server::Close() {
    if (closing.exchange(true)) return;
    usbBridge.Stop();
    auto closeSockets=[this] {
        asio::error_code ignored;acceptor.cancel(ignored);acceptor.close(ignored);
        for (const auto& handshake:handshakes) {
            handshake->done=true;handshake->deadline.cancel();
            handshake->socket.cancel(ignored);handshake->socket.close(ignored);
        }
        handshakes.clear();
        for (const auto& connection:connections) connection->Close(true);
        connections.clear();guard.reset();
    };
    if (thread.joinable()) {
        asio::post(context,std::move(closeSockets));
        thread.join();
    } else closeSockets();
    logger<<"[SERVER] Closed"<<std::endl;
}
void Server::TCPDoAccept() {
    if (closing.load() || !acceptor.is_open()) return;
    acceptor.async_accept([this](asio::error_code error,tcp::socket socket) {
        // A failed or abandoned handshake never consumes the next accept.
        if (!closing.load() && acceptor.is_open()) TCPDoAccept();
        if (error || closing.load()) return;
        if (handshakes.size()>=16 || connections.size()>=32) {
            asio::error_code ignored;socket.close(ignored);return;
        }
        auto handshake=std::make_shared<Handshake>(std::move(socket));
        handshakes.push_back(handshake);
        handshake->deadline.expires_after(std::chrono::seconds(5));
        handshake->deadline.async_wait([this,handshake](asio::error_code error) {
            if (!error) FinishHandshake(handshake);
        });
        ReadHandshake(handshake);
    });
}
void Server::FinishHandshake(std::shared_ptr<Handshake> handshake) {
    if (handshake->done) return;
    handshake->done=true;
    handshake->deadline.cancel();
    asio::error_code ignored;handshake->socket.cancel(ignored);handshake->socket.close(ignored);
    handshakes.erase(std::remove(handshakes.begin(),handshakes.end(),handshake),handshakes.end());
}
void Server::ReadHandshake(std::shared_ptr<Handshake> handshake) {
    if (handshake->done || closing.load()) return;
    handshake->socket.async_read_some(asio::buffer(handshake->chunk),[this,handshake](asio::error_code error,size_t size) {
        if (handshake->done || closing.load()) return;
        if (error || !size || handshake->bytes.size()+size>16384) { FinishHandshake(handshake);return; }
        handshake->bytes.insert(handshake->bytes.end(),handshake->chunk.begin(),handshake->chunk.begin()+size);
        std::optional<DeviceDescriptor> descriptor;size_t consumed=0;
        const auto status=Serializer::ReadDescriptor(handshake->bytes.data(),handshake->bytes.size(),descriptor,consumed);
        if (status==Serializer::DescriptorStatus::Partial) { ReadHandshake(handshake);return; }
        if (status==Serializer::DescriptorStatus::Invalid) { FinishHandshake(handshake);return; }
        handshake->done=true;handshake->deadline.cancel();
        auto connection=std::make_shared<Connection>(std::move(handshake->socket),*descriptor,
            [this](auto value) { OnConnectionDisconnected(value); },
            [this](auto value,const uint8_t* data,size_t count) { OnConnectionReportingError(value,data,count); });
        handshakes.erase(std::remove(handshakes.begin(),handshakes.end(),handshake),handshakes.end());
        connections.push_back(connection);
        connection->Read();
        logger<<"[SERVER] Complete descriptor received: "<<consumed<<" bytes"<<std::endl;
        try { connectionListener.OnDeviceConnected(*descriptor); } catch (...) {}
    });
}
void Server::OnConnectionDisconnected(std::shared_ptr<Connection> connection) {
    connections.erase(std::remove(connections.begin(),connections.end(),connection),connections.end());
    if (!closing.load()) connectionListener.OnDeviceDisconnected(connection->descriptor);
}
void Server::OnConnectionReportingError(std::shared_ptr<Connection> connection,const uint8_t* bytes,size_t size) {
    // Error reports contain severity and two length-prefixed strings. Ignore a
    // malformed/truncated report rather than reading past this network fragment.
    if (!bytes || size<5) return;
    size_t offset=1;
    for (unsigned field=0;field<2;++field) {
        if (size-offset<2) return;
        const size_t length=(size_t(bytes[offset])<<8)|bytes[offset+1];offset+=2;
        if (length>size-offset) return;offset+=length;
    }
    auto report=Serializer::DeserializeErrorReport(bytes,size);
    connectionListener.OnDeviceErrorReported(connection->descriptor,report);
}
