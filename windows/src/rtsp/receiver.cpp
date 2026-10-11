#include "rtsp/receiver.h"
#include "logger.h"
#include <cmath>

namespace RTSP
{
	Receiver::Receiver(OnFrameReceivedCallback frameReceivedListener, OnStatsReceivedCallback statsReceivedCallback)
		: onFrameReceivedCallback(frameReceivedListener),
		onStatsReceivedCallback(statsReceivedCallback),
		isRunning(false)
	{
		avformat_network_init();
	}

	Receiver::~Receiver() 
	{
		Stop();
		avformat_network_deinit();
	}

	void Receiver::Start(std::string url, std::string protocol, int width, int height) 
	{
		if (isRunning)
			return;
	
		this->rtspProtocol = protocol;
		this->rtspUrl = url;
		this->expectedWidth = width;
		this->expectedHeight = height;
        state.stopRequested = false;
		isRunning = true;
		workerThread = std::thread(&Receiver::WorkerFunc, this);
	}

	void Receiver::Stop() 
	{
		isRunning = false;
		state.stopRequested = true;

		if (workerThread.joinable()) 
			workerThread.join();
	}

	bool Receiver::OpenConnection(AVFormatContext** ctx) {
		*ctx = avformat_alloc_context();
        if (!*ctx) return false;
		(*ctx)->interrupt_callback.callback = FFmpegInterrupt::Callback;
		(*ctx)->interrupt_callback.opaque = &this->state;

        if (state.stopRequested) return false;
		state.lastActivityTime = FFmpegInterrupt::GetTimeMs();

		AVDictionary* options = nullptr;
		av_dict_set(&options, "rtsp_transport", rtspProtocol.c_str(), 0);
		// Low latency settings
		av_dict_set(&options, "fflags", "nobuffer", 0);
		av_dict_set(&options, "flags", "low_delay", 0);
		// Immediate start
		av_dict_set(&options, "probesize", "32768", 0);
		av_dict_set(&options, "analyzeduration", "1000000", 0);

		int ret = avformat_open_input(ctx, rtspUrl.c_str(), nullptr, &options);
        av_dict_free(&options);
		if (ret != 0)
		{
			char buffer[512];
			av_strerror(ret, buffer, 512);
			logger << "[RTSP] Error: Could not open stream\n";
			return false;
		}


		// The resolution is known beforehand and the codec is always h264
		// so we can skip this part
		/*if (avformat_find_stream_info(*ctx, nullptr) < 0)
		{
			logger << "[RTSP] Error: Could not find stream info\n";
			return false;
		}*/

		this->state.lastActivityTime = FFmpegInterrupt::GetTimeMs();

		return true;
	}

	bool Receiver::FindVideoStream(AVFormatContext* ctx, int& streamIdx, AVCodecContext** codecCtx, int width, int height) {
		streamIdx = -1;

		for (unsigned int i = 0; i < ctx->nb_streams; i++) 
		{
			if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) 
			{
				streamIdx = i;
				break;
			}
		}

		if (streamIdx == -1) 
		{
			logger << "[RTSP] Error: No video stream found\n";
			return false;
		}

