#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <optional>
#include <mutex>
#include <thread>

#include "codec/ax_video_encoder.h"

namespace axvsdk::codec::internal {

struct ResolvedVideoEncoderConfig {
    VideoCodecType codec{VideoCodecType::kUnknown};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::int32_t device_id{-1};
    std::uint32_t max_width{0};
    std::uint32_t max_height{0};
    double src_frame_rate{30.0};
    double dst_frame_rate{30.0};
    std::uint32_t bitrate_kbps{0};
    std::uint32_t gop{0};
    std::size_t input_queue_depth{10};
    QueueOverflowPolicy overflow_policy{QueueOverflowPolicy::kDropOldest};
    std::size_t stream_buffer_size{0};
};

struct PreparedInputFrame {
    common::AxImage::Ptr frame;
    bool reusable{false};
};

class AxVideoEncoderBase : public VideoEncoder {
public:
    ~AxVideoEncoderBase() override;

    bool Open(const VideoEncoderConfig& config) override;
    void Close() noexcept override;
    bool Start() override;
    void Stop() noexcept override;

    bool SubmitFrame(common::AxImage::Ptr frame) override;
    bool GetLatestPacket(EncodedPacket* packet) override;
    VideoEncoderStats GetStats() const override;
    void SetPacketCallback(PacketCallback callback) override;

protected:
    virtual bool CreateBackend(const ResolvedVideoEncoderConfig& config) = 0;
    virtual void DestroyBackend() noexcept = 0;
    virtual bool StartBackend() = 0;
    virtual void StopBackend() noexcept = 0;
    virtual bool SendFrameToEncoder(const common::AxImage& frame) = 0;
    virtual bool ReceivePacketFromEncoder(EncodedPacket* packet, bool* flow_end) = 0;

    const ResolvedVideoEncoderConfig& config() const noexcept;
    bool ValidateInputFrame(const common::AxImage& frame) const noexcept;
    PreparedInputFrame PrepareInputFrame(const common::AxImage& frame);
    bool stop_requested() const noexcept;

private:
    common::AxImage::Ptr AcquireReusableFrame(const common::ImageDescriptor& descriptor, const char* token);
    void RecyclePreparedFrame(common::AxImage::Ptr frame);
    void ReleaseOldInflightFrame();
    void SendLoop();
    void StreamLoop();

    ResolvedVideoEncoderConfig config_{};
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    std::thread send_thread_;
    std::thread stream_thread_;

    mutable std::mutex input_mutex_;
    std::condition_variable input_cv_;
    std::deque<common::AxImage::Ptr> pending_frames_;
    std::atomic<std::uint64_t> submitted_frames_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};

    mutable std::mutex packet_mutex_;
    EncodedPacket latest_packet_{};
    bool has_latest_packet_{false};
    std::atomic<std::uint64_t> encoded_packets_{0};
    std::atomic<std::uint64_t> key_packets_{0};

    std::mutex callback_mutex_;
    PacketCallback packet_callback_;

    std::mutex staging_mutex_;
    std::deque<common::AxImage::Ptr> reusable_frames_;
    std::deque<common::AxImage::Ptr> inflight_frames_;
};

std::unique_ptr<VideoEncoder> CreatePlatformVideoEncoder();

}  // namespace axvsdk::codec::internal
