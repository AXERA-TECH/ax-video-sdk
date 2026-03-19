#pragma once

#include <cstdint>

namespace axvsdk::common {

enum class BackendType {
    kAuto = 0,
    kAxMsp,
    kAxcl,
};

enum class VdecModule {
    kBothVideoAndJpeg = 0,
    kVideoOnly = 1,
    kJpegOnly = 2,
};

enum class VencType {
    kVideoEncoder = 1,
    kJpegEncoder = 2,
    kMultiEncoder = 3,
};

struct SystemOptions {
    BackendType backend{BackendType::kAuto};
    std::int32_t device_id{-1};

    bool enable_vdec{true};
    bool enable_venc{true};
    bool enable_ivps{true};

    std::uint32_t vdec_max_group_count{16};
    VdecModule vdec_module{VdecModule::kBothVideoAndJpeg};
    bool enable_vdec_mc{false};
    std::int32_t vdec_virtual_channel{0};

    VencType venc_type{VencType::kMultiEncoder};
    std::uint32_t venc_total_thread_num{3};
    bool venc_explicit_sched{false};
};

bool InitializeSystem(const SystemOptions& options = {});
void ShutdownSystem() noexcept;
bool IsSystemInitialized() noexcept;
BackendType GetActiveBackend() noexcept;
std::int32_t GetActiveDeviceId() noexcept;

}  // namespace axvsdk::common
