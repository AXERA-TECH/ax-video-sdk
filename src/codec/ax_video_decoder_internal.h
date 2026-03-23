#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ax_global_type.h"

#include "codec/ax_video_decoder.h"

namespace axvsdk::codec::internal {

class AxVideoDecoderBase : public VideoDecoder {
public:
    ~AxVideoDecoderBase() override;

    bool Open(const VideoDecoderConfig& config) override;
    void Close() noexcept override;
    bool Start() override;
    void Stop() noexcept override;

    bool SubmitPacket(EncodedPacket packet) override;
    bool SubmitEndOfStream() override;

    common::AxImage::Ptr GetLatestFrame() override;
    bool GetLatestFrame(common::AxImage& output_image) override;
    void SetFrameCallback(FrameCallback callback) override;
    void SetFrameCallback(FrameCallback callback, FrameCallbackMode mode) override;

protected:
    virtual bool CreateBackend(const Mp4VideoInfo& video_info) = 0;
    virtual void DestroyBackend() noexcept = 0;
    virtual bool StartBackend() = 0;
    virtual void StopBackend() noexcept = 0;
    virtual bool SendEncodedPacket(const EncodedPacket& packet) = 0;
    virtual bool SendEndOfStream() = 0;
    virtual bool ReceiveDecodedFrame(AX_VIDEO_FRAME_INFO_T* frame_info, bool* flow_end) = 0;
    virtual void ReleaseDecodedFrame(const AX_VIDEO_FRAME_INFO_T& frame_info) noexcept = 0;

    const Mp4VideoInfo& video_info() const noexcept;
    const VideoDecoderConfig& config() const noexcept;
    const common::ImageDescriptor& native_output_descriptor() const noexcept;
    bool stop_requested() const noexcept;

private:
    void SendLoop();
    void ReceiveLoop();
    void CallbackLoop();
    bool ValidateRequestedOutput(const common::ImageDescriptor& requested) const noexcept;
    void PublishFrame(const AX_VIDEO_FRAME_INFO_T& frame_info);

    VideoDecoderConfig config_{};
    Mp4VideoInfo video_info_{};
    common::ImageDescriptor native_output_descriptor_{};

    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> send_finished_{false};
    std::atomic<bool> callback_stop_{false};
    bool input_eos_requested_{false};

    std::thread send_thread_;
    std::thread receive_thread_;
    std::thread callback_thread_;

    std::mutex input_mutex_;
    std::condition_variable input_cv_;
    std::deque<EncodedPacket> pending_packets_;

    mutable std::mutex latest_mutex_;
    common::AxImage::Ptr latest_frame_;

    std::mutex callback_mutex_;
    std::condition_variable callback_cv_;
    FrameCallback frame_callback_;
    FrameCallbackMode callback_mode_{FrameCallbackMode::kLatest};
    common::AxImage::Ptr pending_callback_frame_;
    std::deque<common::AxImage::Ptr> callback_queue_;
};

std::unique_ptr<VideoDecoder> CreatePlatformVideoDecoder();

}  // namespace axvsdk::codec::internal
