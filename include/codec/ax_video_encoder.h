#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

#include "codec/ax_codec_types.h"
#include "common/ax_image.h"
#include "common/ax_image_processor.h"

namespace axvsdk::codec {

// 输入队列满时的处理策略。
enum class QueueOverflowPolicy {
    // 丢弃最旧的待编码帧，优先保留最新画面。
    kDropOldest = 0,
    // 丢弃当前新送入的帧。
    kDropNewest,
    // 阻塞提交线程，直到队列有空位。
    kBlock,
};

struct VideoEncoderStats {
    std::uint64_t submitted_frames{0};
    std::uint64_t dropped_frames{0};
    std::uint64_t encoded_packets{0};
    std::uint64_t key_packets{0};
    std::size_t current_queue_depth{0};
    std::size_t queue_capacity{0};
    // 最近一个统计窗口(1 秒)的实际输出码率。
    std::uint32_t measured_bitrate_kbps{0};
    // QPMAP 模式下当前使用的背景 QP;码率闭环开启时是闭环结果。未开 qpmap 时为 0。
    std::int32_t qpmap_base_qp{0};
    // 码率闭环已经把 QP 推到 max_qp、ROI 优待也收干了,实测码率仍明显高于目标。
    // 这时候压不下去不是参数没调对,而是这个分辨率/帧率下 QP 已经到头了。
    bool rate_target_unreachable{false};
};

// 目标区域矩形,坐标在编码器输入图像(VideoEncoderConfig 的 width/height)坐标系下。
struct QpRoiRect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// 目标区域差异化量化(VENC ABS QPMAP):把码率花在关心的目标上,背景粗编。
// 开启后码控切到 QPMAP 模式,逐 16x16 单元指定绝对 QP:
//   I 帧        -> i_qp
//   P/B 背景    -> p_qp
//   P/B ROI 区  -> clamp(p_qp + roi_qp_delta, 0, 51)
// QP 越小越清晰,所以 roi_qp_delta 取负值。ROI 由 VideoEncoder::SetQpRoi 随时更新。
struct QpMapOptions {
    bool enable{false};
    std::int32_t i_qp{38};
    std::int32_t p_qp{40};
    std::int32_t roi_qp_delta{-4};
    // 码率闭环。非 0 时不再用固定的 i_qp/p_qp,而是按实测输出码率自动调整背景 QP
    // 去逼近这个目标(kbps);i_qp/p_qp 退化为闭环起点,两者的差值保持不变。
    // ROI 区始终按 roi_qp_delta 相对背景加细,所以"目标清晰、背景够用"的结构不变。
    // 也可以运行时用 VideoEncoder::SetTargetBitrateKbps 改。
    std::uint32_t target_bitrate_kbps{0};
    // 闭环允许的背景 QP 范围。下界防止码率目标过宽时白花码率,上界兜住画质底线。
    std::int32_t min_qp{24};
    std::int32_t max_qp{46};
};

struct VideoEncoderConfig {
    VideoCodecType codec{VideoCodecType::kUnknown};
    std::uint32_t width{0};
    std::uint32_t height{0};
    // 设备索引。AXCL 下建议显式指定；板端通常保持默认值即可。
    std::int32_t device_id{-1};
    // 目标输出帧率。传 0 表示内部按 30fps 或上层输入帧率估算。
    double frame_rate{0.0};
    // 目标码率，单位 kbps。传 0 表示内部根据分辨率和编码类型自动估算。
    std::uint32_t bitrate_kbps{0};
    // GOP 长度。传 0 表示内部按目标帧率自动估算。
    std::uint32_t gop{0};
    // 编码前软件输入队列深度。传 0 表示内部使用默认值。
    std::size_t input_queue_depth{0};
    QueueOverflowPolicy overflow_policy{QueueOverflowPolicy::kDropOldest};
    // 当输入图像与编码器目标尺寸不一致时，内部使用该 resize 策略做预处理。
    // 编码器硬件侧最终输入统一为 NV12。
    common::ResizeOptions resize{};
    // 目标区域差异化量化。enable=false 时维持原有 CBR 行为。
    QpMapOptions qpmap{};
};

using PacketCallback = std::function<void(EncodedPacket packet)>;

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual bool Open(const VideoEncoderConfig& config) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;

    // 提交一帧待编码图像。
    // 调用方可传入 NV12 / RGB / BGR 等图像；若不是目标 NV12 尺寸，
    // 编码器会在内部申请或复用中间图像后再送硬件。
    // 默认情况下，中间图像使用 CMM / 设备侧内存。
    virtual bool SubmitFrame(common::AxImage::Ptr frame) = 0;
    // 获取最近一包编码输出。
    virtual bool GetLatestPacket(EncodedPacket* packet) = 0;
    virtual VideoEncoderStats GetStats() const = 0;
    // 注册码流输出回调。
    // 回调拿到的是 host 侧编码包数据，可直接用于写文件、推流或自定义处理。
    virtual void SetPacketCallback(PacketCallback callback) = 0;

    // 设置后续帧的 ROI(编码输入坐标系),线程安全,可随时替换;传空表示整帧按背景 QP。
    // 仅在 config.qpmap.enable 时生效。
    // 注意:上层若用异步检测的框填这里,框相对正在编码的帧天然滞后
    // (见 PipelineStats::det_lag_frames),应按滞后量外扩后再传入。
    virtual void SetQpRoi(const std::vector<QpRoiRect>& rois) { (void)rois; }

    // 运行时改码率闭环的目标(kbps),0 = 关闭闭环回到固定 QP。仅在 qpmap.enable 时生效。
    // 给"按输入码流压 N 倍"这类上层策略用:输入码率是跑起来才知道的。
    virtual void SetTargetBitrateKbps(std::uint32_t kbps) { (void)kbps; }
};

std::unique_ptr<VideoEncoder> CreateVideoEncoder();

}  // namespace axvsdk::codec
