#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

struct PipelineOsdPoint {
    std::int32_t x{0};
    std::int32_t y{0};
};

struct PipelineOsdLine {
    std::vector<PipelineOsdPoint> points;
    std::uint16_t thickness{2};
    std::uint8_t alpha{255};
    std::uint32_t color{0xFF0000};
};

struct PipelineOsdPolygon {
    std::vector<PipelineOsdPoint> points;
    std::uint16_t thickness{2};
    std::uint8_t alpha{255};
    std::uint32_t color{0xFF0000};
    bool filled{false};
};

struct PipelineOsdRect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint16_t thickness{2};
    std::uint8_t alpha{255};
    std::uint32_t color{0xFF0000};
    bool filled{false};
    bool corner_only{false};
    std::uint16_t corner_horizontal_length{0};
    std::uint16_t corner_vertical_length{0};
};

enum class PipelineOsdMosaicBlockSize {
    k2 = 2,
    k4 = 4,
    k8 = 8,
    k16 = 16,
    k32 = 32,
    k64 = 64,
};

struct PipelineOsdMosaic {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    PipelineOsdMosaicBlockSize block_size{PipelineOsdMosaicBlockSize::k16};
};

enum class PipelineOsdBitmapFormat {
    kArgb8888 = 0,
    kRgba8888,
    kArgb1555,
    kRgba5551,
    kArgb4444,
    kRgba4444,
    kArgb8565,
    kRgb888,
    kRgb565,
    kBitmap1,
};

struct PipelineOsdBitmap {
    PipelineOsdBitmapFormat format{PipelineOsdBitmapFormat::kArgb8888};
    std::uint16_t alpha{255};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t dst_x{0};
    std::uint32_t dst_y{0};
    std::uint32_t color{0xFFFFFF};
    bool color_invert{false};
    std::uint32_t color_invert_value{0};
    std::uint32_t color_invert_threshold{0};
    std::vector<std::uint8_t> data;
};

struct PipelineOsdFrame {
    std::uint32_t hold_frames{1};
    std::vector<PipelineOsdLine> lines;
    std::vector<PipelineOsdPolygon> polygons;
    std::vector<PipelineOsdRect> rects;
    std::vector<PipelineOsdMosaic> mosaics;
    std::vector<PipelineOsdBitmap> bitmaps;
};

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
