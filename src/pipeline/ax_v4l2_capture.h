#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "common/ax_image.h"

namespace axvsdk::pipeline {

// V4L2 像素格式
enum class V4l2PixelFormat {
    kUnknown = 0,
    kYuyv,      // YUYV 4:2:2
    kNv12,      // NV12
    kMjpeg,     // MJPEG
};

// V4L2 捕获配置
struct V4l2CaptureConfig {
    std::string device_path{"/dev/video0"};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t fps{30};
    // 优先尝试的格式顺序，空则使用默认优先级
    std::vector<V4l2PixelFormat> preferred_formats;
};

// 捕获帧回调
using V4l2FrameCallback = std::function<void(common::AxImage::Ptr frame, std::uint64_t seq)>;

class V4l2Capture {
public:
    virtual ~V4l2Capture() = default;

    // 打开摄像头并初始化
    virtual bool Open(const V4l2CaptureConfig& config) = 0;
    virtual void Close() noexcept = 0;

    // 开始/停止捕获
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    // 获取实际捕获参数
    virtual std::uint32_t GetWidth() const noexcept = 0;
    virtual std::uint32_t GetHeight() const noexcept = 0;
    virtual std::uint32_t GetFps() const noexcept = 0;
    virtual V4l2PixelFormat GetFormat() const noexcept = 0;

    // 设置帧回调（捕获线程中调用）
    virtual void SetFrameCallback(V4l2FrameCallback callback) = 0;
};

std::unique_ptr<V4l2Capture> CreateV4l2Capture();

}  // namespace axvsdk::pipeline
