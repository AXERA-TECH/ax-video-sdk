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

// pipeline 输入由 URI 自动识别，通常无需调用方手工指定类型。
using PipelineInputConfig = DemuxerConfig;

struct PipelineOutputConfig {
    codec::VideoCodecType codec{codec::VideoCodecType::kH264};
    // 传 0 表示默认跟随输入视频尺寸。
    std::uint32_t width{0};
    std::uint32_t height{0};
    // 传 0 表示默认跟随输入帧率。
    double frame_rate{0.0};
    // 传 0 表示内部按分辨率和编码类型自动估算目标码率。
    std::uint32_t bitrate_kbps{0};
    // 传 0 表示内部按输出帧率自动估算。
    std::uint32_t gop{0};
    // 传 0 表示使用 encoder 内部默认队列深度。
    std::size_t input_queue_depth{0};
    codec::QueueOverflowPolicy overflow_policy{codec::QueueOverflowPolicy::kDropOldest};
    // 输出尺寸与输入不一致时，送入编码器前的 resize 策略。
    common::ResizeOptions resize{};
    // 可同时配置多个目标，例如 MP4 文件和 RTSP 推流地址。
    std::vector<std::string> uris;
    // 额外的 host 侧编码包回调。默认 nullptr。
    codec::PacketCallback packet_callback{};
};

struct PipelineFrameOutputConfig {
    // GetLatestFrame / frame callback 的输出图像描述。
    // format/width/height 为空时默认跟随解码原图，通常为 NV12。
    common::ImageDescriptor output_image{};
    // frame output 的缩放策略。
    common::ResizeOptions resize{};
};

struct PipelineConfig {
    // AXCL 下建议显式指定 device_id；同一个 Pipeline 只绑定一张卡。
    std::int32_t device_id{-1};
    PipelineInputConfig input{};
    // 允许 1 路输入对应 N 路输出。
    std::vector<PipelineOutputConfig> outputs;
    // 控制对外 latest frame / frame callback 的输出格式和尺寸。
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

    // 输入流元信息（来自 demuxer）。
    virtual codec::VideoStreamInfo GetInputStreamInfo() const noexcept = 0;

    // 返回值版本：
    // 返回当前“最新一帧”语义的图像。
    // 默认按 frame_output 配置生成；若未配置，通常返回 NV12 解码原图尺寸。
    virtual common::AxImage::Ptr GetLatestFrame() = 0;
    // 出参版本：
    // 调用方自己准备输出图像，pipeline 负责填充。
    virtual bool GetLatestFrame(common::AxImage& output_image) = 0;
    // 设置 frame callback。
    // 只有注册回调时，pipeline 才会为回调额外准备输出图像。
    virtual void SetFrameCallback(FrameCallback callback) = 0;
    // 设置异步 OSD。
    // 当前 AXCL 后端仅支持 rect；其余平台能力以各自实现为准。
    virtual bool SetOsd(const common::DrawFrame& osd) = 0;
    virtual void ClearOsd() noexcept = 0;

    virtual PipelineStats GetStats() const = 0;

    // --- Dynamic output management (optional) ---
    // Some applications want to add/remove encoding+mux outputs without restarting demux/vdec.
    // Default implementation returns false (not supported).
    virtual bool AddOutput(const PipelineOutputConfig& /*output*/,
                           std::size_t* /*out_index*/,
                           std::string* error) {
        if (error) *error = "AddOutput not supported";
        return false;
    }
    virtual bool RemoveOutput(std::size_t /*index*/, std::string* error) {
        if (error) *error = "RemoveOutput not supported";
        return false;
    }
};

std::unique_ptr<Pipeline> CreatePipeline();

}  // namespace axvsdk::pipeline
