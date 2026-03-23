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
    // 输入码流信息。
    // 解码器本身只负责 packet -> frame，不负责 demux。
    VideoStreamInfo stream{};
    // GetLatestFrame / callback 的默认输出图像描述。
    // format/width/height 传 0 或 Unknown 表示保持硬件原始输出。
    // 当前硬件原始输出通常为 NV12。
    common::ImageDescriptor output_image{};
    // 设备索引。AXCL 下建议显式指定；板端通常保持默认值即可。
    std::int32_t device_id{-1};
};

using FrameCallback = std::function<void(common::AxImage::Ptr frame)>;

enum class FrameCallbackMode {
    // Only keep the newest frame for the callback thread.
    // If the callback is slow, intermediate frames are dropped.
    kLatest = 0,
    // Deliver frames in-order using an internal bounded queue.
    // If the callback is too slow, the oldest frames are dropped to avoid blocking decode.
    kQueue = 1,
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual bool Open(const VideoDecoderConfig& config) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    virtual bool SubmitPacket(EncodedPacket packet) = 0;
    virtual bool SubmitEndOfStream() = 0;

    // 返回值版本：
    // 返回“最新一帧”语义的图像。
    // 若配置或默认行为要求转换/缩放，则只在真正取帧时进行拷贝和处理。
    // 默认返回库内部新申请的图像；常见默认内存类型为 CMM / 设备侧图像。
    virtual common::AxImage::Ptr GetLatestFrame() = 0;
    // 出参版本：
    // 调用方自行分配输出图像，解码器按 output_image 的格式/尺寸写入。
    // 适合复用目标缓冲，减少重复申请释放。
    virtual bool GetLatestFrame(common::AxImage& output_image) = 0;
    // 注册最新帧回调。
    // 回调与解码线程解耦；慢回调不会阻塞底层解码线程本身。
    // 未注册回调时，不会为了回调额外做图像拷贝。
    virtual void SetFrameCallback(FrameCallback callback) = 0;

    // 高级接口：选择回调帧投递策略。
    // 默认 kLatest 更符合 “获取最新一帧” 语义；pipeline 需要完整帧序列时可用 kQueue。
    virtual void SetFrameCallback(FrameCallback callback, FrameCallbackMode mode) = 0;
};

std::unique_ptr<VideoDecoder> CreateVideoDecoder();

}  // namespace axvsdk::codec
