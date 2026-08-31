// BSP/AXCL 版本一致性检查:对比「编译所用 SDK 版本」与「运行环境版本」。
// 板端 BSP 固件与编译库不匹配会产生不可控错误(花屏/驱动 hang/静默出错),
// 这类问题应在启动第一步拦下来,而不是跑起来再排查。
#pragma once

#include <string>

namespace axvsdk::common {

enum class BspVersionStatus {
    kMatch,     // 主版本一致
    kMismatch,  // 主版本不一致
    kUnknown,   // 任一侧版本获取失败(不阻断)
};

struct BspVersionReport {
    BspVersionStatus status{BspVersionStatus::kUnknown};
    // true = 该平台上主版本不匹配应拒绝启动(on-chip MSP)。
    // AXCL host runtime 是版本化稳定接口,跨小版本混用受支持,恒为 false(仅告警)。
    bool enforce{false};
    std::string compiled;  // 编译期 SDK 版本,如 "V3.10.2_20251111020143"
    std::string runtime;   // 运行环境版本,如 "V3.10.2"
    std::string detail;    // 人类可读说明(不匹配细节 / 获取失败原因)
};

// on-chip(MSP): 运行侧读 /proc/ax_proc/version,失败回退扫描板上 libax_sys.so。
// AXCL: 运行侧 axclrtGetVersion()。
// 编译期版本由构建系统从 SDK 库中提取并以宏注入;提取失败时 status=kUnknown 放行。
BspVersionReport CheckBspVersion();

}  // namespace axvsdk::common
