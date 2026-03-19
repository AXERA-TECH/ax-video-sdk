#include "common/ax_image.h"
#include "ax_image_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "ax_sys_api.h"

namespace axvsdk::common {

namespace {

constexpr std::size_t kRgbChannels = 3;

std::size_t PlaneCountForFormat(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::kNv12:
        return 2;
    case PixelFormat::kRgb24:
    case PixelFormat::kBgr24:
        return 1;
    case PixelFormat::kUnknown:
    default:
        return 0;
    }
}

std::size_t DefaultStride(PixelFormat format, std::uint32_t width, std::size_t plane_index) noexcept {
    switch (format) {
    case PixelFormat::kNv12:
        return plane_index < 2 ? width : 0;
    case PixelFormat::kRgb24:
    case PixelFormat::kBgr24:
        return plane_index == 0 ? static_cast<std::size_t>(width) * kRgbChannels : 0;
    case PixelFormat::kUnknown:
    default:
        return 0;
    }
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

bool IsValidDescriptor(const ImageDescriptor& descriptor) noexcept {
    if (descriptor.width == 0 || descriptor.height == 0) {
        return false;
    }

    if (descriptor.format == PixelFormat::kNv12 &&
        ((descriptor.width % 2U) != 0U || (descriptor.height % 2U) != 0U)) {
        return false;
    }

    const auto plane_count = PlaneCountForFormat(descriptor.format);
    if (plane_count == 0) {
        return false;
    }

    for (std::size_t index = 0; index < plane_count; ++index) {
        const auto min_stride = DefaultStride(descriptor.format, descriptor.width, index);
        const auto stride = descriptor.strides[index] == 0 ? min_stride : descriptor.strides[index];
        if (stride < min_stride) {
            return false;
        }
    }

    return true;
}

ImageDescriptor NormalizeDescriptor(ImageDescriptor descriptor) noexcept {
    const auto plane_count = PlaneCountForFormat(descriptor.format);
    for (std::size_t index = 0; index < plane_count; ++index) {
        if (descriptor.strides[index] == 0) {
            descriptor.strides[index] = DefaultStride(descriptor.format, descriptor.width, index);
        }
    }
    return descriptor;
}

AX_IMG_FORMAT_E ToAxFormat(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::kNv12:
        return AX_FORMAT_YUV420_SEMIPLANAR;
    case PixelFormat::kRgb24:
        return AX_FORMAT_RGB888;
    case PixelFormat::kBgr24:
        return AX_FORMAT_BGR888;
    case PixelFormat::kUnknown:
    default:
        return AX_FORMAT_INVALID;
    }
}

PixelFormat FromAxFormat(AX_IMG_FORMAT_E format) noexcept {
    switch (format) {
    case AX_FORMAT_YUV420_SEMIPLANAR:
        return PixelFormat::kNv12;
    case AX_FORMAT_RGB888:
        return PixelFormat::kRgb24;
    case AX_FORMAT_BGR888:
        return PixelFormat::kBgr24;
    default:
        return PixelFormat::kUnknown;
    }
}

MemoryType ResolveMemoryType(MemoryType memory_type) noexcept {
    if (memory_type == MemoryType::kAuto) {
        return MemoryType::kCmm;
    }
    return memory_type;
}

}  // namespace

struct AxImage::Impl {
    ImageDescriptor descriptor{};
    std::size_t plane_count{0};
    std::array<std::size_t, kMaxImagePlanes> plane_offsets{};
    std::array<std::size_t, kMaxImagePlanes> plane_sizes{};
    std::size_t total_size{0};
    MemoryType memory_type{MemoryType::kExternal};
    CacheMode cache_mode{CacheMode::kNonCached};
    bool owns_memory{false};
    std::array<std::uint64_t, kMaxImagePlanes> physical_addresses{};
    std::array<void*, kMaxImagePlanes> virtual_addresses{};
    std::array<std::uint32_t, kMaxImagePlanes> block_ids{};
    AX_VIDEO_FRAME_INFO_T frame_info{};
    internal::AxImageAccess::FrameReleaseCallback release_callback;
    std::shared_ptr<void> lifetime_holder;

