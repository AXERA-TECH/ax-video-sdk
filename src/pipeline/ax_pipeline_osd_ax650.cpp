#include "common/ax_drawer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "ax_ivps_lock.h"

#include "ax_ivps_api.h"
#include "ax_sys_api.h"

#include "ax_image_internal.h"
#include "common/ax_system.h"

namespace axvsdk::common::internal {

namespace {

constexpr AX_U32 kBitmapMemoryAlignment = 0x1000;

struct BitmapMemory {
    ~BitmapMemory() {
        if (phy_addr != 0 && vir_addr != nullptr) {
            (void)AX_SYS_MemFree(phy_addr, vir_addr);
        }
    }

    AX_U64 phy_addr{0};
    AX_VOID* vir_addr{nullptr};
};

struct PreparedBitmap {
    AX_OSD_BMP_ATTR_T attr{};
    std::shared_ptr<BitmapMemory> memory;
};

bool FitsAxCoordinate(std::int32_t value) noexcept {
    return value >= 0 && value <= std::numeric_limits<AX_S16>::max();
}

bool FitsAxSize(std::uint32_t value) noexcept {
    return value > 0 && value <= static_cast<std::uint32_t>(std::numeric_limits<AX_U16>::max());
}

AX_IMG_FORMAT_E ToAxBitmapFormat(DrawBitmapFormat format) noexcept {
    switch (format) {
    case DrawBitmapFormat::kArgb8888:
        return AX_FORMAT_ARGB8888;
    case DrawBitmapFormat::kRgba8888:
        return AX_FORMAT_RGBA8888;
    case DrawBitmapFormat::kArgb1555:
        return AX_FORMAT_ARGB1555;
    case DrawBitmapFormat::kRgba5551:
        return AX_FORMAT_RGBA5551;
    case DrawBitmapFormat::kArgb4444:
        return AX_FORMAT_ARGB4444;
    case DrawBitmapFormat::kRgba4444:
        return AX_FORMAT_RGBA4444;
    case DrawBitmapFormat::kArgb8565:
        return AX_FORMAT_ARGB8565;
    case DrawBitmapFormat::kRgb888:
        return AX_FORMAT_RGB888;
    case DrawBitmapFormat::kRgb565:
        return AX_FORMAT_RGB565;
    case DrawBitmapFormat::kBitmap1:
        return AX_FORMAT_BITMAP;
    default:
        return AX_FORMAT_INVALID;
    }
}

std::size_t BytesPerPixel(DrawBitmapFormat format) noexcept {
    switch (format) {
    case DrawBitmapFormat::kArgb8888:
    case DrawBitmapFormat::kRgba8888:
        return 4;
    case DrawBitmapFormat::kArgb1555:
    case DrawBitmapFormat::kRgba5551:
    case DrawBitmapFormat::kArgb4444:
    case DrawBitmapFormat::kRgba4444:
    case DrawBitmapFormat::kRgb565:
        return 2;
    case DrawBitmapFormat::kArgb8565:
    case DrawBitmapFormat::kRgb888:
        return 3;
    case DrawBitmapFormat::kBitmap1:
    default:
        return 0;
    }
}

std::size_t BitmapRowBytes(const DrawBitmap& bitmap) noexcept {
    if (bitmap.width == 0) {
        return 0;
    }

    if (bitmap.format == DrawBitmapFormat::kBitmap1) {
        return (bitmap.width + 7U) / 8U;
    }

    return static_cast<std::size_t>(bitmap.width) * BytesPerPixel(bitmap.format);
}

std::size_t BitmapByteSize(const DrawBitmap& bitmap) noexcept {
    const auto row_bytes = BitmapRowBytes(bitmap);
    if (row_bytes == 0 || bitmap.height == 0) {
        return 0;
    }
    return row_bytes * bitmap.height;
}

AX_IVPS_MOSAIC_BLK_SIZE_E ToAxBlockSize(DrawMosaicBlockSize size) noexcept {
    switch (size) {
    case DrawMosaicBlockSize::k2:
        return AX_IVPS_MOSAIC_BLK_SIZE_2;
    case DrawMosaicBlockSize::k4:
        return AX_IVPS_MOSAIC_BLK_SIZE_4;
    case DrawMosaicBlockSize::k8:
        return AX_IVPS_MOSAIC_BLK_SIZE_8;
    case DrawMosaicBlockSize::k16:
        return AX_IVPS_MOSAIC_BLK_SIZE_16;
    case DrawMosaicBlockSize::k32:
        return AX_IVPS_MOSAIC_BLK_SIZE_32;
    case DrawMosaicBlockSize::k64:
    default:
        return AX_IVPS_MOSAIC_BLK_SIZE_64;
    }
}

AX_IVPS_GDI_ATTR_T MakeGdiAttr(std::uint16_t thickness,
                               std::uint8_t alpha,
                               std::uint32_t color,
                               bool filled) noexcept {
    AX_IVPS_GDI_ATTR_T attr{};
    attr.nThick = thickness;
    attr.nAlpha = alpha;
    attr.nColor = color;
    attr.bSolid = filled ? AX_TRUE : AX_FALSE;
    attr.bAbsCoo = AX_FALSE;
    return attr;
}

bool ResolveFrame(common::AxImage& image, AX_VIDEO_FRAME_T* frame) {
    if (frame == nullptr) {
        return false;
    }

    *frame = common::internal::AxImageAccess::GetAxFrame(image);
    if (frame->u64VirAddr[0] != 0 || frame->u32BlkId[0] == AX_INVALID_BLOCKID) {
        return true;
    }

    AX_VOID* base_vir_addr = AX_POOL_GetBlockVirAddr(frame->u32BlkId[0]);
    if (base_vir_addr == nullptr) {
        return false;
    }

    const auto base_phy_addr = frame->u64PhyAddr[0];
    const auto base_vir = reinterpret_cast<std::uintptr_t>(base_vir_addr);
    for (std::size_t plane = 0; plane < common::kMaxImagePlanes; ++plane) {
        if (frame->u64PhyAddr[plane] == 0) {
            continue;
        }
        frame->u64VirAddr[plane] =
            static_cast<AX_U64>(base_vir + static_cast<std::uintptr_t>(frame->u64PhyAddr[plane] - base_phy_addr));
    }

    return true;
}

bool BuildCanvas(common::AxImage& image, AX_IVPS_RGN_CANVAS_INFO_T* canvas) {
    if (canvas == nullptr) {
        return false;
    }

    AX_VIDEO_FRAME_T frame{};
    if (!ResolveFrame(image, &frame) || frame.u64VirAddr[0] == 0) {
        return false;
    }

    std::memset(canvas, 0, sizeof(*canvas));
    canvas->nPhyAddr = frame.u64PhyAddr[0];
    canvas->pVirAddr = reinterpret_cast<AX_VOID*>(static_cast<std::uintptr_t>(frame.u64VirAddr[0]));
    canvas->nStride = frame.u32PicStride[0];
    canvas->nW = static_cast<AX_U16>(frame.u32Width);
    // MSP region draw APIs are not fully consistent across versions:
    // some builds ignore nUVOffset and derive the UV base as `pVirAddr + nStride * nH`,
    // while others mis-handle non-zero nUVOffset and can corrupt the frame.
    // Follow MSP samples: compute nH from the UV physical offset when possible and keep nUVOffset=0.
    AX_U16 canvas_h = static_cast<AX_U16>(frame.u32Height);
    if (frame.u32PicStride[0] != 0 && frame.u64PhyAddr[1] > frame.u64PhyAddr[0]) {
        const AX_U64 delta = frame.u64PhyAddr[1] - frame.u64PhyAddr[0];
        const AX_U64 h64 = delta / static_cast<AX_U64>(frame.u32PicStride[0]);
        if (h64 > 0 && h64 <= static_cast<AX_U64>(std::numeric_limits<AX_U16>::max())) {
            canvas_h = static_cast<AX_U16>(h64);
        }
    }
    canvas->nH = canvas_h;
    canvas->nUVOffset = 0;
    canvas->eFormat = frame.enImgFormat;
    return true;
}

bool MakeAxPoints(const std::vector<DrawPoint>& points, std::vector<AX_IVPS_POINT_T>* ax_points) {
    if (ax_points == nullptr || points.empty()) {
        return false;
    }

    ax_points->clear();
    ax_points->reserve(points.size());
    for (const auto& point : points) {
        if (!FitsAxCoordinate(point.x) || !FitsAxCoordinate(point.y)) {
            return false;
        }

        AX_IVPS_POINT_T ax_point{};
        ax_point.nX = static_cast<AX_S16>(point.x);
        ax_point.nY = static_cast<AX_S16>(point.y);
        ax_points->push_back(ax_point);
    }

    return true;
}

bool PrepareBitmap(const DrawBitmap& bitmap, PreparedBitmap* prepared_bitmap) {
    if (prepared_bitmap == nullptr || bitmap.width == 0 || bitmap.height == 0) {
        return false;
    }

    const auto format = ToAxBitmapFormat(bitmap.format);
    const auto row_bytes = BitmapRowBytes(bitmap);
    const auto byte_size = BitmapByteSize(bitmap);
    if (format == AX_FORMAT_INVALID || row_bytes == 0 || byte_size == 0 || bitmap.data.size() != byte_size) {
        return false;
    }

    if ((row_bytes % 16U) != 0U) {
        std::fprintf(stderr, "pipeline osd ax650: bitmap row bytes must be 16-byte aligned, got %zu\n", row_bytes);
        return false;
    }

    auto memory = std::make_shared<BitmapMemory>();
    const auto* token = reinterpret_cast<const AX_S8*>("PipelineOsdBmp");
    if (AX_SYS_MemAlloc(&memory->phy_addr, &memory->vir_addr, static_cast<AX_U32>(byte_size), kBitmapMemoryAlignment,
                        token) != AX_SUCCESS) {
        return false;
    }

    std::memcpy(memory->vir_addr, bitmap.data.data(), byte_size);
    if (AX_SYS_MflushCache(memory->phy_addr, memory->vir_addr, static_cast<AX_U32>(byte_size)) != AX_SUCCESS) {
        return false;
    }

    std::memset(&prepared_bitmap->attr, 0, sizeof(prepared_bitmap->attr));
    prepared_bitmap->attr.u16Alpha = bitmap.alpha;
    prepared_bitmap->attr.enRgbFormat = format;
    prepared_bitmap->attr.pBitmap = static_cast<AX_U8*>(memory->vir_addr);
    prepared_bitmap->attr.u64PhyAddr = memory->phy_addr;
    prepared_bitmap->attr.u32BmpWidth = bitmap.width;
    prepared_bitmap->attr.u32BmpHeight = bitmap.height;
    prepared_bitmap->attr.u32DstXoffset = bitmap.dst_x;
    prepared_bitmap->attr.u32DstYoffset = bitmap.dst_y;
    prepared_bitmap->attr.u32Color = bitmap.color;
    prepared_bitmap->attr.bColorInv = bitmap.color_invert ? AX_TRUE : AX_FALSE;
    prepared_bitmap->attr.u32ColorInv = bitmap.color_invert_value;
    prepared_bitmap->attr.u32ColorInvThr = bitmap.color_invert_threshold;
    prepared_bitmap->memory = std::move(memory);
    return true;
}

class PreparedAx650DrawCommands final : public PreparedDrawCommands {
public:
    explicit PreparedAx650DrawCommands(std::uint32_t hold_frames) : hold_frames_(hold_frames) {}

