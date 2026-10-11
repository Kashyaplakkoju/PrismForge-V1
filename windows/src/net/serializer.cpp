#include "net/serializer.h"

#include <string>
#include <stdexcept>

namespace Serializer
{
	static uint16_t ReadInt16(const uint8_t* bytes)
	{
		uint8_t b0 = static_cast<uint8_t>(bytes[0]);
		uint8_t b1 = static_cast<uint8_t>(bytes[1]);

		return (b0 << 8) | b1;
	}

	static void WriteInt16(std::vector<uint8_t>& buffer, uint16_t value)
	{
		buffer.push_back((value >> 8) & 0xFF);
		buffer.push_back(value & 0xFF);
	}

	static void WriteInt32(std::vector<uint8_t>& buffer, uint32_t value)
	{
		buffer.push_back((value >> 24) & 0xFF);
		buffer.push_back((value >> 16) & 0xFF);
		buffer.push_back((value >> 8) & 0xFF);
		buffer.push_back(value & 0xFF);
	}

	static void WriteBool(std::vector<uint8_t>& buffer, bool value)
	{
		buffer.push_back(value ? 1 : 0);
	}

	static void WriteString(std::vector<uint8_t>& buffer, std::string value)
	{
		WriteInt16(buffer, static_cast<uint16_t>(value.size()));
		buffer.insert(buffer.end(), value.begin(), value.end());
	}

    namespace {
        struct NeedMore {};
        struct InvalidDescriptor {};
        struct DescriptorReader {
            const uint8_t* bytes;
            size_t size, offset=0;
            uint16_t Word() {
                if (size-offset<2) throw NeedMore{};
                const auto value=ReadInt16(bytes+offset);offset+=2;return value;
            }
            std::string String(size_t maximum) {
                const size_t length=Word();
                if (!length || length>maximum) throw InvalidDescriptor{};
                if (size-offset<length) throw NeedMore{};
                std::string value(reinterpret_cast<const char*>(bytes+offset),length);offset+=length;
                if (value.find('\0')!=std::string::npos) throw InvalidDescriptor{};
                return value;
            }
            std::vector<DeviceDescriptor::Resolution> Resolutions() {
                const unsigned count=Word();
                if (count>128) throw InvalidDescriptor{};
                std::vector<DeviceDescriptor::Resolution> result;
                result.reserve(count);
                for (unsigned i=0;i<count;++i) {
                    const auto width=Word(),height=Word();
                    if (!width || !height || width>16384 || height>16384) throw InvalidDescriptor{};
                    result.emplace_back(width,height);
                }
                return result;
            }
        };
    }
    DescriptorStatus ReadDescriptor(const uint8_t* bytes,size_t size,
        std::optional<DeviceDescriptor>& result,size_t& consumed) {
        result.reset();consumed=0;
        if ((!bytes && size) || size>16384) return DescriptorStatus::Invalid;
        try {
            DescriptorReader reader{bytes,size};
            const auto name=reader.String(256);
            const auto url=reader.String(2048);
            const auto front=reader.Resolutions(),back=reader.Resolutions();
            const unsigned filterCount=reader.Word();
            if (filterCount>128) return DescriptorStatus::Invalid;
            Video::Filter::Registry filters;
            for (unsigned i=0;i<filterCount;++i) {
                const auto filter=reader.String(512);
                if (reader.offset==size) throw NeedMore{};
                const auto category=bytes[reader.offset++];
                if (category>Video::Filter::ARTISTIC) return DescriptorStatus::Invalid;
                filters[static_cast<Video::Filter::Category>(category)].push_back(filter);
            }
            result.emplace(name,url,url.find("127.0.0.1")!=std::string::npos ? "tcp":"udp",front,back,filters);
            consumed=reader.offset;
            return DescriptorStatus::Complete;
        } catch (const NeedMore&) { return DescriptorStatus::Partial; }
          catch (...) { return DescriptorStatus::Invalid; }
    }
    DeviceDescriptor DeserializeDeviceDescriptor(const uint8_t* bytes,size_t size) {
        std::optional<DeviceDescriptor> result;size_t consumed=0;
        if (ReadDescriptor(bytes,size,result,consumed)!=DescriptorStatus::Complete)
            throw std::runtime_error("Incomplete or invalid device descriptor");
        return std::move(*result);
    }

	std::vector<uint8_t> SerializeStreamOptions(const StreamOptions& state)
	{
		std::vector<uint8_t> buffer;
		buffer.reserve(512);

		// First byte of the buffer is reserved
		buffer.push_back(0);

		WriteInt32(buffer, state.fps);
		WriteInt32(buffer, state.resolution.first);
		WriteInt32(buffer, state.resolution.second);

		WriteInt32(buffer, state.backCameraActive ? 1 : 0);

		WriteBool(buffer, state.adaptiveBitrate);
		WriteInt32(buffer, state.bitrate);
		WriteInt32(buffer, state.minBitrate);
		WriteInt32(buffer, state.maxBitrate);

		WriteBool(buffer, state.stabilizationEnabled);
		WriteBool(buffer, state.flashEnabled);
		WriteBool(buffer, state.h265Enabled);

		WriteInt32(buffer, state.focusMode);

		std::vector<std::pair<std::string, int32_t>> activeFilters;
		for (const auto& [name, value] : state.filterSliderValues)
		{
			if (value != 50)
				activeFilters.push_back({ name, value });
		}

		WriteInt16(buffer, static_cast<uint16_t>(activeFilters.size()));
		for (const auto& [name, value] : activeFilters)
		{
			WriteString(buffer, name);
			WriteInt32(buffer, value);
		}

		WriteString(buffer, state.activeEffectFilter);

		return buffer;
	}

	Connection::ErrorReport DeserializeErrorReport(const uint8_t* bytes, size_t size)
	{
		int offset = 0;

		Connection::ErrorReport report;

		report.severity = bytes[offset++];
		report.error = std::string((const char*)&bytes[offset + 2], ReadInt16(bytes + offset));

		offset += 2 + report.error.size();
		report.description = std::string((const char*)&bytes[offset + 2], ReadInt16(bytes + offset));

		return report;
	}
}