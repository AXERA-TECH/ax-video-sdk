#include "codec/ax_jpeg_codec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ax_buffer_tool.h"
#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_sys.h"
#include "axcl_rt_memory.h"
#include "axcl_vdec.h"
#include "axcl_venc.h"
#define AX_SYS_MemAlloc AXCL_SYS_MemAlloc
#define AX_SYS_MemFree AXCL_SYS_MemFree
#define AX_VDEC_JpegDecodeOneFrame AXCL_VDEC_JpegDecodeOneFrame
#define AX_VENC_JpegEncodeOneFrame AXCL_VENC_JpegEncodeOneFrame
#else
#include "ax_sys_api.h"
#include "ax_vdec_api.h"
#include "ax_venc_api.h"
#endif

#include "ax_image_internal.h"
#include "common/ax_image_processor.h"
#include "common/ax_system.h"

namespace axvsdk::codec {

namespace {

constexpr std::uint32_t kJpegWidthAlignment = 64;

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

AX_IMG_FORMAT_E ToAxFormat(common::PixelFormat format) noexcept {
    switch (format) {
    case common::PixelFormat::kNv12:
        return AX_FORMAT_YUV420_SEMIPLANAR;
    case common::PixelFormat::kRgb24:
        return AX_FORMAT_RGB888;
    case common::PixelFormat::kBgr24:
        return AX_FORMAT_BGR888;
    case common::PixelFormat::kUnknown:
    default:
        return AX_FORMAT_INVALID;
    }
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* bytes) {
    if (bytes == nullptr) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    bytes->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !bytes->empty() || input.good();
}

std::string StripBase64Prefix(std::string text) {
    const auto comma = text.find(',');
    if (comma != std::string::npos) {
        const auto header = text.substr(0, comma);
        if (header.find("base64") != std::string::npos) {
            return text.substr(comma + 1);
        }
    }
    return text;
}

std::vector<std::uint8_t> DecodeBase64Bytes(const std::string& base64) {
    static const std::array<int, 256> kTable = [] {
        std::array<int, 256> table{};
        table.fill(-1);
        for (int c = 'A'; c <= 'Z'; ++c) {
            table[static_cast<std::size_t>(c)] = c - 'A';
        }
        for (int c = 'a'; c <= 'z'; ++c) {
            table[static_cast<std::size_t>(c)] = c - 'a' + 26;
        }
        for (int c = '0'; c <= '9'; ++c) {
            table[static_cast<std::size_t>(c)] = c - '0' + 52;
        }
        table[static_cast<std::size_t>('+')] = 62;
        table[static_cast<std::size_t>('/')] = 63;
        return table;
    }();

    const auto payload = StripBase64Prefix(base64);
    std::vector<std::uint8_t> output;
    int val = 0;
    int valb = -8;
    for (unsigned char ch : payload) {
        if (std::isspace(ch) != 0) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const int decoded = kTable[ch];
        if (decoded < 0) {
            return {};
        }
        val = (val << 6) + decoded;
        valb += 6;
        if (valb >= 0) {
            output.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return output;
}

std::string EncodeBase64Bytes(const std::uint8_t* data, std::size_t size) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (data == nullptr || size == 0) {
        return {};
    }

    std::string output;
    output.reserve(((size + 2U) / 3U) * 4U);

    std::size_t index = 0;
    while (index + 3U <= size) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data[index]) << 16U) |
            (static_cast<std::uint32_t>(data[index + 1U]) << 8U) |
            static_cast<std::uint32_t>(data[index + 2U]);
        output.push_back(kTable[(value >> 18U) & 0x3FU]);
        output.push_back(kTable[(value >> 12U) & 0x3FU]);
        output.push_back(kTable[(value >> 6U) & 0x3FU]);
        output.push_back(kTable[value & 0x3FU]);
        index += 3U;
    }

    if (index < size) {
        std::uint32_t value = static_cast<std::uint32_t>(data[index]) << 16U;
        output.push_back(kTable[(value >> 18U) & 0x3FU]);
        if (index + 1U < size) {
            value |= static_cast<std::uint32_t>(data[index + 1U]) << 8U;
            output.push_back(kTable[(value >> 12U) & 0x3FU]);
            output.push_back(kTable[(value >> 6U) & 0x3FU]);
            output.push_back('=');
        } else {
            output.push_back(kTable[(value >> 12U) & 0x3FU]);
            output.push_back('=');
            output.push_back('=');
        }
    }

    return output;
}

bool HasRequestedOutputTransform(const common::ImageDescriptor& requested,
                                 const common::AxImage& native) noexcept {
    if (requested.format != common::PixelFormat::kUnknown && requested.format != native.format()) {
        return true;
    }
    if (requested.width != 0 && requested.width != native.width()) {
        return true;
    }
    if (requested.height != 0 && requested.height != native.height()) {
        return true;
    }

    for (std::size_t plane = 0; plane < common::kMaxImagePlanes; ++plane) {
        if (requested.strides[plane] != 0 && requested.strides[plane] != native.stride(plane)) {
            return true;
        }
    }
    return false;
}