		auto codecId = ctx->streams[streamIdx]->codecpar->codec_id;
		if (codecId == AV_CODEC_ID_NONE) codecId = AV_CODEC_ID_H264;
		const AVCodec* codec = avcodec_find_decoder(codecId);
		if (!codec)
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		}
		if (!codec)
		{
			logger << "[RTSP] Error: Decoder not found\n";
			return false;
		}

		*codecCtx = avcodec_alloc_context3(codec);
        if (!*codecCtx || avcodec_parameters_to_context(*codecCtx, ctx->streams[streamIdx]->codecpar) < 0) return false;

		if ((*codecCtx)->width <= 0 && width > 0)
			(*codecCtx)->width = width;
		if ((*codecCtx)->height <= 0 && height > 0)
			(*codecCtx)->height = height;

		if ((*codecCtx)->pix_fmt == AV_PIX_FMT_NONE) 
		{
			(*codecCtx)->pix_fmt = AV_PIX_FMT_YUV420P;
		}

		// Enable multithreading for decoding
		(*codecCtx)->thread_count = 4;
		(*codecCtx)->thread_type = FF_THREAD_SLICE;

		if (avcodec_open2(*codecCtx, codec, nullptr) < 0) 
		{
			logger << "[RTSP] Error: Could not open codec\n";
			return false;
		}
		return true;
	}

	void Receiver::WorkerFunc()
	{
		while (isRunning)
		{
			Loop(expectedWidth, expectedHeight);

			if (isRunning)
			{
				logger << "[RTSP] Connection lost (Resolution change?). Retrying in 500ms...\n";
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		}
	}

	void Receiver::Loop(int expectedWidth, int expectedHeight) 
	{
		AVFormatContext* formatCtx = nullptr;
		AVCodecContext* codecCtx = nullptr;
		int videoStreamIdx = -1;

		// Open connection to the RTSP stream
        if (!OpenConnection(&formatCtx)) {
            if (formatCtx) avformat_close_input(&formatCtx);
            return;
        }

		if (!FindVideoStream(formatCtx, videoStreamIdx, &codecCtx, expectedWidth, expectedHeight))
		{
            avcodec_free_context(&codecCtx);
			avformat_close_input(&formatCtx);
			return;
		}

		// Allocate memory for the incoming frame
		AVFrame* pFrame = av_frame_alloc();
		AVPacket* pPacket = av_packet_alloc();
        if (!pFrame || !pPacket) {
            av_frame_free(&pFrame); av_packet_free(&pPacket);
            avcodec_free_context(&codecCtx); avformat_close_input(&formatCtx); return;
        }

		int currentWidth = -1;
		int currentHeight = -1;

		Stats stats = {0, 0, 0};
		auto lastStatsTime = std::chrono::steady_clock::now();
		int64_t byteAccumulator = 0;
		int frameAccumulator = 0;

		logger << "[RTSP] Decoding started...\n";

		while (isRunning) 
		{
			this->state.lastActivityTime = FFmpegInterrupt::GetTimeMs();

			int ret = av_read_frame(formatCtx, pPacket);
			if (ret < 0)
			{
				char errBuf[256];
				av_strerror(ret, errBuf, sizeof(errBuf));
				logger << "[RTSP] av_read_frame returned error: " << errBuf << " (" << ret << ")\n";
				break;
			}

			this->state.lastActivityTime = FFmpegInterrupt::GetTimeMs();

			if (pPacket->stream_index != videoStreamIdx)
			{
				av_packet_unref(pPacket);
				continue;
			}

			// Accumulate byte count (for bitrate)
			byteAccumulator += pPacket->size;

			if (avcodec_send_packet(codecCtx, pPacket) != 0)
			{
				av_packet_unref(pPacket);
				continue;
			}

			while (avcodec_receive_frame(codecCtx, pFrame) == 0) 
			{
				// Accumulate frame count (for fps)
				frameAccumulator++;
				stats.width = pFrame->width;
				stats.height = pFrame->height;

				if (onFrameReceivedCallback) 
				{
					onFrameReceivedCallback(pFrame);
				}
			}

			auto now = std::chrono::steady_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsTime).count();

			if (duration >= 1000)
			{
				stats.fps = static_cast<int>(std::lround(frameAccumulator * 1000.0 / duration));
				stats.bitrate = (byteAccumulator * 8.0) / (duration * 1000.0);

				if (onStatsReceivedCallback)
				{
					onStatsReceivedCallback(stats);
				}

				byteAccumulator = 0;
				frameAccumulator = 0;
				lastStatsTime = now;
			}

			av_packet_unref(pPacket);
		}

		logger << "[RTSP] Thread stopping, cleaning up...\n";

		av_frame_free(&pFrame);
		av_packet_free(&pPacket);
		avcodec_free_context(&codecCtx);
		avformat_close_input(&formatCtx);
	}
};