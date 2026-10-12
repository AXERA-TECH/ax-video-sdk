// 运动区域检测:把整帧缩成很小的灰度网格做帧差,命中的格子并入 ROI。
//
// 存在的理由:QPMAP 只保护检测器认识的目标,检测器不认识的运动物体
// (狗、落物、晃动的招牌、推车…)会被当背景用高 QP 糊掉。运动区域兜住这一类。
// 网格很小(默认约为原图 1/32),帧差成本可以忽略,且下采样本身抑制了传感器噪点。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/ax_video_encoder.h"
#include "pipeline/ax_pipeline.h"

namespace axvsdk::pipeline::internal {

class MotionRoiDetector {
public:
    // cols/rows 为网格尺寸,src_w/src_h 为原图尺寸(用于把格子换算回像素坐标)。
    bool Reset(std::uint32_t cols, std::uint32_t rows,
               std::uint32_t src_width, std::uint32_t src_height,
               const MotionRoiOptions& options);
    void Clear() noexcept;

    bool valid() const noexcept { return cols_ != 0 && rows_ != 0; }
    std::uint32_t cols() const noexcept { return cols_; }
    std::uint32_t rows() const noexcept { return rows_; }

    // 喂一帧网格灰度(每字节一个格子,stride 可大于 cols)。
    void Update(const std::uint8_t* luma, std::size_t stride);

    // 当前处于"运动中"的区域,像素坐标,已按行合并以减少矩形数量。
    std::vector<codec::QpRoiRect> rects() const;

private:
    MotionRoiOptions options_{};
    std::uint32_t cols_{0};
    std::uint32_t rows_{0};
    std::uint32_t src_width_{0};
    std::uint32_t src_height_{0};
    std::vector<std::uint8_t> prev_;
    std::vector<std::uint16_t> hold_;  // 每格剩余保持帧数
    bool has_prev_{false};
};

}  // namespace axvsdk::pipeline::internal