common::ImageDescriptor MakeNativeJpegDescriptor(std::uint32_t width, std::uint32_t height) noexcept {
    common::ImageDescriptor descriptor{};
    descriptor.format = common::PixelFormat::kNv12;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.strides[0] = AlignUp(width, kJpegWidthAlignment);
    descriptor.strides[1] = descriptor.strides[0];
    return descriptor;
}

bool IsStartOfFrameMarker(std::uint8_t marker) noexcept {
    switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

bool ParseJpegDimensions(const std::uint8_t* bytes,
                         std::size_t size,
                         std::uint32_t* width,
                         std::uint32_t* height) noexcept {
    if (bytes == nullptr || width == nullptr || height == nullptr || size < 4 ||
        bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return false;
    }

    std::size_t offset = 2;
    while (offset + 1 < size) {
        while (offset < size && bytes[offset] != 0xFF) {
            ++offset;
        }
        while (offset < size && bytes[offset] == 0xFF) {
            ++offset;
        }
        if (offset >= size) {
            break;
        }

        const auto marker = bytes[offset++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }

        if (offset + 1 >= size) {
            return false;
        }

        const auto segment_length =
            static_cast<std::uint32_t>(bytes[offset] << 8U) |
            static_cast<std::uint32_t>(bytes[offset + 1U]);
        offset += 2;
        if (segment_length < 2U || offset + segment_length - 2U > size) {
            return false;
        }

        if (IsStartOfFrameMarker(marker)) {
            if (segment_length < 7U) {
                return false;
            }
            *height = static_cast<std::uint32_t>(bytes[offset + 1U] << 8U) |
                      static_cast<std::uint32_t>(bytes[offset + 2U]);
            *width = static_cast<std::uint32_t>(bytes[offset + 3U] << 8U) |
                     static_cast<std::uint32_t>(bytes[offset + 4U]);
            return *width != 0 && *height != 0;
        }

        offset += segment_length - 2U;
    }

    return false;
}

common::AxImage::Ptr PostProcessDecodedImage(const common::AxImage::Ptr& native,
                                             const JpegDecodeOptions& options) {
    if (!native) {
        return nullptr;
    }

    if (!HasRequestedOutputTransform(options.output_image, *native)) {
        return native;
    }

    auto processor = common::CreateImageProcessor();
    if (!processor) {
        return nullptr;
    }

    common::ImageProcessRequest request{};
    request.output_image = options.output_image;
    return processor->Process(*native, request);
}

common::AxImage::Ptr DecodeJpegBytes(const std::uint8_t* bytes,
                                     std::size_t size,
                                     const JpegDecodeOptions& options) {
    if (!common::IsSystemInitialized() || bytes == nullptr || size == 0) {
        std::fprintf(stderr, "jpeg decode: system not initialized or input empty\n");
        return nullptr;
    }

    AX_U64 stream_phy_addr = 0;
    AX_VOID* stream_cpu_vir_addr = nullptr;
#if defined(AXSDK_PLATFORM_AXCL)
    // AXCL: JPEG bitstream lives on host; use pinned host memory for DMA.
    const auto host_ret = axclrtMallocHost(&stream_cpu_vir_addr, size);
    if (host_ret != AXCL_SUCC || stream_cpu_vir_addr == nullptr) {
        std::fprintf(stderr, "jpeg decode: axclrtMallocHost stream failed ret=0x%x size=%zu\n", host_ret, size);
        return nullptr;
    }
    std::memcpy(stream_cpu_vir_addr, bytes, size);
#else
    if (AX_SYS_MemAlloc(&stream_phy_addr, &stream_cpu_vir_addr, static_cast<AX_U32>(size), 0x100,
                        reinterpret_cast<const AX_S8*>("JpegDecodeStream")) != AX_SUCCESS) {
        std::fprintf(stderr, "jpeg decode: AX_SYS_MemAlloc stream failed size=%zu\n", size);
        return nullptr;
    }
    std::memcpy(stream_cpu_vir_addr, bytes, size);
#endif

    std::uint32_t jpeg_width = 0;
    std::uint32_t jpeg_height = 0;
    if (!ParseJpegDimensions(bytes, size, &jpeg_width, &jpeg_height)) {
        std::fprintf(stderr, "jpeg decode: ParseJpegDimensions failed size=%zu\n", size);
#if defined(AXSDK_PLATFORM_AXCL)
        (void)axclrtFreeHost(stream_cpu_vir_addr);
#else
        (void)AX_SYS_MemFree(stream_phy_addr, stream_cpu_vir_addr);
#endif
        return nullptr;
    }

    common::ImageAllocationOptions image_options{};
    image_options.memory_type = common::MemoryType::kCmm;
    image_options.cache_mode = common::CacheMode::kNonCached;
    image_options.alignment = 0x1000;
    image_options.token = "JpegDecode";
    auto native = common::AxImage::Create(
        MakeNativeJpegDescriptor(jpeg_width, jpeg_height), image_options);
    if (!native) {
        std::fprintf(stderr, "jpeg decode: native image alloc failed width=%u height=%u\n",
                     jpeg_width, jpeg_height);
#if defined(AXSDK_PLATFORM_AXCL)
        (void)axclrtFreeHost(stream_cpu_vir_addr);
#else
        (void)AX_SYS_MemFree(stream_phy_addr, stream_cpu_vir_addr);
#endif
        return nullptr;
    }

    auto* native_frame = common::internal::AxImageAccess::MutableAxFrame(native.get());
    if (native_frame == nullptr) {
        std::fprintf(stderr, "jpeg decode: native frame access failed\n");
#if defined(AXSDK_PLATFORM_AXCL)
        (void)axclrtFreeHost(stream_cpu_vir_addr);
#else
        (void)AX_SYS_MemFree(stream_phy_addr, stream_cpu_vir_addr);
#endif
        return nullptr;
    }

    AX_VDEC_DEC_ONE_FRM_T decode_param{};
    decode_param.stStream.u64PhyAddr = stream_phy_addr;
    decode_param.stStream.pu8Addr = static_cast<AX_U8*>(stream_cpu_vir_addr);
    decode_param.stStream.u32StreamPackLen = static_cast<AX_U32>(size);
    decode_param.stFrame = *native_frame;
#if defined(AXSDK_CHIP_AX650) || defined(AXSDK_PLATFORM_AXCL)
    decode_param.enOutputMode = AX_VDEC_OUTPUT_ORIGINAL;
    decode_param.enImgFormat = AX_FORMAT_YUV420_SEMIPLANAR;
#endif

    const auto decode_ret = AX_VDEC_JpegDecodeOneFrame(&decode_param);
    if (decode_ret != AX_SUCCESS) {
        std::fprintf(stderr,
                     "jpeg decode: JpegDecodeOneFrame failed ret=0x%x width=%u height=%u stride=%u fmt=%d\n",
                     decode_ret, jpeg_width, jpeg_height,
                     native_frame->u32PicStride[0], static_cast<int>(decode_param.stFrame.enImgFormat));
#if defined(AXSDK_PLATFORM_AXCL)
        (void)axclrtFreeHost(stream_cpu_vir_addr);
#else
        (void)AX_SYS_MemFree(stream_phy_addr, stream_cpu_vir_addr);
#endif
        return nullptr;
    }

    *native_frame = decode_param.stFrame;
#if defined(AXSDK_PLATFORM_AXCL)
    (void)axclrtFreeHost(stream_cpu_vir_addr);
#else
    (void)AX_SYS_MemFree(stream_phy_addr, stream_cpu_vir_addr);
#endif
    return PostProcessDecodedImage(native, options);
}

common::AxImage::Ptr PrepareJpegEncodeInput(const common::AxImage& image) {
    if (!common::IsSystemInitialized() || image.width() == 0 || image.height() == 0) {
        return nullptr;
    }

    if (image.format() == common::PixelFormat::kNv12 && image.physical_address(0) != 0 &&
        image.virtual_address(0) != nullptr && image.stride(0) == AlignUp(image.width(), kJpegWidthAlignment) &&
        image.stride(1) == image.stride(0)) {
        return common::AxImage::Ptr(const_cast<common::AxImage*>(&image), [](common::AxImage*) {});
    }

    auto processor = common::CreateImageProcessor();
    if (!processor) {
        return nullptr;
    }

    common::ImageDescriptor descriptor{};
    descriptor.format = common::PixelFormat::kNv12;
    descriptor.width = image.width();
    descriptor.height = image.height();
    descriptor.strides[0] = AlignUp(image.width(), kJpegWidthAlignment);
    descriptor.strides[1] = descriptor.strides[0];

    common::ImageAllocationOptions options{};
    options.memory_type = common::MemoryType::kCmm;
    options.cache_mode = common::CacheMode::kNonCached;
    options.alignment = 0x1000;
    options.token = "JpegEncodeInput";
    auto output = common::AxImage::Create(descriptor, options);
    if (!output) {
        return nullptr;
    }

    common::ImageProcessRequest request{};
    request.output_image = descriptor;
    if (!processor->Process(image, request, *output)) {
        return nullptr;
    }
    return output;
}

std::vector<std::uint8_t> EncodeJpegBytes(const common::AxImage& image,
                                          const JpegEncodeOptions& options) {
    if (!common::IsSystemInitialized()) {
        return {};
    }

    auto working_image = PrepareJpegEncodeInput(image);
    if (!working_image) {
        return {};
    }

    auto& mutable_image = const_cast<common::AxImage&>(*working_image);
    if (!mutable_image.FlushCache()) {
        return {};
    }

    const auto stream_buffer_size = static_cast<AX_U32>(std::max<std::size_t>(
        working_image->byte_size(), static_cast<std::size_t>(64U * 1024U)));
    AX_U64 stream_phy_addr = 0;
    AX_VOID* stream_vir_addr = nullptr;
    if (AX_SYS_MemAlloc(&stream_phy_addr, &stream_vir_addr, stream_buffer_size, 0x1000,
                        reinterpret_cast<const AX_S8*>("JpegEncode")) != AX_SUCCESS) {
        return {};
    }

    AX_JPEG_ENCODE_ONCE_PARAMS_T encode_param{};
    encode_param.u32Width = working_image->width();
    encode_param.u32Height = working_image->height();
    encode_param.enImgFormat = ToAxFormat(working_image->format());
    for (std::size_t plane = 0; plane < common::kMaxImagePlanes; ++plane) {
        encode_param.u32PicStride[plane] = static_cast<AX_U32>(working_image->stride(plane));
        encode_param.u64PhyAddr[plane] = working_image->physical_address(plane);
        encode_param.u64VirAddr[plane] =
            static_cast<AX_U64>(reinterpret_cast<std::uintptr_t>(working_image->virtual_address(plane)));
    }
    encode_param.ulPhyAddr = stream_phy_addr;
    encode_param.pu8Addr = static_cast<AX_U8*>(stream_vir_addr);
    encode_param.u32Len = stream_buffer_size;
    encode_param.enStrmBufType = AX_STREAM_BUF_NON_CACHE;
#if !defined(AXSDK_CHIP_AX650) && !defined(AXSDK_PLATFORM_AXCL)
    encode_param.u32OutBufSize = stream_buffer_size;
#endif
    encode_param.stJpegParam.u32Qfactor = std::clamp<std::uint32_t>(options.quality, 1U, 99U);
    encode_param.stJpegParam.bDblkEnable = AX_FALSE;

    std::vector<std::uint8_t> encoded;
    const auto encode_ret = AX_VENC_JpegEncodeOneFrame(&encode_param);
    if (encode_ret == AX_SUCCESS && encode_param.u32Len != 0) {
        encoded.assign(encode_param.pu8Addr, encode_param.pu8Addr + encode_param.u32Len);
    } else if (encode_ret != AX_SUCCESS) {
        std::fprintf(stderr,
                     "jpeg encode: AX_VENC_JpegEncodeOneFrame failed ret=0x%x width=%u height=%u fmt=%d stride0=%u stride1=%u stream_buf=%u\n",
                     encode_ret, encode_param.u32Width, encode_param.u32Height, static_cast<int>(encode_param.enImgFormat),
                     encode_param.u32PicStride[0], encode_param.u32PicStride[1], stream_buffer_size);
    }

    (void)AX_SYS_MemFree(stream_phy_addr, stream_vir_addr);
    return encoded;
}

}  // namespace

