#pragma once

#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>

#include "codec/ax_codec_types.h"
#include "common/ax_image.h"

namespace axvsdk::codec {

struct VideoDecoderConfig {
    VideoStreamInfo stream{};
    common::ImageDescriptor output_image{};
    std::int32_t device_id{-1};
};

using FrameCallback = std::function<void(common::AxImage::Ptr frame)>;

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual bool Open(const VideoDecoderConfig& config) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    virtual bool SubmitPacket(EncodedPacket packet) = 0;
    virtual bool SubmitEndOfStream() = 0;

    virtual common::AxImage::Ptr GetLatestFrame() = 0;
    virtual bool GetLatestFrame(common::AxImage& output_image) = 0;
    virtual void SetFrameCallback(FrameCallback callback) = 0;
};

std::unique_ptr<VideoDecoder> CreateVideoDecoder();

}  // namespace axvsdk::codec
