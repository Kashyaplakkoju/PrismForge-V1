#pragma once

#include "video/1_1scaler.h"
#include <atomic>
#include <future>
#include <functional>
#include <vector>

class SnapshotManager
{
public:
	SnapshotManager();
    ~SnapshotManager();
    void Wait();
    std::function<void(bool)> onSaved;

	/*
		Flags the manager to take a snaphot on the next processed frame
	*/
	void RequestSnapshot();

	/*
		Process an incoming frame for snapshotting if requested
	*/
	void ProcessFrame(AVFrame* frame);

private:
	Video::_1_1Scaler scaler;
	std::atomic<bool> snapshotRequested;

    std::future<void> saving;
    static bool SaveSnapshot(const std::vector<uint8_t>& data, int width, int height);
};
