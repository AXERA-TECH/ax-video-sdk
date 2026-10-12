#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace axvsdk::common {

namespace internal {
struct AxImageAccess;
}

// SDK 当前公开支持的像素格式。
// 硬件编解码原生输入输出统一按 NV12 处理；RGB/BGR 一般用于图像处理、
// frame output、JPEG 输入输出等场景。
enum class PixelFormat {
    kUnknown = 0,
    kNv12,
    kRgb24,
    kBgr24,
};

// AxImage 底层内存来源。
// kAuto: 由库按当前平台默认策略选择，当前等价于 CMM。
// kCmm: 由 AX CMM 分配，可用于硬件模块直连。
// kPool: 来自 AX pool，常用于解码输出等硬件帧。
// kExternal: 外部传入内存，库不拥有底层内存。
enum class MemoryType {
    kAuto = 0,
    kCmm,
    kPool,
    kExternal,
};

// 仅对有 cache 属性的 AX 内存生效。
// kNonCached 适合大多数硬件直连场景；kCached 适合 CPU 频繁访问场景，
// 但调用方需要在读写前后正确配合 FlushCache / InvalidateCache。
enum class CacheMode {
    kNonCached = 0,
    kCached,
};

constexpr std::size_t kMaxImagePlanes = 3;
constexpr std::uint32_t kInvalidPoolId = 0xFFFFFFFFU;

struct ImageDescriptor {
    PixelFormat format{PixelFormat::kUnknown};
    std::uint32_t width{0};
    std::uint32_t height{0};
    // stride 为每个 plane 的字节跨度。
    // 传 0 表示由库按平台默认对齐规则自动推导。
    std::array<std::size_t, kMaxImagePlanes> strides{};
};

struct ImageAllocationOptions {
    // 默认使用 kAuto，由库选择适合硬件模块使用的内存类型。
    MemoryType memory_type{MemoryType::kAuto};
    CacheMode cache_mode{CacheMode::kNonCached};
    // CMM 分配对齐字节数，默认 4KB。
    std::uint32_t alignment{0x1000};
    // 仅 memory_type == kPool 时生效。
    std::uint32_t pool_id{kInvalidPoolId};
    // 某些平台 / 驱动下用于选择分区。
    std::string partition_name;
    // 传给底层分配接口的调试 token，便于排查内存来源。
    std::string token{"AxImage"};
};

struct ExternalImagePlane {
    // 外部内存至少需要提供 virtual_address。
    // 如果同时提供 physical_address / block_id，则库可以更高效地走硬件 copy。
    void* virtual_address{nullptr};
    std::uint64_t physical_address{0};
    std::uint32_t block_id{kInvalidPoolId};
};

class AxImage {
public:
    using Ptr = std::shared_ptr<AxImage>;
    using LifetimeHolder = std::shared_ptr<void>;

    static Ptr Create(PixelFormat format,
                      std::uint32_t width,
                      std::uint32_t height,
                      const ImageAllocationOptions& options = {});
    // 使用指定描述符和分配选项创建一张新的图像。
    // 默认走库内部分配，当前默认内存类型为 CMM。
    static Ptr Create(const ImageDescriptor& descriptor, const ImageAllocationOptions& options = {});
    // 包装外部图像内存。
    // 外部内存生命周期由 lifetime 持有；当 memory_type 为 kExternal 时，
    // AxImage 不会主动释放底层内存。
    static Ptr WrapExternal(const ImageDescriptor& descriptor,
                            const std::array<ExternalImagePlane, kMaxImagePlanes>& planes,
                            LifetimeHolder lifetime = {});

    AxImage(const AxImage&) = delete;
    AxImage& operator=(const AxImage&) = delete;
    AxImage(AxImage&&) noexcept;
    AxImage& operator=(AxImage&&) noexcept;
    ~AxImage();

    PixelFormat format() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::size_t plane_count() const noexcept;
    std::size_t stride(std::size_t plane_index) const noexcept;
    std::size_t plane_size(std::size_t plane_index) const noexcept;
    std::size_t byte_size() const noexcept;
    const ImageDescriptor& descriptor() const noexcept;
    MemoryType memory_type() const noexcept;
    CacheMode cache_mode() const noexcept;

    // 帧标识。pts 来自码流(硬件透传);seq 是 pipeline 为每个解码帧盖的单调序号,
    // 用于把"异步算出的检测结果"和"当前正在编码的帧"配对,进而算出结果过期了几帧。
    std::uint64_t pts() const noexcept;
    std::uint64_t seq() const noexcept;
    void set_seq(std::uint64_t seq) noexcept;

    std::uint64_t physical_address(std::size_t plane_index) const noexcept;
    void* virtual_address(std::size_t plane_index) noexcept;
    const void* virtual_address(std::size_t plane_index) const noexcept;
    std::uint32_t block_id(std::size_t plane_index) const noexcept;

    // 返回当前 plane 的 CPU 可访问地址。
    // 对 AXCL 设备侧图像，这个地址不一定可被 host 直接访问；
    // 如需 host 读写，应先显式复制到 host 可访问内存。
    std::uint8_t* mutable_plane_data(std::size_t plane_index) noexcept;
    const std::uint8_t* plane_data(std::size_t plane_index) const noexcept;
    void Fill(std::uint8_t value) noexcept;

    // 把各 plane 紧密拷到 host 缓冲(dst_size 至少要有 byte_size() 那么大)。
    // AXCL 下设备侧图像**不能**被 CPU 直接读,plane_data() 拿到的地址解引用会段错误;
    // 要在 host 侧处理像素就用这个(内部走 PCIe memcpy)。板端是 cache invalidate + memcpy。
    bool CopyToHost(void* dst, std::size_t dst_size) const noexcept;
    // 反方向:host 缓冲 → 图像各 plane,写完会 flush,可直接送硬件。
    bool CopyFromHost(const void* src, std::size_t src_size) noexcept;

    // CPU 写 cached 内存后，送入硬件前调用。
    bool FlushCache() noexcept;
    // 硬件写 cached 内存后，CPU 读取前调用。
    bool InvalidateCache() noexcept;

private:
    struct Impl;
    friend struct internal::AxImageAccess;

    explicit AxImage(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace axvsdk::common
