#pragma once

#include <cstdlib>
#include <mutex>

namespace axvsdk::common::internal {

// AX_IVPS VPP/Draw APIs are not consistently thread-safe across MSP/driver versions.
// For maximum throughput, we do NOT serialize IVPS operations by default.
// If you hit instability on a specific MSP/driver version, enable serialization via env:
//   AXVSDK_IVPS_SERIALIZE=1  (or AXP_IVPS_SERIALIZE=1)
inline bool IvpsSerializeEnabled() noexcept {
    static const bool enabled = [] {
        const char* v = std::getenv("AXVSDK_IVPS_SERIALIZE");
        if (v == nullptr || *v == '\0') {
            v = std::getenv("AXP_IVPS_SERIALIZE");
        }
        if (v == nullptr || *v == '\0') {
            return false;
        }
        // "1"/"true"/"yes" -> enable. Everything else -> disable.
        if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y') {
            return true;
        }
        return false;
    }();
    return enabled;
}

inline std::mutex& IvpsGlobalMutex() noexcept {
    static std::mutex mu;
    return mu;
}

}  // namespace axvsdk::common::internal
