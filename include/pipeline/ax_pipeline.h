#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/ax_drawer.h"
#include "common/ax_image_processor.h"
#include "codec/ax_video_encoder.h"
#include "pipeline/ax_demuxer.h"
#include "pipeline/ax_muxer.h"

namespace axvsdk::pipeline {

using PipelineInputConfig = DemuxerConfig;

struct PipelineOutputConfig {
    codec::VideoCodecType codec{codec::VideoCodecType::kH264};
    std::uint32_t width{0};
    std::uint32_t height{0};
    double frame_rate{0.0};
    std::uint32_t bitrate_kbps{0};
    std::uint32_t gop{0};
    std::size_t input_queue_depth{0};
    codec::QueueOverflowPolicy overflow_policy{codec::QueueOverflowPolicy::kDropOldest};
    common::ResizeOptions resize{};
    std::vector<std::string> uris;
    codec::PacketCallback packet_callback{};
};

struct PipelineFrameOutputConfig {
    common::ImageDescriptor output_image{};
    common::ResizeOptions resize{};
};

using PipelineOsdPoint = common::DrawPoint;
using PipelineOsdLine = common::DrawLine;
using PipelineOsdPolygon = common::DrawPolygon;
using PipelineOsdRect = common::DrawRect;
using PipelineOsdMosaicBlockSize = common::DrawMosaicBlockSize;
using PipelineOsdMosaic = common::DrawMosaic;
using PipelineOsdBitmapFormat = common::DrawBitmapFormat;
using PipelineOsdBitmap = common::DrawBitmap;
using PipelineOsdFrame = common::DrawFrame;

struct PipelineConfig {
    std::int32_t device_id{-1};
    PipelineInputConfig input{};
    std::vector<PipelineOutputConfig> outputs;
    PipelineFrameOutputConfig frame_output{};
};

struct PipelineStats {
    std::uint64_t decoded_frames{0};
    std::uint64_t branch_submit_failures{0};
    std::vector<codec::VideoEncoderStats> output_stats;
};

using FrameCallback = std::function<void(common::AxImage::Ptr frame)>;

class Pipeline {
public:
    virtual ~Pipeline() = default;

    virtual bool Open(const PipelineConfig& config) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    virtual common::AxImage::Ptr GetLatestFrame() = 0;
    virtual bool GetLatestFrame(common::AxImage& output_image) = 0;
    virtual void SetFrameCallback(FrameCallback callback) = 0;
    virtual bool SetOsd(const PipelineOsdFrame& osd) = 0;
    virtual void ClearOsd() noexcept = 0;

    virtual PipelineStats GetStats() const = 0;
};

std::unique_ptr<Pipeline> CreatePipeline();

}  // namespace axvsdk::pipeline
