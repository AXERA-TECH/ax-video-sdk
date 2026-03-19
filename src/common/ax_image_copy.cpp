#include "ax_image_copy.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_ivps.h"
#include "axcl_rt_memory.h"
#define AX_IVPS_CmmCopyVpp AXCL_IVPS_CmmCopyVpp
#define AX_IVPS_CscVpp AXCL_IVPS_CscVpp
#else
#include "ax_ivps_api.h"
#include "ax_sys_api.h"
#endif

#include "ax_image_internal.h"
#include "ax_system_internal.h"

namespace axvsdk::common::internal {

namespace {

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

std::size_t PlaneCount(PixelFormat format) noexcept {
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

std::size_t PlaneRows(const ImageDescriptor& descriptor, std::size_t plane_index) noexcept {
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

std::size_t PlaneRowBytes(const ImageDescriptor& descriptor, std::size_t plane_index) noexcept {
    switch (descriptor.format) {
    case PixelFormat::kNv12:
        return plane_index < 2 ? descriptor.width : 0;
    case PixelFormat::kRgb24:
    case PixelFormat::kBgr24:
        return plane_index == 0 ? static_cast<std::size_t>(descriptor.width) * 3U : 0;
    case PixelFormat::kUnknown:
    default:
        return 0;
    }
}

bool CopyPlaneByIvps(AX_U64 src_phy_addr,
                     std::size_t src_stride,
                     AX_U64 dst_phy_addr,
                     std::size_t dst_stride,
                     std::size_t rows,
                     std::size_t row_bytes) noexcept {
#if defined(AXSDK_CHIP_AX650) || defined(AXSDK_PLATFORM_AXCL)
    if (src_phy_addr == 0 || dst_phy_addr == 0 || rows == 0 || row_bytes == 0 ||
        src_stride < row_bytes || dst_stride < row_bytes) {
        return false;
    }

    if (src_stride == dst_stride) {
        const auto copy_bytes = static_cast<AX_U32>(dst_stride * rows);
        return AX_IVPS_CmmCopyVpp(src_phy_addr, dst_phy_addr, copy_bytes) == AX_SUCCESS;
    }

    for (std::size_t row = 0; row < rows; ++row) {
        const auto ret = AX_IVPS_CmmCopyVpp(src_phy_addr + row * src_stride,
                                            dst_phy_addr + row * dst_stride,
                                            static_cast<AX_U32>(row_bytes));
        if (ret != AX_SUCCESS) {
            return false;
        }
    }

    return true;
#else
    (void)src_phy_addr;
    (void)src_stride;
    (void)dst_phy_addr;
    (void)dst_stride;
    (void)rows;
    (void)row_bytes;
    return false;
#endif
}

bool CopyFrameByCsc(const AX_VIDEO_FRAME_T& source_frame, AxImage* destination) noexcept {
#if defined(AXSDK_CHIP_AX650) || defined(AXSDK_PLATFORM_AXCL)
    if (destination == nullptr) {
        return false;
    }

    auto* dst_frame = AxImageAccess::MutableAxFrame(destination);
    if (dst_frame == nullptr) {
        return false;
    }

    AX_VIDEO_FRAME_T src_frame = source_frame;
    return AX_IVPS_CscVpp(&src_frame, dst_frame) == AX_SUCCESS && destination->InvalidateCache();
#else
    (void)source_frame;
    (void)destination;
    return false;
#endif
}

bool CopyFrameByCmm(const AX_VIDEO_FRAME_T& source_frame, AxImage* destination) noexcept {
#if defined(AXSDK_CHIP_AX650) || defined(AXSDK_PLATFORM_AXCL)
    if (destination == nullptr || source_frame.u64PhyAddr[0] == 0 || destination->physical_address(0) == 0 ||
        destination->byte_size() == 0) {
        return false;
    }

    return AX_IVPS_CmmCopyVpp(source_frame.u64PhyAddr[0], destination->physical_address(0),
                              static_cast<AX_U32>(destination->byte_size())) == AX_SUCCESS &&
           destination->InvalidateCache();
#else
    (void)source_frame;
    (void)destination;
    return false;
#endif
}

#if defined(AXSDK_PLATFORM_AXCL)
bool CopyPlaneByRuntime(const void* src,
                        std::size_t src_stride,
                        bool src_is_device,
                        void* dst,
                        std::size_t dst_stride,
                        bool dst_is_device,
                        std::size_t rows,
                        std::size_t row_bytes) noexcept {
    if (src == nullptr || dst == nullptr || rows == 0 || row_bytes == 0 ||
        src_stride < row_bytes || dst_stride < row_bytes) {
        return false;
    }
    if (!EnsureAxclThreadContext()) {
        return false;
    }

    axclrtMemcpyKind kind = AXCL_MEMCPY_HOST_TO_HOST;
    if (src_is_device && dst_is_device) {
        kind = AXCL_MEMCPY_DEVICE_TO_DEVICE;
    } else if (!src_is_device && dst_is_device) {
        kind = AXCL_MEMCPY_HOST_TO_DEVICE;
    } else if (src_is_device && !dst_is_device) {
        kind = AXCL_MEMCPY_DEVICE_TO_HOST;
    }

    for (std::size_t row = 0; row < rows; ++row) {
        auto* dst_row = static_cast<std::uint8_t*>(dst) + row * dst_stride;
        const auto* src_row = static_cast<const std::uint8_t*>(src) + row * src_stride;
        if (axclrtMemcpy(dst_row, src_row, row_bytes, kind) != AXCL_SUCC) {
            return false;
        }
    }

    return true;
}
#endif

void CopyPlaneByCpu(const std::uint8_t* src,
                    std::size_t src_stride,
                    std::uint8_t* dst,
                    std::size_t dst_stride,
                    std::size_t rows,
                    std::size_t row_bytes) noexcept {
    for (std::size_t row = 0; row < rows; ++row) {
        std::memcpy(dst + row * dst_stride, src + row * src_stride, row_bytes);
    }
}

bool CopyImageImpl(const ImageDescriptor& source_descriptor,
                   const AX_VIDEO_FRAME_T* source_frame,
                   const std::uint64_t* source_phy_addrs,
                   const void* const* source_vir_addrs,
                   const std::size_t* source_strides,
                   AxImage* destination) noexcept {
    if (destination == nullptr || source_descriptor.format != destination->format() ||
        source_descriptor.width != destination->width() || source_descriptor.height != destination->height()) {
        return false;
    }

    const auto plane_count = PlaneCount(source_descriptor.format);
    if (plane_count == 0) {
        return false;
    }

    if (source_frame != nullptr) {
        if (plane_count == 1 && CopyFrameByCmm(*source_frame, destination)) {
            return true;
        }

        bool ivps_copy_ok = true;
        for (std::size_t plane = 0; plane < plane_count; ++plane) {
            const auto rows = PlaneRows(source_descriptor, plane);
            const auto row_bytes = PlaneRowBytes(source_descriptor, plane);
            const auto src_stride = static_cast<std::size_t>(source_frame->u32PicStride[plane]);
            const auto dst_stride = destination->stride(plane);
            if (row_bytes == 0 || src_stride < row_bytes || dst_stride < row_bytes) {
                ivps_copy_ok = false;
                break;
            }

            if (!CopyPlaneByIvps(source_frame->u64PhyAddr[plane], src_stride,
                                 destination->physical_address(plane), dst_stride,
                                 rows, row_bytes)) {
                ivps_copy_ok = false;
                break;
            }
        }

        if (ivps_copy_ok) {
            return destination->InvalidateCache();
        }

        if (CopyFrameByCsc(*source_frame, destination)) {
            return true;
        }
    } else {
        bool ivps_copy_ok = true;
        for (std::size_t plane = 0; plane < plane_count; ++plane) {
            const auto rows = PlaneRows(source_descriptor, plane);
            const auto row_bytes = PlaneRowBytes(source_descriptor, plane);
            const auto src_stride = source_strides[plane];
            const auto dst_stride = destination->stride(plane);
            if (row_bytes == 0 || src_stride < row_bytes || dst_stride < row_bytes) {
                return false;
            }

            if (!CopyPlaneByIvps(source_phy_addrs[plane], src_stride,
                                 destination->physical_address(plane), dst_stride,
                                 rows, row_bytes)) {
                ivps_copy_ok = false;
                break;
            }
        }

        if (ivps_copy_ok) {
            return destination->InvalidateCache();
        }
    }

    for (std::size_t plane = 0; plane < plane_count; ++plane) {
#if defined(AXSDK_PLATFORM_AXCL)
        const bool src_is_device = source_phy_addrs[plane] != 0;
        const bool dst_is_device = destination->physical_address(plane) != 0;
        if (src_is_device || dst_is_device) {
            const void* runtime_src = src_is_device ? reinterpret_cast<const void*>(source_phy_addrs[plane])
                                                    : source_vir_addrs[plane];
            void* runtime_dst = dst_is_device ? reinterpret_cast<void*>(destination->physical_address(plane))
                                              : destination->virtual_address(plane);
            if (!CopyPlaneByRuntime(runtime_src, source_strides[plane], src_is_device,
                                    runtime_dst, destination->stride(plane), dst_is_device,
                                    PlaneRows(source_descriptor, plane), PlaneRowBytes(source_descriptor, plane))) {
                return false;
            }
            continue;
        }
#endif

        const auto* src = static_cast<const std::uint8_t*>(source_vir_addrs[plane]);
        auto* dst = destination->mutable_plane_data(plane);
        if (src == nullptr || dst == nullptr) {
            return false;
        }

        CopyPlaneByCpu(src, source_strides[plane], dst, destination->stride(plane),
                       PlaneRows(source_descriptor, plane), PlaneRowBytes(source_descriptor, plane));
    }

    return destination->FlushCache();
}

}  // namespace

bool CopyImage(const AxImage& source, AxImage* destination) noexcept {
    if (destination == nullptr || source.format() != destination->format() ||
        source.width() != destination->width() || source.height() != destination->height()) {
        return false;
    }

    const auto plane_count = PlaneCount(source.format());
    if (plane_count == 0) {
        return false;
    }

    auto& mutable_source = const_cast<AxImage&>(source);
    (void)mutable_source.FlushCache();
    const auto& source_frame = AxImageAccess::GetAxFrame(source);

    std::uint64_t source_phy_addrs[kMaxImagePlanes]{};
    const void* source_vir_addrs[kMaxImagePlanes]{};
    std::size_t source_strides[kMaxImagePlanes]{};
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        source_phy_addrs[plane] = source.physical_address(plane);
        source_vir_addrs[plane] = source.plane_data(plane);
        source_strides[plane] = source.stride(plane);
    }

    if (!CopyImageImpl(source.descriptor(), &source_frame, source_phy_addrs, source_vir_addrs, source_strides,
                       destination)) {
        return false;
    }

    return true;
}

bool CopyVideoFrameToImage(const AX_VIDEO_FRAME_INFO_T& frame_info, AxImage* destination) noexcept {
    if (destination == nullptr) {
        return false;
    }

    ImageDescriptor source_descriptor{};
    source_descriptor.format = FromAxFormat(frame_info.stVFrame.enImgFormat);
    source_descriptor.width = frame_info.stVFrame.u32Width;
    source_descriptor.height = frame_info.stVFrame.u32Height;
    const auto plane_count = PlaneCount(source_descriptor.format);
    if (plane_count == 0 || source_descriptor.width == 0 || source_descriptor.height == 0) {
        return false;
    }

    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        source_descriptor.strides[plane] = frame_info.stVFrame.u32PicStride[plane];
    }

    std::uint64_t source_phy_addrs[kMaxImagePlanes]{};
    const void* source_vir_addrs[kMaxImagePlanes]{};
    std::size_t source_strides[kMaxImagePlanes]{};
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        source_phy_addrs[plane] = frame_info.stVFrame.u64PhyAddr[plane];
        source_vir_addrs[plane] = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(frame_info.stVFrame.u64VirAddr[plane]));
        source_strides[plane] = frame_info.stVFrame.u32PicStride[plane];
    }

    return CopyImageImpl(source_descriptor, &frame_info.stVFrame, source_phy_addrs, source_vir_addrs,
                         source_strides, destination);
}

}  // namespace axvsdk::common::internal
