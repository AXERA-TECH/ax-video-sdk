#include <cstdio>

#include "ax_image_internal.h"
#include "common/ax_image.h"
#include "common/ax_system.h"

int main() {
    axvsdk::common::SystemOptions system_options{};
    system_options.enable_vdec = false;
    system_options.enable_venc = false;
    system_options.enable_ivps = false;
    if (!axvsdk::common::InitializeSystem(system_options)) {
        std::fprintf(stderr, "initialize system failed\n");
        return 2;
    }

    axvsdk::common::ImageDescriptor descriptor{};
    descriptor.format = axvsdk::common::PixelFormat::kRgb24;
    descriptor.width = 320;
    descriptor.height = 240;
    descriptor.strides[0] = 320 * 3;

    axvsdk::common::ImageAllocationOptions options{};
    options.memory_type = axvsdk::common::MemoryType::kCmm;
    options.cache_mode = axvsdk::common::CacheMode::kCached;
    options.alignment = 0x1000;
    options.token = "AxImageStrideSmoke";
    auto image = axvsdk::common::AxImage::Create(descriptor, options);
    if (!image) {
        std::fprintf(stderr, "create RGB image failed\n");
        axvsdk::common::ShutdownSystem();
        return 3;
    }

    const auto& frame = axvsdk::common::internal::AxImageAccess::GetAxFrame(*image);
    const auto expected_pic_stride = descriptor.width;
    if (frame.u32PicStride[0] != expected_pic_stride) {
        std::fprintf(stderr,
                     "RGB pic stride mismatch: got %u expected %u (byte stride %zu)\n",
                     frame.u32PicStride[0], expected_pic_stride, descriptor.strides[0]);
        axvsdk::common::ShutdownSystem();
        return 1;
    }

    axvsdk::common::ShutdownSystem();
    return 0;
}
