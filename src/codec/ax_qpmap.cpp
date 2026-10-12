#include "ax_qpmap.h"

#include <algorithm>
#include <cstring>

namespace axvsdk::codec::internal {

namespace {

constexpr std::uint32_t kQpMapUnitPixels = 16;  // QPMAP 的 QP 粒度
constexpr std::uint32_t kBlockPixels = 8;       // 一个字节描述的块边长

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t align) noexcept {
    return align == 0 ? value : ((value + align - 1) / align) * align;
}

// 把一个 16x16 单元写进 CTB-major 排布的缓冲。
void WriteUnit(std::int8_t* map, const QpMapLayout& layout,
               std::uint32_t col, std::uint32_t row, std::int32_t qp) noexcept {
    const auto blks_per_ctb = layout.max_cu_size / kBlockPixels;
    const auto blks_per_unit = kQpMapUnitPixels / kBlockPixels;
    const auto ctb_per_row = layout.cols * kQpMapUnitPixels / layout.max_cu_size;
    const auto ctb_row = row * blks_per_unit / blks_per_ctb;
    const auto ctb_col = col * blks_per_unit / blks_per_ctb;
    const auto ctb_row_stride = ctb_per_row * blks_per_ctb * blks_per_ctb;
    const auto x_offset = (col * blks_per_unit) % blks_per_ctb;
    const auto y_offset = (row * blks_per_unit) % blks_per_ctb;

    const auto encoded = EncodeAbsQp(qp);
    const std::size_t base = static_cast<std::size_t>(ctb_row) * ctb_row_stride +
                             static_cast<std::size_t>(ctb_col) * blks_per_ctb * blks_per_ctb +
                             static_cast<std::size_t>(y_offset) * blks_per_ctb + x_offset;
    for (std::uint32_t y = 0; y < blks_per_unit; ++y) {
        const std::size_t row_base = base + static_cast<std::size_t>(y) * blks_per_ctb;
        for (std::uint32_t x = 0; x < blks_per_unit; ++x) {
            const std::size_t index = row_base + x;
            if (index >= layout.size) continue;
            const std::int32_t old_qp = (static_cast<std::uint8_t>(map[index]) >> 1) & 0x3f;
            if (qp < old_qp) map[index] = encoded;
        }
    }
}

}  // namespace

QpMapLayout MakeQpMapLayout(VideoCodecType codec, std::uint32_t width, std::uint32_t height) noexcept {
    QpMapLayout layout{};
    if (width == 0 || height == 0) return layout;
    layout.max_cu_size = (codec == VideoCodecType::kH264) ? 16U : 64U;
    const auto map_width = AlignUp(width, layout.max_cu_size);
    const auto map_height = AlignUp(height, layout.max_cu_size);
    layout.size = static_cast<std::size_t>(map_width) * map_height / (kBlockPixels * kBlockPixels);
    layout.cols = map_width / kQpMapUnitPixels;
    layout.rows = map_height / kQpMapUnitPixels;
    return layout;
}

std::int8_t EncodeAbsQp(std::int32_t qp) noexcept {
    qp = std::min(51, std::max(0, qp));
    return static_cast<std::int8_t>(((qp & 0x3f) << 1) | 1);
}

void FillQpMap(std::int8_t* map, const QpMapLayout& layout, std::int32_t qp) noexcept {
    if (map == nullptr || !layout.valid()) return;
    std::memset(map, EncodeAbsQp(qp), layout.size);
}

void ApplyQpMapRois(std::int8_t* map,
                    const QpMapLayout& layout,
                    const std::vector<QpRoiRect>& rois,
                    std::int32_t roi_qp,
                    std::uint32_t image_width,
                    std::uint32_t image_height) noexcept {
    if (map == nullptr || !layout.valid() || rois.empty()) return;

    for (const auto& roi : rois) {
        if (roi.width == 0 || roi.height == 0) continue;
        std::int64_t x0 = roi.x;
        std::int64_t y0 = roi.y;
        std::int64_t x1 = x0 + static_cast<std::int64_t>(roi.width);
        std::int64_t y1 = y0 + static_cast<std::int64_t>(roi.height);
        x0 = std::max<std::int64_t>(x0, 0);
        y0 = std::max<std::int64_t>(y0, 0);
        if (image_width != 0) x1 = std::min<std::int64_t>(x1, image_width);
        if (image_height != 0) y1 = std::min<std::int64_t>(y1, image_height);
        if (x1 <= x0 || y1 <= y0) continue;

        // 边界向外取整到整单元:部分覆盖的单元也算 ROI。
        const auto col_begin = static_cast<std::uint32_t>(x0) / kQpMapUnitPixels;
        const auto row_begin = static_cast<std::uint32_t>(y0) / kQpMapUnitPixels;
        const auto col_end = std::min(layout.cols,
                                      static_cast<std::uint32_t>(x1 - 1) / kQpMapUnitPixels + 1);
        const auto row_end = std::min(layout.rows,
                                      static_cast<std::uint32_t>(y1 - 1) / kQpMapUnitPixels + 1);
        for (std::uint32_t row = row_begin; row < row_end; ++row) {
            for (std::uint32_t col = col_begin; col < col_end; ++col) {
                WriteUnit(map, layout, col, row, roi_qp);
            }
        }
    }
}

}  // namespace axvsdk::codec::internal
