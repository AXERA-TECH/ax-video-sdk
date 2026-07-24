#include "common/ax_drawer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "axcl_ivps.h"
#include "axcl_sys.h"

#include "ax_image_internal.h"
#include "ax_system_internal.h"
#include "common/ax_system.h"

namespace axvsdk::common::internal {

namespace {

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

    AX_VOID* base_vir_addr = AXCL_POOL_GetBlockVirAddr(frame->u32BlkId[0]);
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
    canvas->nUVOffset =
        frame.u64PhyAddr[1] > frame.u64PhyAddr[0] ? static_cast<AX_U32>(frame.u64PhyAddr[1] - frame.u64PhyAddr[0]) : 0;
    canvas->nStride = frame.u32PicStride[0];
    canvas->nW = static_cast<AX_U16>(frame.u32Width);
    canvas->nH = static_cast<AX_U16>(frame.u32Height);
    canvas->eFormat = frame.enImgFormat;
    return true;
}

class PreparedAxclDrawCommands final : public PreparedDrawCommands {
public:
    explicit PreparedAxclDrawCommands(std::uint32_t hold_frames) : hold_frames_(hold_frames) {}

    std::uint32_t hold_frames() const noexcept override {
        return hold_frames_;
    }

    bool Apply(common::AxImage& image) const override {
        if (!common::IsSystemInitialized() || !common::internal::EnsureAxclThreadContext()) {
            return false;
        }
        if (rects_.empty()) {
            return true;
        }

        // AXCL IVPS draw APIs are not guaranteed to be thread-safe across multiple pipelines.
        // Serialize draw calls to avoid sporadic failures under multi-stream load.
        static std::mutex g_draw_mutex;
        std::lock_guard<std::mutex> lock(g_draw_mutex);

        if (!image.InvalidateCache()) {
            return false;
        }

        AX_IVPS_RGN_CANVAS_INFO_T canvas{};
        if (!BuildCanvas(image, &canvas)) {
            return false;
        }

        const bool osd_debug = (std::getenv("AXVSDK_OSD_DEBUG") != nullptr);
        std::size_t rect_skipped = 0;
        for (const auto& rect : rects_) {
            if (rect.width == 0 || rect.height == 0) {
                continue;  // nothing to draw
            }

            auto attr = MakeGdiAttr(rect.thickness, rect.alpha, rect.color, rect.filled);
            if (rect.corner_only) {
                attr.tCornerRect.bEnable = AX_TRUE;
                attr.tCornerRect.nHorLength = rect.corner_horizontal_length;
                attr.tCornerRect.nVerLength = rect.corner_vertical_length;
            }

            // Sanitize rectangle geometry inside the SDK so callers can pass raw detection
            // coordinates (off-screen, over-sized, or covering the whole frame) without pre-clamping.
            // IVPS strokes the border of width nThick outward around the rectangle and requires the
            // whole stroked rectangle to stay inside the canvas; otherwise a full-frame or
            // edge-touching box is rejected with AX_ERR_IVPS_RGN_ILLEGAL_PARAM. Clamp each rect into
            // the canvas leaving a border-thickness margin on every side. 64-bit math keeps
            // out-of-range inputs safe.
            const std::int64_t margin = static_cast<std::int64_t>(rect.thickness);
            const std::int64_t canvas_w = static_cast<std::int64_t>(canvas.nW);
            const std::int64_t canvas_h = static_cast<std::int64_t>(canvas.nH);
            const std::int64_t rx0 = std::max<std::int64_t>(rect.x, margin);
            const std::int64_t ry0 = std::max<std::int64_t>(rect.y, margin);
            const std::int64_t rx1 =
                std::min<std::int64_t>(static_cast<std::int64_t>(rect.x) + rect.width, canvas_w - margin);
            const std::int64_t ry1 =
                std::min<std::int64_t>(static_cast<std::int64_t>(rect.y) + rect.height, canvas_h - margin);
            if (rx1 - rx0 < 1 || ry1 - ry0 < 1) {
                ++rect_skipped;  // rectangle lies (almost) entirely outside the canvas
                continue;
            }

            AX_IVPS_RECT_T ax_rect{};
            ax_rect.nX = static_cast<AX_S16>(rx0);
            ax_rect.nY = static_cast<AX_S16>(ry0);
            ax_rect.nW = static_cast<AX_U16>(rx1 - rx0);
            ax_rect.nH = static_cast<AX_U16>(ry1 - ry0);
            // Defensive backstop: if IVPS still rejects a rectangle, skip only that rectangle instead
            // of blanking the entire frame's overlay.
            const AX_S32 ret = AXCL_IVPS_DrawRect(&canvas, attr, ax_rect);
            if (ret != AX_SUCCESS) {
                ++rect_skipped;
                if (osd_debug) {
                    std::fprintf(stderr,
                                 "pipeline osd axcl: AXCL_IVPS_DrawRect ret=0x%x, skip rect{x=%d y=%d w=%u "
                                 "h=%u thick=%u} canvas{w=%u h=%u}\n",
                                 static_cast<unsigned>(ret), rect.x, rect.y, rect.width, rect.height,
                                 rect.thickness, canvas.nW, canvas.nH);
                }
                continue;
            }
        }
        if (osd_debug && rect_skipped != 0) {
            std::fprintf(stderr, "pipeline osd axcl: %zu/%zu rects skipped this frame\n", rect_skipped,
                         rects_.size());
        }

        return image.FlushCache();
    }

    std::vector<DrawRect> rects_;

private:
    std::uint32_t hold_frames_{1};
};

class AxclDrawer final : public AxDrawer {
public:
    std::shared_ptr<const PreparedDrawCommands> Prepare(const DrawFrame& frame) override {
        if (!frame.lines.empty() || !frame.polygons.empty() || !frame.mosaics.empty() || !frame.bitmaps.empty()) {
            std::fprintf(stderr, "pipeline osd axcl: only rect OSD is currently supported on AXCL\n");
            return nullptr;
        }

        auto prepared = std::make_shared<PreparedAxclDrawCommands>(frame.hold_frames);
        prepared->rects_ = frame.rects;
        return prepared;
    }

    bool Draw(const PreparedDrawCommands& commands, AxImage& image) override {
        return commands.Apply(image);
    }
};

}  // namespace

std::unique_ptr<AxDrawer> CreatePlatformDrawer() {
    return std::make_unique<AxclDrawer>();
}

}  // namespace axvsdk::common::internal
