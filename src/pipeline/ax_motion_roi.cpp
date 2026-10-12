#include "ax_motion_roi.h"

#include <algorithm>
#include <cstdlib>

namespace axvsdk::pipeline::internal {

namespace {
// 命中格子占比超过这个比例时判定为全局变化(切场景、灯光突变、摄像头晃动)。
// 这种情况下整帧都"运动",ROI 失去意义,不如交给码控和 I 帧处理。
constexpr double kGlobalChangeRatio = 0.6;
}  // namespace

bool MotionRoiDetector::Reset(std::uint32_t cols, std::uint32_t rows,
                              std::uint32_t src_width, std::uint32_t src_height,
                              const MotionRoiOptions& options) {
    Clear();
    if (cols == 0 || rows == 0 || src_width == 0 || src_height == 0) return false;
    options_ = options;
    cols_ = cols;
    rows_ = rows;
    src_width_ = src_width;
    src_height_ = src_height;
    prev_.assign(static_cast<std::size_t>(cols_) * rows_, 0);
    hold_.assign(static_cast<std::size_t>(cols_) * rows_, 0);
    return true;
}

void MotionRoiDetector::Clear() noexcept {
    cols_ = 0;
    rows_ = 0;
    src_width_ = 0;
    src_height_ = 0;
    prev_.clear();
    hold_.clear();
    has_prev_ = false;
}

void MotionRoiDetector::Update(const std::uint8_t* luma, std::size_t stride) {
    if (luma == nullptr || !valid()) return;
    const auto cells = static_cast<std::size_t>(cols_) * rows_;
    const auto threshold = static_cast<int>(options_.sensitivity);
    const auto hold_frames = static_cast<std::uint16_t>(
        options_.hold_frames == 0 ? 1U : options_.hold_frames);

    if (!has_prev_) {
        for (std::uint32_t r = 0; r < rows_; ++r) {
            std::copy_n(luma + r * stride, cols_, prev_.begin() + static_cast<std::size_t>(r) * cols_);
        }
        has_prev_ = true;
        return;
    }

    std::size_t hot = 0;
    for (std::uint32_t r = 0; r < rows_; ++r) {
        const auto* row = luma + r * stride;
        for (std::uint32_t c = 0; c < cols_; ++c) {
            const auto index = static_cast<std::size_t>(r) * cols_ + c;
            const int diff = std::abs(static_cast<int>(row[c]) - static_cast<int>(prev_[index]));
            if (diff > threshold) {
                hold_[index] = hold_frames;
                ++hot;
            } else if (hold_[index] > 0) {
                --hold_[index];
                ++hot;
            }
            prev_[index] = row[c];
        }
    }

    if (cells != 0 && static_cast<double>(hot) / static_cast<double>(cells) > kGlobalChangeRatio) {
        std::fill(hold_.begin(), hold_.end(), 0);
    }
}

std::vector<codec::QpRoiRect> MotionRoiDetector::rects() const {
    std::vector<codec::QpRoiRect> out;
    if (!valid()) return out;

    // 同一行里连续命中的格子合成一个矩形,避免产生成千上万个小框。
    for (std::uint32_t r = 0; r < rows_; ++r) {
        std::uint32_t run_begin = 0;
        bool in_run = false;
        for (std::uint32_t c = 0; c <= cols_; ++c) {
            const bool active =
                c < cols_ && hold_[static_cast<std::size_t>(r) * cols_ + c] > 0;
            if (active && !in_run) {
                run_begin = c;
                in_run = true;
            } else if (!active && in_run) {
                const auto x0 = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(run_begin) * src_width_ / cols_);
                const auto x1 = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(c) * src_width_ / cols_);
                const auto y0 = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(r) * src_height_ / rows_);
                const auto y1 = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(r + 1) * src_height_ / rows_);
                codec::QpRoiRect rect{};
                rect.x = static_cast<std::int32_t>(x0);
                rect.y = static_cast<std::int32_t>(y0);
                rect.width = x1 > x0 ? x1 - x0 : 1;
                rect.height = y1 > y0 ? y1 - y0 : 1;
                out.push_back(rect);
                in_run = false;
            }
        }
    }
    return out;
}

}  // namespace axvsdk::pipeline::internal
