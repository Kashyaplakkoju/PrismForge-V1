#include "directshowsource.h"
#include <chrono>

DirectShowSource::DirectShowSource(int width, int height)
	: scaler(width, height),
	width(width),
	height(height)
{
	camera = scCreateCamera(width, height, 0);
	mfWriter.Initialize(width, height);
	streamingEnabled = true;
}

DirectShowSource::~DirectShowSource()
{
	std::lock_guard<std::mutex> lock(mtx);
	streamingEnabled = false;
	mfWriter.Close();
	if (camera)
	{
		scDeleteCamera(camera);
		camera = nullptr;
	}
}

void DirectShowSource::SendRawFrame(const AVFrame* frame, DeliveryTiming* timing)
{
	if (timing) *timing = {};
	if (!streamingEnabled || !frame)
		return;

	std::lock_guard<std::mutex> lock(mtx);
	if (!streamingEnabled)
		return;

	const auto begin = std::chrono::steady_clock::now();
	// 1. Output to Media Foundation Virtual Camera (Windows Settings, Camera app, Teams, etc.)
	mfWriter.WriteFrame(frame);
	const auto mfDone = std::chrono::steady_clock::now();

	// Softcam remembers whether a consumer has ever connected. Until then,
	// avoid a full RGB conversion with no reader. After the first connection
	// keep publishing so later receiver starts/reconnects retain fresh frames.
	if (camera && scIsConnected(camera))
	{
		int w, h;
		const uint8_t* bytes = scaler.Process(frame, w, h);
		if (bytes && w > 0 && h > 0)
		{
			scSendFrame(camera, bytes);
		}
	}
	if (timing) {
		timing->mediaFoundationMs = std::chrono::duration<double, std::milli>(mfDone - begin).count();
		timing->directShowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mfDone).count();
	}
}

bool DirectShowSource::Recreate(int newWidth, int newHeight)
{
	std::lock_guard<std::mutex> lock(mtx);

	if (width == newWidth && height == newHeight && camera != nullptr)
		return true;

	streamingEnabled = false;

	if (camera)
	{
		scDeleteCamera(camera);
		camera = nullptr;
	}

	width = newWidth;
	height = newHeight;
	scaler = Video::DirectShowScaler(width, height);
	camera = scCreateCamera(width, height, 0);
	mfWriter.Initialize(width, height);
	streamingEnabled = true;

	return streamingEnabled;
}

void DirectShowSource::WaitForConnection()
{
	std::lock_guard<std::mutex> lock(mtx);
	if (camera)
		scWaitForConnection(camera);
}

void DirectShowSource::Pause()
{
	std::lock_guard<std::mutex> lock(mtx);
	streamingEnabled = false;
	mfWriter.SetStreaming(false);
}

void DirectShowSource::Resume()
{
	std::lock_guard<std::mutex> lock(mtx);
	streamingEnabled = true;
	mfWriter.SetStreaming(true);
}