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

enum class PixelFormat {
    kUnknown = 0,
    kNv12,
    kRgb24,
    kBgr24,
};

enum class MemoryType {
    kAuto = 0,
    kCmm,
    kPool,
    kExternal,
};

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
    std::array<std::size_t, kMaxImagePlanes> strides{};
};

struct ImageAllocationOptions {
    MemoryType memory_type{MemoryType::kAuto};
    CacheMode cache_mode{CacheMode::kNonCached};
    std::uint32_t alignment{0x1000};
    std::uint32_t pool_id{kInvalidPoolId};
    std::string partition_name;
    std::string token{"AxImage"};
};

struct ExternalImagePlane {
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
    static Ptr Create(const ImageDescriptor& descriptor, const ImageAllocationOptions& options = {});
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

    std::uint64_t physical_address(std::size_t plane_index) const noexcept;
    void* virtual_address(std::size_t plane_index) noexcept;
    const void* virtual_address(std::size_t plane_index) const noexcept;
    std::uint32_t block_id(std::size_t plane_index) const noexcept;

    std::uint8_t* mutable_plane_data(std::size_t plane_index) noexcept;
    const std::uint8_t* plane_data(std::size_t plane_index) const noexcept;
    void Fill(std::uint8_t value) noexcept;
    bool FlushCache() noexcept;
    bool InvalidateCache() noexcept;

private:
    struct Impl;
    friend struct internal::AxImageAccess;

    explicit AxImage(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace axvsdk::common
