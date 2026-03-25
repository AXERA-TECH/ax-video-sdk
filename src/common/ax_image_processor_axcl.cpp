#include "common/ax_image_processor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "axcl_ivps.h"
#include "axcl_rt_device.h"
#include "axcl_rt_memory.h"

#include "ax_image_copy.h"
#include "ax_image_internal.h"
#include "ax_system_internal.h"
#include "common/ax_system.h"

namespace axvsdk::common::internal {

namespace {

constexpr std::size_t kDefaultStrideAlignment = 16;

std::size_t AlignUp(std::size_t value, std::size_t alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

std::size_t MinStrideForFormat(PixelFormat format, std::uint32_t width) noexcept {
    switch (format) {
    case PixelFormat::kNv12:
        return width;
    case PixelFormat::kRgb24:
    case PixelFormat::kBgr24:
        return static_cast<std::size_t>(width) * 3U;
    case PixelFormat::kUnknown:
    default:
        return 0;
    }
}

bool ResolveOutputDescriptor(const AxImage& source,
                             const ImageProcessRequest& request,
                             ImageDescriptor* descriptor) noexcept {
    if (descriptor == nullptr) {
        return false;
    }

    *descriptor = request.output_image;
    if (descriptor->format == PixelFormat::kUnknown) {
        descriptor->format = source.format();
    }
    if (descriptor->width == 0) {
        descriptor->width = request.enable_crop && request.crop.width != 0 ? request.crop.width : source.width();
    }
    if (descriptor->height == 0) {
        descriptor->height = request.enable_crop && request.crop.height != 0 ? request.crop.height : source.height();
    }

    if (descriptor->width == 0 || descriptor->height == 0) {
        return false;
    }

    const auto min_stride = MinStrideForFormat(descriptor->format, descriptor->width);
    if (min_stride == 0) {
        return false;
    }

    if (descriptor->strides[0] == 0) {
        const auto stride_alignment =
            (descriptor->format == PixelFormat::kRgb24 || descriptor->format == PixelFormat::kBgr24)
                ? (kDefaultStrideAlignment * 3U)  // 16 pixels (48 bytes) to keep RGB stride % 3 == 0.
                : kDefaultStrideAlignment;
        descriptor->strides[0] = AlignUp(min_stride, stride_alignment);
    }

    if (descriptor->format == PixelFormat::kNv12) {
        if ((descriptor->width % 2U) != 0U || (descriptor->height % 2U) != 0U) {
            return false;
        }
        if (descriptor->strides[1] == 0) {
            descriptor->strides[1] = descriptor->strides[0];
        }
    }

    return descriptor->strides[0] >= min_stride;
}

bool ValidateCrop(const AxImage& source, const ImageProcessRequest& request) noexcept {
    if (!request.enable_crop) {
        return true;
    }

    if (request.crop.width == 0 || request.crop.height == 0 || request.crop.x < 0 || request.crop.y < 0) {
        return false;
    }

    const auto crop_right = static_cast<std::uint32_t>(request.crop.x) + request.crop.width;
    const auto crop_bottom = static_cast<std::uint32_t>(request.crop.y) + request.crop.height;
    if (crop_right > source.width() || crop_bottom > source.height()) {
        return false;
    }

    if (source.format() == PixelFormat::kNv12) {
        if ((request.crop.x % 2) != 0 || (request.crop.y % 2) != 0 ||
            (request.crop.width % 2U) != 0U || (request.crop.height % 2U) != 0U) {
            return false;
        }
    }

    return true;
}

AX_IVPS_ASPECT_RATIO_ALIGN_E ToHorizontalAlign(ResizeAlign align) noexcept {
    switch (align) {
    case ResizeAlign::kStart:
        return AX_IVPS_ASPECT_RATIO_HORIZONTAL_LEFT;
    case ResizeAlign::kEnd:
        return AX_IVPS_ASPECT_RATIO_HORIZONTAL_RIGHT;
    case ResizeAlign::kCenter:
    default:
        return AX_IVPS_ASPECT_RATIO_HORIZONTAL_CENTER;
    }
}

AX_IVPS_ASPECT_RATIO_ALIGN_E ToVerticalAlign(ResizeAlign align) noexcept {
    switch (align) {
    case ResizeAlign::kStart:
        return AX_IVPS_ASPECT_RATIO_VERTICAL_TOP;
    case ResizeAlign::kEnd:
        return AX_IVPS_ASPECT_RATIO_VERTICAL_BOTTOM;
    case ResizeAlign::kCenter:
    default:
        return AX_IVPS_ASPECT_RATIO_VERTICAL_CENTER;
    }
}

AX_IVPS_ASPECT_RATIO_T MakeAspectRatio(const ImageProcessRequest& request) noexcept {
    AX_IVPS_ASPECT_RATIO_T aspect_ratio{};
    if (request.resize.mode == ResizeMode::kKeepAspectRatio) {
        aspect_ratio.eMode = AX_IVPS_ASPECT_RATIO_AUTO;
        aspect_ratio.eAligns[0] = ToHorizontalAlign(request.resize.horizontal_align);
        aspect_ratio.eAligns[1] = ToVerticalAlign(request.resize.vertical_align);
        aspect_ratio.nBgColor = request.resize.background_color;
    } else {
        aspect_ratio.eMode = AX_IVPS_ASPECT_RATIO_STRETCH;
        aspect_ratio.eAligns[0] = AX_IVPS_ASPECT_RATIO_HORIZONTAL_CENTER;
        aspect_ratio.eAligns[1] = AX_IVPS_ASPECT_RATIO_VERTICAL_CENTER;
        aspect_ratio.nBgColor = 0;
    }
    return aspect_ratio;
}

bool SameGeometryAndFormat(const AxImage& source, const AxImage& destination, const ImageProcessRequest& request) noexcept {
    return !request.enable_crop &&
           request.resize.mode == ResizeMode::kStretch &&
           source.format() == destination.format() &&
           source.width() == destination.width() &&
           source.height() == destination.height() &&
           source.stride(0) == destination.stride(0) &&
           source.stride(1) == destination.stride(1);
}

class AxclImageProcessor final : public ImageProcessor {
public:
    AxImage::Ptr Process(const AxImage& source, const ImageProcessRequest& request) override {
        ImageDescriptor output_descriptor{};
        if (!ResolveOutputDescriptor(source, request, &output_descriptor)) {
            return nullptr;
        }

        ImageAllocationOptions options{};
        options.memory_type = MemoryType::kCmm;
        options.cache_mode = CacheMode::kNonCached;
        options.alignment = 0x1000;
        options.token = "AxclImageProcessor";
        auto output = AxImage::Create(output_descriptor, options);
        if (!output) {
            return nullptr;
        }

        return Process(source, request, *output) ? output : nullptr;
    }

    bool Process(const AxImage& source, const ImageProcessRequest& request, AxImage& destination) override {
        if (!common::IsSystemInitialized() || !ValidateCrop(source, request) || !EnsureAxclThreadContext()) {
            return false;
        }

        ImageDescriptor expected_output{};
        if (!ResolveOutputDescriptor(source, request, &expected_output)) {
            return false;
        }

        if (destination.format() != expected_output.format || destination.width() != expected_output.width ||
            destination.height() != expected_output.height || destination.stride(0) < expected_output.strides[0] ||
            (destination.format() == PixelFormat::kNv12 && destination.stride(1) < expected_output.strides[1])) {
            return false;
        }

        if (SameGeometryAndFormat(source, destination, request)) {
            return CopyImage(source, &destination);
        }

        auto& mutable_source = const_cast<AxImage&>(source);
        (void)mutable_source.FlushCache();
        AX_VIDEO_FRAME_T src_frame = AxImageAccess::GetAxFrame(source);
        auto* dst_frame = AxImageAccess::MutableAxFrame(&destination);
        if (dst_frame == nullptr) {
            return false;
        }

        if (request.enable_crop) {
            src_frame.s16CropX = static_cast<AX_S16>(request.crop.x);
            src_frame.s16CropY = static_cast<AX_S16>(request.crop.y);
            src_frame.s16CropWidth = static_cast<AX_S16>(request.crop.width);
            src_frame.s16CropHeight = static_cast<AX_S16>(request.crop.height);
        }

        const auto aspect_ratio = MakeAspectRatio(request);
        const bool is_csc_only =
            (!request.enable_crop && source.width() == destination.width() && source.height() == destination.height());

        AX_S32 ret = AX_SUCCESS;
        if (is_csc_only) {
            // Fast path: geometry matches, only CSC is needed.
            ret = AXCL_IVPS_CscVgp(&src_frame, dst_frame);
            if (ret != AX_SUCCESS) {
                return false;
            }
        } else {
            // Clear destination to avoid stale garbage in padding region (letterbox).
            const auto plane_count = destination.plane_count();
            for (std::size_t plane = 0; plane < plane_count; ++plane) {
                const auto phy = destination.physical_address(plane);
                const auto sz = destination.plane_size(plane);
                if (phy == 0 || sz == 0) {
                    return false;
                }
                const std::uint8_t v = (destination.format() == PixelFormat::kNv12 && plane == 1) ? 128U : 0U;
                if (axclrtMemset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(phy)), v, sz) != AXCL_SUCC) {
                    return false;
                }
            }

            // On AXCL, VGP is more reliable for crop/resize when output is packed RGB/BGR.
            if (destination.format() == PixelFormat::kRgb24 || destination.format() == PixelFormat::kBgr24) {
                ret = AXCL_IVPS_CropResizeVgp(&src_frame, dst_frame, &aspect_ratio);
            } else {
                ret = AXCL_IVPS_CropResizeVpp(&src_frame, dst_frame, &aspect_ratio);
            }
            if (ret != AX_SUCCESS) {
                return false;
            }
        }

        // AXCL_IVPS_* APIs are synchronous enough for downstream device consumers (e.g. NPU/VENC).
        // Avoid axclrtSynchronizeDevice here: it's a device-wide barrier and will wait for unrelated
        // codec/ivps workloads, which can severely reduce throughput.
        return destination.InvalidateCache();
    }

private:
};

}  // namespace

std::unique_ptr<ImageProcessor> CreatePlatformImageProcessor() {
    return std::make_unique<AxclImageProcessor>();
}

}  // namespace axvsdk::common::internal
