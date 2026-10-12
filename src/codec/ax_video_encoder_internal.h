#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <optional>
#include <mutex>
#include <thread>
#include <vector>

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
    common::ResizeOptions resize{};
    QpMapOptions qpmap{};
};

struct PreparedInputFrame {
    common::AxImage::Ptr frame;
    bool reusable{false};
    bool hold_for_inflight{false};
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
    void SetQpRoi(const std::vector<QpRoiRect>& rois) override;
    void SetTargetBitrateKbps(std::uint32_t kbps) override;

protected:
    virtual bool CreateBackend(const ResolvedVideoEncoderConfig& config) = 0;
    virtual void DestroyBackend() noexcept = 0;
    virtual bool StartBackend() = 0;
    virtual void StopBackend() noexcept = 0;
    virtual bool SendFrameToEncoder(const common::AxImage& frame) = 0;
    virtual bool ReceivePacketFromEncoder(EncodedPacket* packet, bool* flow_end) = 0;

    const ResolvedVideoEncoderConfig& config() const noexcept;
    // 后端在送帧时取当前 ROI 快照来构建 QPMAP。
    std::vector<QpRoiRect> qp_rois() const;
    // 后端在送帧时取本帧该用的背景 QP:固定配置值,或码率闭环的当前结果。
    std::int32_t QpMapBaseQp(bool intra) const noexcept;
    // ROI 区该用的 QP。背景封顶后闭环会从这里继续要码率(收窄 ROI 优待)。
    std::int32_t QpMapRoiQp(std::int32_t base_qp) const noexcept;
    bool ValidateInputFrame(const common::AxImage& frame) const noexcept;
    PreparedInputFrame PrepareInputFrame(const common::AxImage& frame);
    bool stop_requested() const noexcept;

private:
    struct PoolEntry {
        common::ImageDescriptor descriptor{};
        std::uint64_t block_size{0};
        std::uint32_t pool_id{common::kInvalidPoolId};
    };

    common::AxImage::Ptr AcquireReusableFrame(const common::ImageDescriptor& descriptor, const char* token,
                                              bool require_pool);
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

    mutable std::mutex roi_mutex_;
    std::vector<QpRoiRect> qp_rois_;

    // 码率统计与 QPMAP 背景 QP 闭环。
    void AccountPacket(std::size_t bytes);
    mutable std::mutex rate_mutex_;
    std::uint32_t target_bitrate_kbps_{0};
    std::uint32_t measured_bitrate_kbps_{0};
    double qp_bias_{0.0};
    std::uint64_t window_bytes_{0};
    std::uint64_t window_packets_{0};

    std::mutex staging_mutex_;
    std::deque<common::AxImage::Ptr> reusable_frames_;
    std::deque<common::AxImage::Ptr> inflight_frames_;
    std::deque<common::AxImage::Ptr> inflight_hold_frames_;
    std::vector<PoolEntry> pools_;
};

std::unique_ptr<VideoEncoder> CreatePlatformVideoEncoder();

}  // namespace axvsdk::codec::internal
