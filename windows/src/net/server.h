#pragma once

#include <tuple>
#include <thread>
#include <asio.hpp>
#include <string>
#include <atomic>
#include <array>
#include <optional>

#include "connection.h"
#include "usbbridge.h"

class Server : std::enable_shared_from_this<Server>
{
public:
	using tcp = asio::ip::tcp;
	using udp = asio::ip::udp;
	using HostInfo = std::tuple<std::string, std::string, std::string>;

	struct ConnectionListener
	{
		virtual void OnDeviceConnected(DeviceDescriptor& descriptor) const = 0;
		virtual void OnDeviceDisconnected(DeviceDescriptor& descriptor) const = 0;
		virtual void OnDeviceErrorReported(DeviceDescriptor& descriptor, const Connection::ErrorReport& error) const = 0;
	};

	struct DeviceInfo
	{
		std::string name;
		std::string address;
		unsigned short port;
	};

	Server(int port, const ConnectionListener& connectionListener, PrismForge::Usb::Runner runner={});
    unsigned short BoundPort() const;
	~Server();
	void RetryUsb();

	/// <summary>
	/// Gets host device's info (name, IPv4 address and port)
	/// </summary>
	HostInfo GetHostInfo();

	void Send(int id, const unsigned char* bytes, size_t size) const;
	void Start();
	void Close();

private:
	int port;
	PrismForge::Usb::Bridge usbBridge;

	const ConnectionListener& connectionListener;

	//asio::executor_work_guard<asio::io_context::executor_type> guard;
	asio::io_context context;

	tcp::acceptor acceptor;
	udp::endpoint remote_endpoint;

    struct Handshake {
        tcp::socket socket;
        asio::steady_timer deadline;
        std::array<unsigned char,1024> chunk{};
        std::vector<uint8_t> bytes;
        bool done=false;
        explicit Handshake(tcp::socket value) : socket(std::move(value)),deadline(socket.get_executor()) {}
    };
    std::atomic_bool closing{false};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
    std::vector<std::shared_ptr<Handshake>> handshakes;
    void ReadHandshake(std::shared_ptr<Handshake> handshake);
    void FinishHandshake(std::shared_ptr<Handshake> handshake);
	std::thread thread;

	std::vector<std::shared_ptr<Connection>> connections;

	void TCPDoAccept();
	void OnConnectionDisconnected(std::shared_ptr<Connection> connection);
	void OnConnectionReportingError(std::shared_ptr<Connection> connection, const uint8_t* bytes, size_t size);
};