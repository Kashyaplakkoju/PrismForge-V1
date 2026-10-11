#pragma once

#include "softcam/softcam.h"
#include <atomic>
#include <mutex>
#include "video/directshowscaler.h"
#include "mfsource/MFSharedMemWriter.h"

/*
	Wrapper around softcam library and Media Foundation Virtual Camera.
	Manages both the scCamera instance and the cross-session shared memory producer.
*/
class DirectShowSource
{
public:
	DirectShowSource(int width, int height);
	~DirectShowSource();

	struct DeliveryTiming { double mediaFoundationMs=0, directShowMs=0; };
	void SendRawFrame(const AVFrame* frame, DeliveryTiming* timing=nullptr);
	bool Recreate(int newWidth, int newHeight);

	void WaitForConnection();
	void Pause();
	void Resume();

private:
	int width, height;

	scCamera camera = nullptr;
	Video::DirectShowScaler scaler;
	MFSharedMemWriter mfWriter;

	std::mutex mtx;
	std::atomic<bool> streamingEnabled{ false };
	std::atomic<bool> streamingFrame{ false };
};
