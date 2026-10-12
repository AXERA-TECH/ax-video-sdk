// VENC ABS QPMAP 的平台无关构建:算布局、填背景 QP、按 ROI 覆写。
// 缓冲格式由硬件规定:每字节描述一个 8x8 块,按 CTB 分块存放
// (H.264 CTB=16,H.265 CTB=64),QP 粒度固定 16x16 像素一个单元。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/ax_video_encoder.h"

namespace axvsdk::codec::internal {

struct QpMapLayout {
    std::uint32_t max_cu_size{16};  // CTB 边长(像素)
    std::uint32_t cols{0};          // 16x16 单元列数
    std::uint32_t rows{0};          // 16x16 单元行数
    std::size_t size{0};            // 缓冲字节数

    bool valid() const noexcept { return size != 0 && cols != 0 && rows != 0; }
};

QpMapLayout MakeQpMapLayout(VideoCodecType codec, std::uint32_t width, std::uint32_t height) noexcept;

// qp -> QPMAP 字节。最低位是 valid 标志,不是直接写数值。
std::int8_t EncodeAbsQp(std::int32_t qp) noexcept;

void FillQpMap(std::int8_t* map, const QpMapLayout& layout, std::int32_t qp) noexcept;

// 在 ROI 覆盖到的单元上写 roi_qp。单元只要与 ROI 有交集就算覆盖(宁可多框),
// 重叠处取更小的 QP(更清晰的那个赢)。
void ApplyQpMapRois(std::int8_t* map,
                    const QpMapLayout& layout,
                    const std::vector<QpRoiRect>& rois,
                    std::int32_t roi_qp,
                    std::uint32_t image_width,
                    std::uint32_t image_height) noexcept;

}  // namespace axvsdk::codec::internal
