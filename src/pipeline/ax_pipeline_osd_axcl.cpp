#include "common/ax_drawer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

bool FitsAxCoordinate(std::int32_t value) noexcept {
    return value >= 0 && value <= std::numeric_limits<AX_S16>::max();
}

bool FitsAxSize(std::uint32_t value) noexcept {
    return value > 0 && value <= static_cast<std::uint32_t>(std::numeric_limits<AX_U16>::max());
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
            if (AXCL_IVPS_DrawRect(&canvas, attr, ax_rect) != AX_SUCCESS) {
                return false;
            }
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
