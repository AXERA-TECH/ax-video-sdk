#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/ax_drawer.h"
#include "common/ax_image.h"
#include "pipeline/ax_muxer.h"
#include "pipeline/ax_pipeline.h"
#include "codec/ax_video_encoder.h"

namespace axvsdk::pipeline {

// 摄像头 Pipeline 输出配置（复用现有配置）
using CameraPipelineOutputConfig = PipelineOutputConfig;

struct CameraPipelineConfig {
    std::int32_t device_id{-1};
    // V4L2 设备路径
    std::string device_path{"/dev/video0"};
    // 期望捕获参数
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    double fps{30.0};
    // 输出配置（支持多路输出）
    std::vector<CameraPipelineOutputConfig> outputs;
    // frame callback 输出配置（给 NPU 用）
    struct FrameOutputConfig {
        common::ImageDescriptor output_image{};
        common::ResizeOptions resize{};
    } frame_output{};
};

struct CameraPipelineStats {
    std::uint64_t captured_frames{0};
    std::uint64_t submit_failures{0};
    std::vector<codec::VideoEncoderStats> output_stats;
};

using CameraFrameCallback = std::function<void(common::AxImage::Ptr frame)>;

class CameraPipeline {
public:
    virtual ~CameraPipeline() = default;

    virtual bool Open(const CameraPipelineConfig& config) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    // 获取输入流元信息
    virtual std::uint32_t GetWidth() const noexcept = 0;
    virtual std::uint32_t GetHeight() const noexcept = 0;
    virtual double GetFps() const noexcept = 0;

    // 获取最新一帧（用于显式取帧）
    virtual common::AxImage::Ptr GetLatestFrame() = 0;

    // 设置帧回调（用于 NPU 处理）
    virtual void SetFrameCallback(CameraFrameCallback callback) = 0;

    // OSD 支持
    virtual bool SetOsd(const common::DrawFrame& osd) = 0;
    virtual void ClearOsd() noexcept = 0;

    virtual CameraPipelineStats GetStats() const = 0;
};

std::unique_ptr<CameraPipeline> CreateCameraPipeline();

}  // namespace axvsdk::pipeline