    void InitializeLayout(const ImageDescriptor& descriptor) {
        this->descriptor = NormalizeDescriptor(descriptor);
        plane_count = PlaneCountForFormat(this->descriptor.format);
        plane_offsets.fill(0);
        plane_sizes.fill(0);
        physical_addresses.fill(0);
        virtual_addresses.fill(nullptr);
        block_ids.fill(0);
        total_size = 0;

        for (std::size_t index = 0; index < plane_count; ++index) {
            plane_offsets[index] = total_size;
            plane_sizes[index] = this->descriptor.strides[index] * PlaneHeight(this->descriptor, index);
            total_size += plane_sizes[index];
        }
    }

    void PopulateFrameInfo(AX_U64 base_phy_addr, void* base_vir_addr, AX_BLK block_id) noexcept {
        std::memset(&frame_info, 0, sizeof(frame_info));

        auto& frame = frame_info.stVFrame;
        frame.u32Width = descriptor.width;
        frame.u32Height = descriptor.height;
        frame.enImgFormat = ToAxFormat(descriptor.format);
        frame.enVscanFormat = AX_VSCAN_FORMAT_RASTER;
        frame.stCompressInfo.enCompressMode = AX_COMPRESS_MODE_NONE;
        frame.stDynamicRange = AX_DYNAMIC_RANGE_SDR8;
        frame.stColorGamut = AX_COLOR_GAMUT_BT709;
        frame.u32FrameSize = static_cast<AX_U32>(total_size);

        const auto base_vir = reinterpret_cast<std::uintptr_t>(base_vir_addr);
        for (std::size_t index = 0; index < plane_count; ++index) {
            frame.u32PicStride[index] = static_cast<AX_U32>(descriptor.strides[index]);
            frame.u64PhyAddr[index] = base_phy_addr + plane_offsets[index];
            frame.u64VirAddr[index] = static_cast<AX_U64>(base_vir + plane_offsets[index]);
            physical_addresses[index] = frame.u64PhyAddr[index];
            virtual_addresses[index] = reinterpret_cast<void*>(base_vir + plane_offsets[index]);
        }

        if (block_id != AX_INVALID_BLOCKID) {
            frame.u32BlkId[0] = block_id;
            block_ids[0] = block_id;
        }

        frame_info.enModId = AX_ID_USER;
        frame_info.bEndOfStream = AX_FALSE;
    }

    bool AllocateFromCmm(const ImageAllocationOptions& options) {
        AX_U64 phy_addr = 0;
        AX_VOID* vir_addr = nullptr;

        const auto* token = reinterpret_cast<const AX_S8*>(
            options.token.empty() ? "AxImage" : options.token.c_str());
        const AX_S32 ret = options.cache_mode == CacheMode::kCached
                               ? AX_SYS_MemAllocCached(&phy_addr, &vir_addr, static_cast<AX_U32>(total_size),
                                                       options.alignment, token)
                               : AX_SYS_MemAlloc(&phy_addr, &vir_addr, static_cast<AX_U32>(total_size),
                                                 options.alignment, token);
        if (ret != AX_SUCCESS || phy_addr == 0 || vir_addr == nullptr) {
            return false;
        }

        owns_memory = true;
        PopulateFrameInfo(phy_addr, vir_addr, AX_INVALID_BLOCKID);
        return true;
    }

    bool AllocateFromPool(const ImageAllocationOptions& options) {
        if (options.pool_id == kInvalidPoolId) {
            return false;
        }

        const auto* partition_name = options.partition_name.empty()
                                         ? nullptr
                                         : reinterpret_cast<const AX_S8*>(options.partition_name.c_str());
        const AX_BLK block_id = AX_POOL_GetBlock(options.pool_id, total_size, partition_name);
        if (block_id == AX_INVALID_BLOCKID) {
            return false;
        }

        const AX_U64 phy_addr = AX_POOL_Handle2PhysAddr(block_id);
        AX_VOID* vir_addr = AX_POOL_GetBlockVirAddr(block_id);
        if (phy_addr == 0 || vir_addr == nullptr) {
            (void)AX_POOL_ReleaseBlock(block_id);
            return false;
        }

        owns_memory = true;
        PopulateFrameInfo(phy_addr, vir_addr, block_id);
        return true;
    }

