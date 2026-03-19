#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/ax_image.h"

namespace axvsdk::common {

struct DrawPoint {
    std::int32_t x{0};
    std::int32_t y{0};
};

struct DrawLine {
    std::vector<DrawPoint> points;
    std::uint16_t thickness{2};
    std::uint8_t alpha{255};
    std::uint32_t color{0xFF0000};
};

struct DrawPolygon {
    std::vector<DrawPoint> points;
    std::uint16_t thickness{2};
    std::uint8_t alpha{255};
    std::uint32_t color{0xFF0000};
    bool filled{false};
};

struct DrawRect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint16_t thickness{2};
    std::uint8_t alpha{255};
    std::uint32_t color{0xFF0000};
    bool filled{false};
    bool corner_only{false};
    std::uint16_t corner_horizontal_length{0};
    std::uint16_t corner_vertical_length{0};
};

enum class DrawMosaicBlockSize {
    k2 = 2,
    k4 = 4,
    k8 = 8,
    k16 = 16,
    k32 = 32,
    k64 = 64,
};

struct DrawMosaic {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    DrawMosaicBlockSize block_size{DrawMosaicBlockSize::k16};
};

enum class DrawBitmapFormat {
    kArgb8888 = 0,
    kRgba8888,
    kArgb1555,
    kRgba5551,
    kArgb4444,
    kRgba4444,
    kArgb8565,
    kRgb888,
    kRgb565,
    kBitmap1,
};

struct DrawBitmap {
    DrawBitmapFormat format{DrawBitmapFormat::kArgb8888};
    std::uint16_t alpha{255};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t dst_x{0};
    std::uint32_t dst_y{0};
    std::uint32_t color{0xFFFFFF};
    bool color_invert{false};
    std::uint32_t color_invert_value{0};
    std::uint32_t color_invert_threshold{0};
    std::vector<std::uint8_t> data;
};

struct DrawFrame {
    std::uint32_t hold_frames{1};
    std::vector<DrawLine> lines;
    std::vector<DrawPolygon> polygons;
    std::vector<DrawRect> rects;
    std::vector<DrawMosaic> mosaics;
    std::vector<DrawBitmap> bitmaps;
};

class PreparedDrawCommands {
public:
    virtual ~PreparedDrawCommands() = default;

    virtual std::uint32_t hold_frames() const noexcept = 0;
    virtual bool Apply(AxImage& image) const = 0;
};

class AxDrawer {
public:
    virtual ~AxDrawer() = default;

    virtual std::shared_ptr<const PreparedDrawCommands> Prepare(const DrawFrame& frame) = 0;
    virtual bool Draw(const PreparedDrawCommands& commands, AxImage& image) = 0;

    bool Draw(const DrawFrame& frame, AxImage& image) {
        auto prepared = Prepare(frame);
        return prepared && Draw(*prepared, image);
    }
};

std::unique_ptr<AxDrawer> CreateDrawer();

}  // namespace axvsdk::common
