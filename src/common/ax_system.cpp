#include "common/ax_system.h"
#include "ax_system_internal.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl.h"
#include "axcl_ivps.h"
#include "axcl_rt_context.h"
#include "axcl_rt_device.h"
#include "axcl_sys.h"
#include "axcl_vdec.h"
#include "axcl_venc.h"
#else
#include "ax_ivps_api.h"
#include "ax_sys_api.h"
#include "ax_vdec_api.h"
#include "ax_vdec_type.h"
#include "ax_venc_api.h"
#include "ax_venc_comm.h"
#endif

namespace axvsdk::common {

namespace {

#if defined(AXSDK_CHIP_AX650)
AX_VDEC_ENABLE_MOD_E ToAxVdecModule(VdecModule module) noexcept {
    switch (module) {
    case VdecModule::kVideoOnly:
        return AX_ENABLE_ONLY_VDEC;
    case VdecModule::kJpegOnly:
        return AX_ENABLE_ONLY_JDEC;
    case VdecModule::kBothVideoAndJpeg:
    default:
        return AX_ENABLE_BOTH_VDEC_JDEC;
    }
}
#endif

AX_VENC_ENCODER_TYPE_E ToAxVencType(VencType type) noexcept {
    switch (type) {
    case VencType::kVideoEncoder:
        return AX_VENC_VIDEO_ENCODER;
    case VencType::kJpegEncoder:
        return AX_VENC_JPEG_ENCODER;
    case VencType::kMultiEncoder:
    default:
        return AX_VENC_MULTI_ENCODER;
    }
}

struct SystemState {
    struct DeviceState {
        bool sys_initialized{false};
        bool vdec_initialized{false};
        bool venc_initialized{false};
        bool ivps_initialized{false};
        std::size_t ref_count{0};
    };

    bool axcl_initialized{false};
    BackendType backend{BackendType::kAuto};
    std::int32_t active_device_id{-1};
    std::size_t ref_count{0};
    std::map<std::int32_t, DeviceState> devices;
};

#if defined(AXSDK_PLATFORM_AXCL)
struct ThreadAxclContext {
    ~ThreadAxclContext() {
        if (context != nullptr) {
            (void)axclrtDestroyContext(context);
        }
    }