    ~Impl() {
        Release();
    }

    void Release() noexcept {
        if (release_callback) {
            try {
                release_callback(frame_info);
            } catch (...) {
            }
            release_callback = {};
        }

        if (!owns_memory) {
            return;
        }

        if (memory_type == MemoryType::kPool && block_ids[0] != AX_INVALID_BLOCKID) {
            (void)AX_POOL_ReleaseBlock(block_ids[0]);
        } else if (memory_type == MemoryType::kCmm && physical_addresses[0] != 0 && virtual_addresses[0] != nullptr) {
            (void)AX_SYS_MemFree(physical_addresses[0], virtual_addresses[0]);
        }

        owns_memory = false;
        physical_addresses.fill(0);
        virtual_addresses.fill(nullptr);
        block_ids.fill(0);
    }
};

AxImage::Ptr AxImage::Create(PixelFormat format,
                             std::uint32_t width,
                             std::uint32_t height,
                             const ImageAllocationOptions& options) {
    return Create(ImageDescriptor{format, width, height, {}}, options);
}

AxImage::Ptr AxImage::Create(const ImageDescriptor& descriptor, const ImageAllocationOptions& options) {
    if (!IsValidDescriptor(descriptor)) {
        return nullptr;
    }

    auto impl = std::make_unique<AxImage::Impl>();
    impl->InitializeLayout(descriptor);
    impl->memory_type = ResolveMemoryType(options.memory_type);
    impl->cache_mode = options.cache_mode;

    bool allocated = false;
    switch (impl->memory_type) {
    case MemoryType::kCmm:
        allocated = impl->AllocateFromCmm(options);
        break;
    case MemoryType::kPool:
        allocated = impl->AllocateFromPool(options);
        break;
    case MemoryType::kAuto:
        break;
    case MemoryType::kExternal:
        break;
    }

    if (!allocated) {
        return nullptr;
    }

    return AxImage::Ptr(new AxImage(std::move(impl)));
}

AxImage::Ptr AxImage::WrapExternal(const ImageDescriptor& descriptor,
                                   const std::array<ExternalImagePlane, kMaxImagePlanes>& planes,
                                   LifetimeHolder lifetime) {
    if (!IsValidDescriptor(descriptor)) {
        return nullptr;
    }

    auto impl = std::make_unique<AxImage::Impl>();
    impl->InitializeLayout(descriptor);
    impl->memory_type = MemoryType::kExternal;
    impl->cache_mode = CacheMode::kNonCached;
    impl->owns_memory = false;
    impl->lifetime_holder = std::move(lifetime);
    std::memset(&impl->frame_info, 0, sizeof(impl->frame_info));

    auto& frame = impl->frame_info.stVFrame;
    frame.u32Width = impl->descriptor.width;
    frame.u32Height = impl->descriptor.height;
    frame.enImgFormat = ToAxFormat(impl->descriptor.format);
    frame.enVscanFormat = AX_VSCAN_FORMAT_RASTER;
    frame.stCompressInfo.enCompressMode = AX_COMPRESS_MODE_NONE;
    frame.stDynamicRange = AX_DYNAMIC_RANGE_SDR8;
    frame.stColorGamut = AX_COLOR_GAMUT_BT709;
    frame.u32FrameSize = static_cast<AX_U32>(impl->total_size);
    impl->frame_info.enModId = AX_ID_USER;
    impl->frame_info.bEndOfStream = AX_FALSE;

    for (std::size_t index = 0; index < impl->plane_count; ++index) {
        frame.u32PicStride[index] = static_cast<AX_U32>(impl->descriptor.strides[index]);
        frame.u64PhyAddr[index] = planes[index].physical_address;
        frame.u64VirAddr[index] =
            static_cast<AX_U64>(reinterpret_cast<std::uintptr_t>(planes[index].virtual_address));
        frame.u32BlkId[index] = planes[index].block_id == kInvalidPoolId ? AX_INVALID_BLOCKID : planes[index].block_id;
        impl->physical_addresses[index] = planes[index].physical_address;
        impl->virtual_addresses[index] = planes[index].virtual_address;
        impl->block_ids[index] = planes[index].block_id;
        if (planes[index].virtual_address == nullptr) {
            return nullptr;
        }
    }

    return AxImage::Ptr(new AxImage(std::move(impl)));
}

