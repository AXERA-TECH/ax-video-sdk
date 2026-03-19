#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "codec/ax_codec_types.h"
#include "common/ax_image.h"

namespace axvsdk::codec {

enum class QueueOverflowPolicy {
    kDropOldest = 0,
    kDropNewest,
    kBlock,
};

struct VideoEncoderStats {
    std::uint64_t submitted_frames{0};
    std::uint64_t dropped_frames{0};
    std::uint64_t encoded_packets{0};
    std::uint64_t key_packets{0};
    std::size_t current_queue_depth{0};
    std::size_t queue_capacity{0};
};

struct VideoEncoderConfig {
    VideoCodecType codec{VideoCodecType::kUnknown};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::int32_t device_id{-1};
    double frame_rate{0.0};
    std::uint32_t bitrate_kbps{0};
    std::uint32_t gop{0};
    std::size_t input_queue_depth{0};
    QueueOverflowPolicy overflow_policy{QueueOverflowPolicy::kDropOldest};
};

using PacketCallback = std::function<void(EncodedPacket packet)>;

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual bool Open(const VideoEncoderConfig& config) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    virtual bool SubmitFrame(common::AxImage::Ptr frame) = 0;
    virtual bool GetLatestPacket(EncodedPacket* packet) = 0;
    virtual VideoEncoderStats GetStats() const = 0;
    virtual void SetPacketCallback(PacketCallback callback) = 0;
};

std::unique_ptr<VideoEncoder> CreateVideoEncoder();

}  // namespace axvsdk::codec
