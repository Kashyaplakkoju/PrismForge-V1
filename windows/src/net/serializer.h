#pragma once

#include "devicedescriptor.h"
#include <optional>
#include "rtsp/streamoptions.h"
#include "net/connection.h"

namespace Serializer
{
    enum class DescriptorStatus { Partial, Complete, Invalid };
    DescriptorStatus ReadDescriptor(const uint8_t* bytes, size_t size, std::optional<DeviceDescriptor>& result, size_t& consumed);
	DeviceDescriptor DeserializeDeviceDescriptor(const uint8_t* bytes, size_t size);
	std::vector<uint8_t> SerializeStreamOptions(const StreamOptions& state);
	Connection::ErrorReport DeserializeErrorReport(const uint8_t* bytes, size_t size);
}