AxImage::Ptr internal::AxImageAccess::WrapVideoFrame(const AX_VIDEO_FRAME_INFO_T& frame_info,
                                                     FrameReleaseCallback release_callback) {
    const auto format = FromAxFormat(frame_info.stVFrame.enImgFormat);
    if (format == PixelFormat::kUnknown || frame_info.stVFrame.u32Width == 0 || frame_info.stVFrame.u32Height == 0) {
        return nullptr;
    }

    ImageDescriptor descriptor{};
    descriptor.format = format;
    descriptor.width = frame_info.stVFrame.u32Width;
    descriptor.height = frame_info.stVFrame.u32Height;

    const auto plane_count = PlaneCountForFormat(format);
    for (std::size_t index = 0; index < plane_count; ++index) {
        descriptor.strides[index] = frame_info.stVFrame.u32PicStride[index];
    }

    auto impl = std::make_unique<AxImage::Impl>();
    impl->InitializeLayout(descriptor);
    impl->memory_type = frame_info.stVFrame.u32BlkId[0] != AX_INVALID_BLOCKID ? MemoryType::kPool
                                                                               : MemoryType::kExternal;
    impl->cache_mode = CacheMode::kNonCached;
    impl->owns_memory = false;
    impl->frame_info = frame_info;
    impl->release_callback = std::move(release_callback);

    for (std::size_t index = 0; index < impl->plane_count; ++index) {
        impl->physical_addresses[index] = frame_info.stVFrame.u64PhyAddr[index];
        impl->virtual_addresses[index] =
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame_info.stVFrame.u64VirAddr[index]));
        impl->block_ids[index] = frame_info.stVFrame.u32BlkId[index];
    }

    if (impl->virtual_addresses[0] == nullptr && frame_info.stVFrame.u32BlkId[0] != AX_INVALID_BLOCKID &&
        frame_info.stVFrame.u64PhyAddr[0] != 0) {
        AX_VOID* base_vir_addr = AX_POOL_GetBlockVirAddr(frame_info.stVFrame.u32BlkId[0]);
        if (base_vir_addr != nullptr) {
            const auto base_phy_addr = frame_info.stVFrame.u64PhyAddr[0];
            const auto base_vir = reinterpret_cast<std::uintptr_t>(base_vir_addr);
            for (std::size_t index = 0; index < impl->plane_count; ++index) {
                if (impl->physical_addresses[index] == 0) {
                    continue;
                }
                impl->virtual_addresses[index] = reinterpret_cast<void*>(
                    base_vir + static_cast<std::uintptr_t>(impl->physical_addresses[index] - base_phy_addr));
                impl->frame_info.stVFrame.u64VirAddr[index] =
                    static_cast<AX_U64>(reinterpret_cast<std::uintptr_t>(impl->virtual_addresses[index]));
            }
        }
    }

    if (frame_info.stVFrame.u32FrameSize != 0) {
        impl->total_size = frame_info.stVFrame.u32FrameSize;
    }

    return AxImage::Ptr(new AxImage(std::move(impl)));
}

AxImage::AxImage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AxImage::AxImage(AxImage&&) noexcept = default;

AxImage& AxImage::operator=(AxImage&&) noexcept = default;

AxImage::~AxImage() = default;

PixelFormat AxImage::format() const noexcept {
    return impl_->descriptor.format;
}

std::uint32_t AxImage::width() const noexcept {
    return impl_->descriptor.width;
}

std::uint32_t AxImage::height() const noexcept {
    return impl_->descriptor.height;
}

std::size_t AxImage::plane_count() const noexcept {
    return impl_->plane_count;
}

