#include "ax_image_processor_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "ax_ivps_lock.h"

#include "ax_ivps_api.h"

#include "ax_image_internal.h"
#include "common/ax_system.h"

namespace axvsdk::common::internal {

namespace {

constexpr std::size_t kDefaultStrideAlignment = 16;
constexpr AX_U32 kProcessorPoolBlockCount = 24;
constexpr AX_U64 kProcessorPoolMetaSize = 512;

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

std::size_t PlaneHeight(const ImageDescriptor& descriptor, std::size_t plane_index) noexcept {
    switch (descriptor.format) {
    case PixelFormat::kNv12:
        return plane_index == 0 ? descriptor.height : descriptor.height / 2U;
    case PixelFormat::kRgb24:
    case PixelFormat::kBgr24:
        return descriptor.height;
    case PixelFormat::kUnknown:
    default:
        return 0;
    }
}

AX_U64 ComputeImageByteSize(const ImageDescriptor& descriptor) noexcept {
    AX_U64 total_size = 0;
    total_size += static_cast<AX_U64>(descriptor.strides[0]) * PlaneHeight(descriptor, 0);
    if (descriptor.format == PixelFormat::kNv12) {
        total_size += static_cast<AX_U64>(descriptor.strides[1]) * PlaneHeight(descriptor, 1);
    }
    return total_size;
}

bool MakeAlignedDescriptor(PixelFormat format,
                           std::uint32_t width,
                           std::uint32_t height,
                           ImageDescriptor* descriptor) noexcept {
    if (descriptor == nullptr) {
        return false;
    }

    ImageDescriptor out{};
    out.format = format;
    out.width = width;
    out.height = height;

    if (out.width == 0 || out.height == 0) {
        return false;
    }

    const auto min_stride = MinStrideForFormat(out.format, out.width);
    if (min_stride == 0) {
        return false;
    }

    const auto stride_alignment =
        (out.format == PixelFormat::kRgb24 || out.format == PixelFormat::kBgr24)
            ? (kDefaultStrideAlignment * 3U)  // 16 pixels (48 bytes) to keep RGB stride % 3 == 0.
            : kDefaultStrideAlignment;
    out.strides[0] = AlignUp(min_stride, stride_alignment);

    if (out.format == PixelFormat::kNv12) {
        if ((out.width % 2U) != 0U || (out.height % 2U) != 0U) {
            return false;
        }
        out.strides[1] = out.strides[0];
    }

    *descriptor = out;
    return out.strides[0] >= min_stride;
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

AX_IVPS_ASPECT_RATIO_T MakeStretchAspectRatio() noexcept {
    AX_IVPS_ASPECT_RATIO_T aspect_ratio{};
    aspect_ratio.eMode = AX_IVPS_ASPECT_RATIO_STRETCH;
    aspect_ratio.eAligns[0] = AX_IVPS_ASPECT_RATIO_HORIZONTAL_CENTER;
    aspect_ratio.eAligns[1] = AX_IVPS_ASPECT_RATIO_VERTICAL_CENTER;
    aspect_ratio.nBgColor = 0;
    return aspect_ratio;
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
    if (request.resize.mode == ResizeMode::kKeepAspectRatio) {
        AX_IVPS_ASPECT_RATIO_T aspect_ratio{};
        aspect_ratio.eMode = AX_IVPS_ASPECT_RATIO_AUTO;
        aspect_ratio.eAligns[0] = ToHorizontalAlign(request.resize.horizontal_align);
        aspect_ratio.eAligns[1] = ToVerticalAlign(request.resize.vertical_align);
        aspect_ratio.nBgColor = request.resize.background_color;
        return aspect_ratio;
    }

    return MakeStretchAspectRatio();
}

std::uint8_t ClampToByte(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void RgbToYuv(std::uint32_t rgb, std::uint8_t* y, std::uint8_t* u, std::uint8_t* v) noexcept {
    if (y == nullptr || u == nullptr || v == nullptr) {
        return;
    }

    const auto r = static_cast<int>((rgb >> 16U) & 0xFFU);
    const auto g = static_cast<int>((rgb >> 8U) & 0xFFU);
    const auto b = static_cast<int>(rgb & 0xFFU);

    *y = ClampToByte(static_cast<int>(std::lround(0.299 * r + 0.587 * g + 0.114 * b)));
    *u = ClampToByte(static_cast<int>(std::lround(-0.169 * r - 0.331 * g + 0.5 * b + 128.0)));
    *v = ClampToByte(static_cast<int>(std::lround(0.5 * r - 0.419 * g - 0.081 * b + 128.0)));
}

bool FillBackground(AxImage& image, std::uint32_t rgb) noexcept {
    switch (image.format()) {
    case PixelFormat::kNv12: {
        auto* y_plane = image.mutable_plane_data(0);
        auto* uv_plane = image.mutable_plane_data(1);
        if (y_plane == nullptr || uv_plane == nullptr) {
            return false;
        }

        std::uint8_t y = 0;
        std::uint8_t u = 128;
        std::uint8_t v = 128;
        RgbToYuv(rgb, &y, &u, &v);

        for (std::size_t row = 0; row < image.height(); ++row) {
            std::fill_n(y_plane + row * image.stride(0), image.width(), y);
        }
        for (std::size_t row = 0; row < image.height() / 2U; ++row) {
            auto* row_ptr = uv_plane + row * image.stride(1);
            for (std::size_t col = 0; col < image.width(); col += 2U) {
                row_ptr[col] = u;
                row_ptr[col + 1U] = v;
            }
        }
        return image.FlushCache();
    }
    case PixelFormat::kRgb24:
    case PixelFormat::kBgr24: {
        auto* plane = image.mutable_plane_data(0);
        if (plane == nullptr) {
            return false;
        }

        const auto r = static_cast<std::uint8_t>((rgb >> 16U) & 0xFFU);
        const auto g = static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU);
        const auto b = static_cast<std::uint8_t>(rgb & 0xFFU);
        for (std::size_t row = 0; row < image.height(); ++row) {
            auto* row_ptr = plane + row * image.stride(0);
            for (std::size_t col = 0; col < image.width(); ++col) {
                const std::size_t offset = col * 3U;
                if (image.format() == PixelFormat::kRgb24) {
                    row_ptr[offset] = r;
                    row_ptr[offset + 1U] = g;
                    row_ptr[offset + 2U] = b;
                } else {
                    row_ptr[offset] = b;
                    row_ptr[offset + 1U] = g;
                    row_ptr[offset + 2U] = r;
                }
            }
        }
        return image.FlushCache();
    }
    case PixelFormat::kUnknown:
    default:
        return false;
    }
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

class Ax650ImageProcessor final : public ImageProcessor {
public:
    AxImage::Ptr Process(const AxImage& source, const ImageProcessRequest& request) override {
        ImageDescriptor output_descriptor{};
        if (!ResolveOutputDescriptor(source, request, &output_descriptor)) {
            return nullptr;
        }

        const auto pool = AcquirePool(output_descriptor);
        if (!pool || pool->id == AX_INVALID_POOLID) {
            std::fprintf(stderr, "ax650 image processor: acquire pool failed\n");
            return nullptr;
        }

        ImageAllocationOptions options{};
        options.memory_type = MemoryType::kPool;
        options.cache_mode = CacheMode::kNonCached;
        options.alignment = 0x1000;
        options.pool_id = pool->id;
        options.token = "AxImageProcessor";
        auto output = AxImage::Create(output_descriptor, options);
        if (!output) {
            return nullptr;
        }

        AxImageAccess::AttachLifetime(output.get(), pool);

        if (!Process(source, request, *output)) {
            return nullptr;
        }

        return output;
    }

    bool Process(const AxImage& source, const ImageProcessRequest& request, AxImage& destination) override {
        if (!common::IsSystemInitialized() || !ValidateCrop(source, request)) {
            std::fprintf(stderr, "ax650 image processor: system not ready or crop invalid\n");
            return false;
        }

        // IVPS is not reliably thread-safe across MSP versions; serialize in-process.
        std::lock_guard<std::mutex> ivps_lock(common::internal::IvpsGlobalMutex());

        ImageDescriptor expected_output{};
        if (!ResolveOutputDescriptor(source, request, &expected_output)) {
            std::fprintf(stderr, "ax650 image processor: resolve output descriptor failed\n");
            return false;
        }

        if (destination.format() != expected_output.format || destination.width() != expected_output.width ||
            destination.height() != expected_output.height || destination.stride(0) < expected_output.strides[0] ||
            (destination.format() == PixelFormat::kNv12 && destination.stride(1) < expected_output.strides[1])) {
            std::fprintf(stderr,
                         "ax650 image processor: destination mismatch fmt=%d/%d size=%ux%u/%ux%u stride=%zu/%zu\n",
                         static_cast<int>(destination.format()), static_cast<int>(expected_output.format),
                         destination.width(), destination.height(), expected_output.width, expected_output.height,
                         destination.stride(0), expected_output.strides[0]);
            return false;
        }

        auto& mutable_source = const_cast<AxImage&>(source);
        (void)mutable_source.FlushCache();
        const auto& src_frame = AxImageAccess::GetAxFrame(source);
        auto* dst_frame = AxImageAccess::MutableAxFrame(&destination);
        if (dst_frame == nullptr) {
            return false;
        }

        AX_S32 ret = AX_SUCCESS;
        if (SameGeometryAndFormat(source, destination, request)) {
            ret = AX_IVPS_CmmCopyVpp(src_frame.u64PhyAddr[0], dst_frame->u64PhyAddr[0], destination.byte_size());
        } else {
            AX_VIDEO_FRAME_T src_frame_for_processing = src_frame;
            if (request.enable_crop) {
                src_frame_for_processing.s16CropX = static_cast<AX_S16>(request.crop.x);
                src_frame_for_processing.s16CropY = static_cast<AX_S16>(request.crop.y);
                src_frame_for_processing.s16CropWidth = static_cast<AX_S16>(request.crop.width);
                src_frame_for_processing.s16CropHeight = static_cast<AX_S16>(request.crop.height);
            }

            const auto aspect_ratio = MakeAspectRatio(request);
            const bool needs_geometry_change =
                request.enable_crop || source.width() != destination.width() || source.height() != destination.height();
            const bool needs_format_change = source.format() != destination.format();

            if (!needs_geometry_change && needs_format_change) {
                // For packed RGB/BGR, AX650 MSP samples consistently use the TDP path.
                // Some MSP versions show corruption when using VPP CSC for RGB/BGR.
                ret = AX_IVPS_CscTdp(&src_frame_for_processing, dst_frame);
            } else if (!needs_format_change) {
                if (request.resize.mode == ResizeMode::kKeepAspectRatio &&
                    !FillBackground(destination, request.resize.background_color)) {
                    std::fprintf(stderr, "ax650 image processor: fill background failed fmt=%d size=%ux%u\n",
                                 static_cast<int>(destination.format()), destination.width(), destination.height());
                    return false;
                }

                // Same format: CropResizeVpp handles crop/resize.
                ret = AX_IVPS_CropResizeVpp(&src_frame_for_processing, dst_frame, &aspect_ratio);
            } else {
                // AX650 IVPS CropResizeVpp does not reliably support simultaneous resize and color conversion.
                // Stage it: resize in source format -> CSC to destination format.
                ImageDescriptor intermediate_desc{};
                if (!MakeAlignedDescriptor(source.format(), destination.width(), destination.height(), &intermediate_desc)) {
                    std::fprintf(stderr,
                                 "ax650 image processor: make intermediate descriptor failed src_fmt=%d dst_fmt=%d size=%ux%u\n",
                                 static_cast<int>(source.format()), static_cast<int>(destination.format()),
                                 destination.width(), destination.height());
                    return false;
                }

                auto intermediate = AcquireIntermediate(intermediate_desc);
                if (!intermediate) {
                    std::fprintf(stderr, "ax650 image processor: allocate intermediate failed fmt=%d size=%ux%u\n",
                                 static_cast<int>(intermediate_desc.format),
                                 intermediate_desc.width,
                                 intermediate_desc.height);
                    return false;
                }

                if (request.resize.mode == ResizeMode::kKeepAspectRatio &&
                    !FillBackground(*intermediate, request.resize.background_color)) {
                    std::fprintf(stderr, "ax650 image processor: fill intermediate background failed fmt=%d size=%ux%u\n",
                                 static_cast<int>(intermediate->format()), intermediate->width(), intermediate->height());
                    return false;
                }

                auto* intermediate_frame = AxImageAccess::MutableAxFrame(intermediate.get());
                if (intermediate_frame == nullptr) {
                    return false;
                }

                ret = AX_IVPS_CropResizeVpp(&src_frame_for_processing, intermediate_frame, &aspect_ratio);
                if (ret == AX_SUCCESS) {
                    ret = AX_IVPS_CscTdp(intermediate_frame, dst_frame);
                }
            }
        }

        if (ret != AX_SUCCESS) {
            std::fprintf(stderr, "ax650 image processor: ivps op failed ret=0x%x src_fmt=%d dst_fmt=%d src=%ux%u dst=%ux%u crop=%d\n",
                         ret, static_cast<int>(source.format()), static_cast<int>(destination.format()),
                         source.width(), source.height(), destination.width(), destination.height(),
                         request.enable_crop ? 1 : 0);
            return false;
        }

        return destination.InvalidateCache();
    }

private:
    common::AxImage::Ptr AcquireIntermediate(const common::ImageDescriptor& descriptor) {
        if (intermediate_ &&
            intermediate_->format() == descriptor.format &&
            intermediate_->width() == descriptor.width &&
            intermediate_->height() == descriptor.height &&
            intermediate_->stride(0) == descriptor.strides[0] &&
            (descriptor.format != PixelFormat::kNv12 || intermediate_->stride(1) == descriptor.strides[1])) {
            return intermediate_;
        }

        ImageAllocationOptions options{};
        options.memory_type = MemoryType::kCmm;
        options.cache_mode = CacheMode::kNonCached;
        options.alignment = 0x1000;
        options.token = "AxImageProcessorIntermediate";
        intermediate_ = AxImage::Create(descriptor, options);
        return intermediate_;
    }

    struct PoolHandle {
        explicit PoolHandle(AX_POOL pool_id) noexcept : id(pool_id) {}

        ~PoolHandle() {
            if (id != AX_INVALID_POOLID) {
                (void)AX_POOL_DestroyPool(id);
            }
        }

        AX_POOL id{AX_INVALID_POOLID};
    };

    struct PoolEntry {
        ImageDescriptor descriptor{};
        AX_U64 block_size{0};
        std::shared_ptr<PoolHandle> pool;
    };

    std::shared_ptr<PoolHandle> AcquirePool(const ImageDescriptor& descriptor) {
        const auto block_size = ComputeImageByteSize(descriptor);
        if (block_size == 0) {
            return {};
        }

        std::lock_guard<std::mutex> lock(pool_mutex_);
        for (const auto& pool : pools_) {
            if (pool.block_size == block_size && pool.descriptor.format == descriptor.format &&
                pool.descriptor.width == descriptor.width && pool.descriptor.height == descriptor.height &&
                pool.descriptor.strides[0] == descriptor.strides[0] &&
                pool.descriptor.strides[1] == descriptor.strides[1]) {
                return pool.pool;
            }
        }

        AX_POOL_CONFIG_T pool_config{};
        pool_config.MetaSize = kProcessorPoolMetaSize;
        pool_config.BlkCnt = kProcessorPoolBlockCount;
        pool_config.BlkSize = block_size;
        pool_config.CacheMode = POOL_CACHE_MODE_NONCACHE;
        std::snprintf(reinterpret_cast<char*>(pool_config.PartitionName), AX_MAX_PARTITION_NAME_LEN, "anonymous");

        const auto pool_id = AX_POOL_CreatePool(&pool_config);
        if (pool_id == AX_INVALID_POOLID) {
            return {};
        }

        auto pool = std::make_shared<PoolHandle>(pool_id);
        pools_.push_back(PoolEntry{descriptor, block_size, pool});
        return pool;
    }

    std::mutex pool_mutex_;
    std::vector<PoolEntry> pools_;
    common::AxImage::Ptr intermediate_;
};

}  // namespace

std::unique_ptr<ImageProcessor> CreatePlatformImageProcessor() {
    return std::make_unique<Ax650ImageProcessor>();
}

}  // namespace axvsdk::common::internal