    std::uint32_t hold_frames() const noexcept override {
        return hold_frames_;
    }

    bool Apply(common::AxImage& image) const override {
        if (!common::IsSystemInitialized()) {
            return false;
        }

        // Serialize IVPS draw operations with other IVPS users (e.g. pre-process) to avoid MSP thread-safety issues.
        // For maximum throughput, serialization is disabled by default.
        // If you hit instability on a specific MSP/driver version, set AXVSDK_IVPS_SERIALIZE=1 (or AXP_IVPS_SERIALIZE=1).
        std::unique_lock<std::mutex> ivps_lock(common::internal::IvpsGlobalMutex(), std::defer_lock);
        if (common::internal::IvpsSerializeEnabled()) {
            ivps_lock.lock();
        }

        AX_VIDEO_FRAME_T frame{};
        if (!ResolveFrame(image, &frame)) {
            return false;
        }

        if (!mosaics_.empty()) {
            auto mosaics = mosaics_;
            if (AX_IVPS_DrawMosaicTdp(&frame, mosaics.data(), static_cast<AX_U32>(mosaics.size())) != AX_SUCCESS) {
                return false;
            }
        }

        if (!bitmaps_.empty()) {
            std::vector<AX_OSD_BMP_ATTR_T> attrs;
            attrs.reserve(bitmaps_.size());
            for (const auto& bitmap : bitmaps_) {
                attrs.push_back(bitmap.attr);
            }

            if (AX_IVPS_DrawOsdTdp(&frame, attrs.data(), static_cast<AX_U32>(attrs.size())) != AX_SUCCESS) {
                return false;
            }
        }

        if (lines_.empty() && polygons_.empty() && rects_.empty()) {
            return true;
        }

        if (!image.InvalidateCache()) {
            return false;
        }

        AX_IVPS_RGN_CANVAS_INFO_T canvas{};
        if (!BuildCanvas(image, &canvas)) {
            return false;
        }

        std::vector<AX_IVPS_POINT_T> ax_points;
        for (const auto& line : lines_) {
            if (line.points.size() < 2 || !MakeAxPoints(line.points, &ax_points)) {
                return false;
            }

            const auto attr = MakeGdiAttr(line.thickness, line.alpha, line.color, false);
            if (AX_IVPS_DrawLine(&canvas, attr, ax_points.data(), static_cast<AX_U32>(ax_points.size())) != AX_SUCCESS) {
                return false;
            }
        }

        for (const auto& polygon : polygons_) {
            if (polygon.points.size() < AX_IVPS_MIN_POLYGON_POINT_NUM ||
                polygon.points.size() > AX_IVPS_MAX_POLYGON_POINT_NUM ||
                !MakeAxPoints(polygon.points, &ax_points)) {
                return false;
            }

            const auto attr = MakeGdiAttr(polygon.thickness, polygon.alpha, polygon.color, polygon.filled);
            if (AX_IVPS_DrawPolygon(&canvas, attr, ax_points.data(), static_cast<AX_U32>(ax_points.size())) !=
                AX_SUCCESS) {
                return false;
            }
        }

        for (const auto& rect : rects_) {
            if (!FitsAxCoordinate(rect.x) || !FitsAxCoordinate(rect.y) || !FitsAxSize(rect.width) ||
                !FitsAxSize(rect.height)) {
                return false;
            }

            auto attr = MakeGdiAttr(rect.thickness, rect.alpha, rect.color, rect.filled);
            if (rect.corner_only) {
                attr.tCornerRect.bEnable = AX_TRUE;
                attr.tCornerRect.nHorLength = rect.corner_horizontal_length;
                attr.tCornerRect.nVerLength = rect.corner_vertical_length;
            }

            AX_IVPS_RECT_T ax_rect{};
            ax_rect.nX = static_cast<AX_S16>(rect.x);
            ax_rect.nY = static_cast<AX_S16>(rect.y);
            ax_rect.nW = static_cast<AX_U16>(rect.width);
            ax_rect.nH = static_cast<AX_U16>(rect.height);
            if (AX_IVPS_DrawRect(&canvas, attr, ax_rect) != AX_SUCCESS) {
                return false;
            }
        }

        return image.FlushCache();
    }