    axclrtContext context{nullptr};
    std::int32_t device_id{-1};
};

ThreadAxclContext& GetThreadAxclContext() {
    thread_local ThreadAxclContext thread_context;
    return thread_context;
}
#endif

SystemState& GetSystemState() {
    static SystemState state;
    return state;
}

std::mutex& GetSystemMutex() {
    static std::mutex mutex;
    return mutex;
}

AX_VDEC_MOD_ATTR_T MakeVdecModAttr(const SystemOptions& options) {
    AX_VDEC_MOD_ATTR_T mod_attr{};
    mod_attr.u32MaxGroupCount = options.vdec_max_group_count;
#if defined(AXSDK_CHIP_AX650)
    mod_attr.enDecModule = ToAxVdecModule(options.vdec_module);
    mod_attr.bVdecMc = options.enable_vdec_mc ? AX_TRUE : AX_FALSE;
    mod_attr.VdecVirtChn = static_cast<AX_VDEC_CHN>(options.vdec_virtual_channel);
#endif
    return mod_attr;
}

AX_VENC_MOD_ATTR_T MakeVencModAttr(const SystemOptions& options) {
    AX_VENC_MOD_ATTR_T mod_attr{};
    mod_attr.enVencType = ToAxVencType(options.venc_type);
    mod_attr.stModThdAttr.u32TotalThreadNum = options.venc_total_thread_num;
    mod_attr.stModThdAttr.bExplicitSched = options.venc_explicit_sched ? AX_TRUE : AX_FALSE;
    return mod_attr;
}

#if defined(AXSDK_PLATFORM_AXCL)
bool ResolveAxclDeviceIndex(std::int32_t requested_device_index, std::int32_t* resolved_device_id) {
    if (resolved_device_id == nullptr) {
        return false;
    }

    axclrtDeviceList device_list{};
    const auto ret = axclrtGetDeviceList(&device_list);
    if (ret != AXCL_SUCC || device_list.num == 0) {
        std::cerr << "axclrtGetDeviceList failed or no device found, ret=0x" << std::hex << ret << std::dec << "\n";
        return false;
    }

    if (requested_device_index < 0) {
        *resolved_device_id = device_list.devices[0];
        return true;
    }

    if (requested_device_index < static_cast<std::int32_t>(device_list.num)) {
        *resolved_device_id = device_list.devices[requested_device_index];
        return true;
    }

    std::cerr << "invalid AXCL device index: " << requested_device_index << "\n";
    return false;
}
#endif

}  // namespace

bool InitializeSystem(const SystemOptions& options) {
    std::lock_guard<std::mutex> lock(GetSystemMutex());
    auto& state = GetSystemState();

    const auto backend =
#if defined(AXSDK_PLATFORM_AXCL)
        BackendType::kAxcl;
#else
        BackendType::kAxMsp;
#endif

#if defined(AXSDK_PLATFORM_AXCL)
    bool did_axcl = false;
    bool did_device = false;
    axclError axcl_set_device_ret = AXCL_SUCC;
#endif
    std::int32_t device_index = options.device_id < 0 ? 0 : options.device_id;
    std::int32_t resolved_device_id = 0;
    SystemState::DeviceState* device_state = nullptr;
    bool did_sys = false;
    bool did_vdec = false;
    bool did_venc = false;
    bool did_ivps = false;

#if defined(AXSDK_PLATFORM_AXCL)
    if (!state.axcl_initialized) {
        // AXCL official samples call axclInit(json_path). Some environments ship a default config under
        // /usr/bin/axcl/axcl.json. Keep nullptr as fallback for older installs.
        const char* config_path = std::getenv("AXCL_JSON");
        if (config_path == nullptr || config_path[0] == '\0') {
            config_path = std::getenv("AXCL_CONFIG");
        }
        if (config_path == nullptr || config_path[0] == '\0') {
            static constexpr const char* kDefaultAxclJson = "/usr/bin/axcl/axcl.json";
            if (std::ifstream(kDefaultAxclJson).good()) {
                config_path = kDefaultAxclJson;
            } else {
                config_path = nullptr;
            }
        }

        const auto ret = axclInit(config_path);
        if (ret != AXCL_SUCC) {
            std::cerr << "axclInit failed, ret=0x" << std::hex << ret << std::dec << "\n";
            return false;
        }
        state.axcl_initialized = true;
        did_axcl = true;
    }

    resolved_device_id = -1;
    if (!ResolveAxclDeviceIndex(device_index, &resolved_device_id)) {
        goto rollback;
    }
    if (state.active_device_id < 0) {
        state.active_device_id = device_index;
    }
    device_state = &state.devices[device_index];

    axcl_set_device_ret = axclrtSetDevice(resolved_device_id);
    if (axcl_set_device_ret != AXCL_SUCC) {
        std::cerr << "axclrtSetDevice failed, ret=0x" << std::hex << axcl_set_device_ret << std::dec
                  << " device=" << resolved_device_id << "\n";
        goto rollback;
    }
    did_device = true;

    if (!internal::EnsureAxclThreadContext(device_index)) {
        std::cerr << "EnsureAxclThreadContext failed\n";
        goto rollback;
    }
#else
    device_state = &state.devices[device_index];
#endif

    if (state.active_device_id < 0) {
        state.active_device_id = device_index;
    }

    if (!device_state->sys_initialized) {
        const auto ret =
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_SYS_Init();
#else
            AX_SYS_Init();
#endif
        if (ret != AX_SUCCESS) {
            std::cerr << "AX_SYS_Init failed, ret=0x" << std::hex << ret << std::dec << "\n";
            goto rollback;
        }
        device_state->sys_initialized = true;
        state.backend = backend;
        did_sys = true;
    }

    if (options.enable_vdec && !device_state->vdec_initialized) {
        const auto mod_attr = MakeVdecModAttr(options);
        const auto ret =
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_VDEC_Init(&mod_attr);
#else
            AX_VDEC_Init(&mod_attr);
#endif
        if (ret != AX_SUCCESS) {
            std::cerr << "AX_VDEC_Init failed, ret=0x" << std::hex << ret << std::dec << "\n";
            goto rollback;
        }
        device_state->vdec_initialized = true;
        did_vdec = true;
    }

    if (options.enable_venc && !device_state->venc_initialized) {
        const auto mod_attr = MakeVencModAttr(options);
        const auto ret =
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_VENC_Init(&mod_attr);
#else
            AX_VENC_Init(&mod_attr);
#endif
        if (ret != AX_SUCCESS) {
            std::cerr << "AX_VENC_Init failed, ret=0x" << std::hex << ret << std::dec << "\n";
            goto rollback;
        }
        device_state->venc_initialized = true;
        did_venc = true;
    }

    if (options.enable_ivps && !device_state->ivps_initialized) {
        const auto ret =
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_IVPS_Init();
#else
            AX_IVPS_Init();
#endif
        if (ret != AX_SUCCESS) {
            std::cerr << "AX_IVPS_Init failed, ret=0x" << std::hex << ret << std::dec << "\n";
            goto rollback;
        }
        device_state->ivps_initialized = true;
        did_ivps = true;
    }

    ++state.ref_count;
    ++device_state->ref_count;
    return true;

rollback:
    if (did_ivps) {
        (void)
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_IVPS_Deinit();
#else
            AX_IVPS_Deinit();
#endif
        if (device_state != nullptr) {
            device_state->ivps_initialized = false;
        }
    }
    if (did_venc) {
        (void)
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_VENC_Deinit();
#else
            AX_VENC_Deinit();
#endif
        if (device_state != nullptr) {
            device_state->venc_initialized = false;
        }
    }
    if (did_vdec) {
        (void)
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_VDEC_Deinit();
#else
            AX_VDEC_Deinit();
#endif
        if (device_state != nullptr) {
            device_state->vdec_initialized = false;
        }
    }
    if (did_sys) {
        (void)
#if defined(AXSDK_PLATFORM_AXCL)
            AXCL_SYS_Deinit();
#else
            AX_SYS_Deinit();
#endif
        if (device_state != nullptr) {
            device_state->sys_initialized = false;
        }
        state.backend = BackendType::kAuto;
    }
#if defined(AXSDK_PLATFORM_AXCL)
    if (did_device) {
        internal::ReleaseAxclThreadContext();
        (void)axclrtResetDevice(resolved_device_id);
        state.devices.erase(device_index);
    }
    if (did_axcl) {
        (void)axclFinalize();
        state.axcl_initialized = false;
    }
#endif
    return false;
}

void ShutdownSystem() noexcept {
    std::lock_guard<std::mutex> lock(GetSystemMutex());
    auto& state = GetSystemState();

    if (state.ref_count == 0) {
        return;
    }

    --state.ref_count;
    if (state.ref_count != 0) {
        return;
    }

#if defined(AXSDK_PLATFORM_AXCL)
    for (auto& [device_index, device_state] : state.devices) {
        if (!internal::EnsureAxclThreadContext(device_index)) {
            continue;
        }
        if (device_state.ivps_initialized) {
            (void)AXCL_IVPS_Deinit();
            device_state.ivps_initialized = false;
        }
        if (device_state.venc_initialized) {
            (void)AXCL_VENC_Deinit();
            device_state.venc_initialized = false;
        }
        if (device_state.vdec_initialized) {
            (void)AXCL_VDEC_Deinit();
            device_state.vdec_initialized = false;
        }
        if (device_state.sys_initialized) {
            (void)AXCL_SYS_Deinit();
            device_state.sys_initialized = false;
        }
    }
    internal::ReleaseAxclThreadContext();
    for (const auto& [device_index, device_state] : state.devices) {
        (void)device_state;
        std::int32_t resolved_device_id = -1;
        if (ResolveAxclDeviceIndex(device_index, &resolved_device_id)) {
            (void)axclrtResetDevice(resolved_device_id);
        }
    }
    state.devices.clear();
    state.active_device_id = -1;
    if (state.axcl_initialized) {
        (void)axclFinalize();
        state.axcl_initialized = false;
    }
#else
    for (auto& [device_index, device_state] : state.devices) {
        (void)device_index;
        if (device_state.ivps_initialized) {
            (void)AX_IVPS_Deinit();
            device_state.ivps_initialized = false;
        }
        if (device_state.venc_initialized) {
            (void)AX_VENC_Deinit();
            device_state.venc_initialized = false;
        }
        if (device_state.vdec_initialized) {
            (void)AX_VDEC_Deinit();
            device_state.vdec_initialized = false;
        }
        if (device_state.sys_initialized) {
            (void)AX_SYS_Deinit();
            device_state.sys_initialized = false;
        }
    }
    state.devices.clear();
    state.active_device_id = -1;
#endif
    state.backend = BackendType::kAuto;
}

bool IsSystemInitialized() noexcept {
    std::lock_guard<std::mutex> lock(GetSystemMutex());
    return GetSystemState().ref_count != 0;
}

BackendType GetActiveBackend() noexcept {
    std::lock_guard<std::mutex> lock(GetSystemMutex());
    return GetSystemState().backend;
}

std::int32_t GetActiveDeviceId() noexcept {
    std::lock_guard<std::mutex> lock(GetSystemMutex());
    return GetSystemState().active_device_id;
}

}  // namespace axvsdk::common

