#include "common/ax_version_check.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_rt.h"
#endif

namespace axvsdk::common {
namespace {

// "V3.10.2_20251111020143" -> "V3.10.2"。新固件 /proc 里只有主段,老格式带
// "_P1_时间戳" 尾巴,统一取下划线前的主段参与比较。
std::string MajorOf(const std::string& v) {
    const auto us = v.find('_');
    return us == std::string::npos ? v : v.substr(0, us);
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    return s.substr(b);
}

#if !defined(AXSDK_PLATFORM_AXCL)
// "Ax_Version V3.10.2" -> "V3.10.2"
std::string ReadProcVersion() {
    std::ifstream f("/proc/ax_proc/version");
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    const auto sp = line.find(' ');
    return Trim(sp == std::string::npos ? line : line.substr(sp + 1));
}

// 板上一般没有 strings 命令,自己在 libax_sys.so 里搜版本串:
// "[Axera version]: libax_sys.so V3.10.2_20251111020143 Nov 11 ..."
std::string ScanLibaxSysVersion() {
    static const char* kPaths[] = {"/soc/lib/libax_sys.so", "/opt/lib/libax_sys.so"};
    static const std::string kTag = "[Axera version]: libax_sys.so ";
    for (const char* path : kPaths) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string blob = ss.str();
        const auto pos = blob.find(kTag);
        if (pos == std::string::npos) continue;
        std::string out;
        for (std::size_t i = pos + kTag.size(); i < blob.size(); ++i) {
            const char c = blob[i];
            if (c == ' ' || c == '\0' || c == '\n') break;
            out.push_back(c);
        }
        if (!out.empty()) return out;
    }
    return {};
}
#endif

}  // namespace

BspVersionReport CheckBspVersion() {
    BspVersionReport rep;
#ifdef AXVSDK_BSP_COMPILED_VERSION
    rep.compiled = AXVSDK_BSP_COMPILED_VERSION;
#endif

#if defined(AXSDK_PLATFORM_AXCL)
    // AXCL host runtime 是版本化稳定接口,跨小版本混用受支持 -> 只告警不拦截
    rep.enforce = false;
    std::int32_t major = 0, minor = 0, patch = 0;
    if (axclrtGetVersion(&major, &minor, &patch) == 0) {
        // 部分 AXCL runtime 没有填真实版本,axclrtGetVersion 返回占位的 1.0.0——
        // 与编译版本比较无意义,当作"拿不到"处理(实测见于 aarch64 3.10.x 环境)
        if (!(major == 1 && minor == 0 && patch == 0)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "V%d.%d.%d", major, minor, patch);
            rep.runtime = buf;
        }
    }
#else
    rep.enforce = true;
    rep.runtime = ReadProcVersion();
    if (rep.runtime.empty()) rep.runtime = ScanLibaxSysVersion();
#endif

    if (rep.compiled.empty() || rep.runtime.empty()) {
        rep.status = BspVersionStatus::kUnknown;
        rep.detail = rep.compiled.empty() ? "compiled version unavailable"
                                          : "runtime version unavailable";
        rep.enforce = false;
        return rep;
    }
    if (MajorOf(rep.compiled) == MajorOf(rep.runtime)) {
        rep.status = BspVersionStatus::kMatch;
    } else {
        rep.status = BspVersionStatus::kMismatch;
        rep.detail = "compiled " + MajorOf(rep.compiled) + " vs runtime " + MajorOf(rep.runtime);
    }
    return rep;
}

}  // namespace axvsdk::common
