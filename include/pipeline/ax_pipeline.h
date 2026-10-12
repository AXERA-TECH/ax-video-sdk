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
    // 目标区域差异化量化(码率花在目标上,背景粗编)。ROI 用 Pipeline::SetQpRoi 下发。
    codec::QpMapOptions qpmap{};
    // 码率目标按"相对输入码流压缩多少倍"给,例如 4.0 = 输出压到输入码率的 1/4。
    // 比直接调 QP 好用:部署现场关心的是"能不能省下 3/4 的带宽和存储",不是 QP 取多少。
    // 需要 qpmap.enable;非 0 时覆盖 qpmap.target_bitrate_kbps,并随实测输入码率滚动更新
    // (RTSP 源的码率本身是变的)。
    double compression_ratio{0.0};
};

struct PipelineFrameOutputConfig {
    // GetLatestFrame / frame callback 的输出图像描述。
    // format/width/height 为空时默认跟随解码原图，通常为 NV12。
    common::ImageDescriptor output_image{};
    // frame output 的缩放策略。
    common::ResizeOptions resize{};
};

// 运动区域并入 ROI。检测器只认识它训练过的类别,画面里其它运动物体
// (动物、落物、晃动的招牌…)若只靠检测框会被当背景用高 QP 糊掉。
// 开销:每 detect_interval 帧一次 IVPS 缩图 + 一次极小的 host 帧差。
struct MotionRoiOptions {
    bool enable{false};
    // 网格尺寸,0 = 自动(原图 1/16,正好对齐 QPMAP 单元)。
    // 会被钳到不低于 1/16:IVPS 单次缩放做不到更高倍率。
    std::uint32_t grid_cols{0};
    std::uint32_t grid_rows{0};
    // 格子亮度差超过它算运动(0~255)。
    std::uint32_t sensitivity{12};
    // 命中后保持多少帧,既防闪烁也顺带覆盖目标的去向。
    std::uint32_t hold_frames{15};
    // 每隔几帧算一次。运动区域变化慢,不必逐帧。
    std::uint32_t detect_interval{3};
    // ROI 面积占画面的硬上限(百分比,0 = 不限)。超出时只保留帧差最大的那些格子,
    // 其余退回背景 QP。繁忙场景下运动区域能吃掉大半画面,智能编码退化成全图高质量,
    // 码率闭环也就压不下去了;这个上限保证"花在目标上的码率"始终有边界。
    std::uint32_t max_coverage_percent{40};
};

struct PipelineConfig {
    // AXCL 下建议显式指定 device_id；同一个 Pipeline 只绑定一张卡。
    std::int32_t device_id{-1};
    PipelineInputConfig input{};
    // 允许 1 路输入对应 N 路输出。
    std::vector<PipelineOutputConfig> outputs;
    // 控制对外 latest frame / frame callback 的输出格式和尺寸。
    PipelineFrameOutputConfig frame_output{};
    // 运动区域并入 QPMAP 的 ROI(仅对开启了 qpmap 的输出分支有意义)。
    MotionRoiOptions motion_roi{};
};

struct PipelineStats {
    std::uint64_t decoded_frames{0};
    std::uint64_t branch_submit_failures{0};
    // 最新检测结果相对当前帧滞后了几帧(见 Pipeline::SetDetectionSourceSeq)。
    // 异步推理下这个值天然 > 0;给 ROI 编码等"按位置使用检测结果"的功能做补偿依据。
    std::uint64_t det_lag_frames{0};
    std::uint64_t det_lag_frames_max{0};
    // 输入码流的实测码率(kbps,1 秒窗口)。compression_ratio 的分母。
    std::uint32_t input_bitrate_kbps{0};
    // 运动 ROI 当前覆盖画面的比例(百分比)。未开启运动 ROI 时为 0。
    std::uint32_t motion_coverage_percent{0};
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
    // 记录"最新一次检测结果基于哪个解码帧(AxImage::seq)算出"。
    // 推理结果一到就调,**与该结果是否产生了检测框无关**——画面里暂时没目标
    // 不等于结果过期。供 PipelineStats::det_lag_frames 统计。
    virtual void SetDetectionSourceSeq(std::uint64_t source_seq) { (void)source_seq; }

    // 下发 ROI 给所有已配置 qpmap 的输出分支。坐标统一用**解码原图**坐标系,
    // pipeline 会按各分支的编码尺寸自动缩放。传空表示整帧按背景 QP。
    virtual void SetQpRoi(const std::vector<codec::QpRoiRect>& rois) { (void)rois; }
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