common::AxImage::Ptr DecodeJpegFile(const std::string& path, const JpegDecodeOptions& options) {
    std::vector<std::uint8_t> bytes;
    if (!ReadFile(path, &bytes)) {
        return nullptr;
    }
    return DecodeJpegBytes(bytes.data(), bytes.size(), options);
}

common::AxImage::Ptr DecodeJpegMemory(const void* data,
                                      std::size_t size,
                                      const JpegDecodeOptions& options) {
    return DecodeJpegBytes(static_cast<const std::uint8_t*>(data), size, options);
}

common::AxImage::Ptr DecodeJpegBase64(const std::string& base64,
                                      const JpegDecodeOptions& options) {
    const auto bytes = DecodeBase64Bytes(base64);
    if (bytes.empty()) {
        return nullptr;
    }
    return DecodeJpegBytes(bytes.data(), bytes.size(), options);
}

std::vector<std::uint8_t> EncodeJpegToMemory(const common::AxImage& image,
                                             const JpegEncodeOptions& options) {
    return EncodeJpegBytes(image, options);
}

bool EncodeJpegToFile(const common::AxImage& image,
                      const std::string& path,
                      const JpegEncodeOptions& options) {
    const auto bytes = EncodeJpegBytes(image, options);
    if (bytes.empty()) {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::string EncodeJpegToBase64(const common::AxImage& image,
                               const JpegEncodeOptions& options) {
    const auto bytes = EncodeJpegBytes(image, options);
    return EncodeBase64Bytes(bytes.data(), bytes.size());
}

}  // namespace axvsdk::codec