std::size_t AxImage::stride(std::size_t plane_index) const noexcept {
    if (plane_index >= impl_->plane_count) {
        return 0;
    }
    return impl_->descriptor.strides[plane_index];
}

std::size_t AxImage::plane_size(std::size_t plane_index) const noexcept {
    if (plane_index >= impl_->plane_count) {
        return 0;
    }
    return impl_->plane_sizes[plane_index];
}

std::size_t AxImage::byte_size() const noexcept {
    return impl_->total_size;
}

const ImageDescriptor& AxImage::descriptor() const noexcept {
    return impl_->descriptor;
}

MemoryType AxImage::memory_type() const noexcept {
    return impl_->memory_type;
}

CacheMode AxImage::cache_mode() const noexcept {
    return impl_->cache_mode;
}

std::uint64_t AxImage::physical_address(std::size_t plane_index) const noexcept {
    if (plane_index >= impl_->plane_count) {
        return 0;
    }
    return impl_->physical_addresses[plane_index];
}

void* AxImage::virtual_address(std::size_t plane_index) noexcept {
    if (plane_index >= impl_->plane_count) {
        return nullptr;
    }
    return impl_->virtual_addresses[plane_index];
}

const void* AxImage::virtual_address(std::size_t plane_index) const noexcept {
    if (plane_index >= impl_->plane_count) {
        return nullptr;
    }
    return impl_->virtual_addresses[plane_index];
}

std::uint32_t AxImage::block_id(std::size_t plane_index) const noexcept {
    if (plane_index >= impl_->plane_count) {
        return 0;
    }
    return impl_->block_ids[plane_index];
}

std::uint8_t* AxImage::mutable_plane_data(std::size_t plane_index) noexcept {
    return static_cast<std::uint8_t*>(virtual_address(plane_index));
}

const std::uint8_t* AxImage::plane_data(std::size_t plane_index) const noexcept {
    return static_cast<const std::uint8_t*>(virtual_address(plane_index));
}

void AxImage::Fill(std::uint8_t value) noexcept {
    if (impl_->virtual_addresses[0] == nullptr || impl_->total_size == 0) {
        return;
    }
    std::memset(impl_->virtual_addresses[0], value, impl_->total_size);
}

bool AxImage::FlushCache() noexcept {
    if (impl_->cache_mode != CacheMode::kCached || impl_->physical_addresses[0] == 0 ||
        impl_->virtual_addresses[0] == nullptr || impl_->total_size == 0) {
        return true;
    }

    return AX_SYS_MflushCache(impl_->physical_addresses[0], impl_->virtual_addresses[0],
                              static_cast<AX_U32>(impl_->total_size)) == AX_SUCCESS;
}

bool AxImage::InvalidateCache() noexcept {
    if (impl_->cache_mode != CacheMode::kCached || impl_->physical_addresses[0] == 0 ||
        impl_->virtual_addresses[0] == nullptr || impl_->total_size == 0) {
        return true;
    }

    return AX_SYS_MinvalidateCache(impl_->physical_addresses[0], impl_->virtual_addresses[0],
                                   static_cast<AX_U32>(impl_->total_size)) == AX_SUCCESS;
}

const AX_VIDEO_FRAME_INFO_T& internal::AxImageAccess::GetAxFrameInfo(const AxImage& image) noexcept {
    return image.impl_->frame_info;
}

AX_VIDEO_FRAME_INFO_T* internal::AxImageAccess::MutableAxFrameInfo(AxImage* image) noexcept {
    return image ? &image->impl_->frame_info : nullptr;
}

const AX_VIDEO_FRAME_T& internal::AxImageAccess::GetAxFrame(const AxImage& image) noexcept {
    return image.impl_->frame_info.stVFrame;
}

AX_VIDEO_FRAME_T* internal::AxImageAccess::MutableAxFrame(AxImage* image) noexcept {
    return image ? &image->impl_->frame_info.stVFrame : nullptr;
}

void internal::AxImageAccess::AttachLifetime(AxImage* image, std::shared_ptr<void> lifetime) noexcept {
    if (image == nullptr) {
        return;
    }
    image->impl_->lifetime_holder = std::move(lifetime);
}

}  // namespace axvsdk::common