#if defined(AXSDK_PLATFORM_AXCL)
namespace axvsdk::common::internal {

bool EnsureAxclThreadContext(int device_id) noexcept {
    auto& state = GetSystemState();
    if (!state.axcl_initialized || state.devices.empty()) {
        return false;
    }

    auto& thread_context = GetThreadAxclContext();
    auto resolved_device_id =
        device_id >= 0 ? device_id
                       : (thread_context.device_id >= 0 ? thread_context.device_id : state.active_device_id);
    if (resolved_device_id < 0 || state.devices.find(resolved_device_id) == state.devices.end()) {
        return false;
    }

    std::int32_t runtime_device_id = -1;
    if (!ResolveAxclDeviceIndex(resolved_device_id, &runtime_device_id)) {
        return false;
    }

    if (thread_context.context != nullptr && thread_context.device_id == resolved_device_id) {
        // NOTE: axclrtGetCurrentContext logs an error when the thread has no current context.
        // This can happen if other modules temporarily bind/destroy their own contexts.
        // Always (re-)bind here to keep logs clean and avoid false alarms at shutdown.
        (void)axclrtSetCurrentContext(thread_context.context);
        return true;
    }

    if (thread_context.context != nullptr) {
        (void)axclrtDestroyContext(thread_context.context);
        thread_context.context = nullptr;
        thread_context.device_id = -1;
    }

    if (axclrtSetDevice(runtime_device_id) != AXCL_SUCC) {
        return false;
    }

    axclrtContext context = nullptr;
    const auto create_ret = axclrtCreateContext(&context, runtime_device_id);
    if (create_ret != AXCL_SUCC || context == nullptr) {
        return false;
    }

    const auto set_ret = axclrtSetCurrentContext(context);
    if (set_ret != AXCL_SUCC) {
        (void)axclrtDestroyContext(context);
        return false;
    }

    thread_context.context = context;
    thread_context.device_id = resolved_device_id;
    return true;
}

void ReleaseAxclThreadContext() noexcept {
    auto& thread_context = GetThreadAxclContext();
    if (thread_context.context != nullptr) {
        (void)axclrtDestroyContext(thread_context.context);
        thread_context.context = nullptr;
        thread_context.device_id = -1;
    }
}

}  // namespace axvsdk::common::internal
#endif