    std::vector<DrawLine> lines_;
    std::vector<DrawPolygon> polygons_;
    std::vector<DrawRect> rects_;
    std::vector<AX_IVPS_RGN_MOSAIC_T> mosaics_;
    std::vector<PreparedBitmap> bitmaps_;

private:
    std::uint32_t hold_frames_{1};
};

class Ax650Drawer final : public AxDrawer {
public:
    std::shared_ptr<const PreparedDrawCommands> Prepare(const DrawFrame& frame) override {
        auto prepared = std::make_shared<PreparedAx650DrawCommands>(frame.hold_frames);
        prepared->lines_ = frame.lines;
        prepared->polygons_ = frame.polygons;
        prepared->rects_ = frame.rects;

        prepared->mosaics_.reserve(frame.mosaics.size());
        for (const auto& mosaic : frame.mosaics) {
            if (!FitsAxCoordinate(mosaic.x) || !FitsAxCoordinate(mosaic.y) || !FitsAxSize(mosaic.width) ||
                !FitsAxSize(mosaic.height)) {
                return nullptr;
            }

            AX_IVPS_RGN_MOSAIC_T ax_mosaic{};
            ax_mosaic.tRect.nX = static_cast<AX_S16>(mosaic.x);
            ax_mosaic.tRect.nY = static_cast<AX_S16>(mosaic.y);
            ax_mosaic.tRect.nW = static_cast<AX_U16>(mosaic.width);
            ax_mosaic.tRect.nH = static_cast<AX_U16>(mosaic.height);
            ax_mosaic.eBklSize = ToAxBlockSize(mosaic.block_size);
            prepared->mosaics_.push_back(ax_mosaic);
        }

        prepared->bitmaps_.reserve(frame.bitmaps.size());
        for (const auto& bitmap : frame.bitmaps) {
            PreparedBitmap prepared_bitmap{};
            if (!PrepareBitmap(bitmap, &prepared_bitmap)) {
                return nullptr;
            }
            prepared->bitmaps_.push_back(std::move(prepared_bitmap));
        }

        return prepared;
    }

    bool Draw(const PreparedDrawCommands& commands, AxImage& image) override {
        return commands.Apply(image);
    }
};

}  // namespace

std::unique_ptr<AxDrawer> CreatePlatformDrawer() {
    return std::make_unique<Ax650Drawer>();
}

}  // namespace axvsdk::common::internal
