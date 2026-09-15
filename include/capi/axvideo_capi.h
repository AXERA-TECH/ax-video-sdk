// axvideo_capi.h —— ax-video-sdk 的纯 C ABI 封装
//
// 目标:给语言绑定(pyaxvideo 的 ctypes 等)一个跨 Python/语言版本稳定的接口。
// 每个后端各编译一份动态库(libaxvideo_capi_axcl.so / libaxvideo_capi_ax650.so),
// C ABI 完全一致,由绑定层在运行时选择加载哪一个。
//
// 线程约定:所有接口线程安全;AXCL 下每个调用线程的 runtime context 由本库内部维护。
// 错误约定:失败返回 NULL / 非 0,细节用 axv_last_error() 取(thread-local)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define AXV_API __declspec(dllexport)
#else
#define AXV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 系统 ---------- */

// device_id:AXCL 下为设备索引(0 起);板端(MSP)忽略,传 0 即可。
AXV_API int axv_init(int device_id);
AXV_API void axv_deinit(void);
// "axcl" / "ax650"
AXV_API const char* axv_backend(void);
AXV_API const char* axv_version(void);
// 最近一次失败的描述,thread-local,永不返回 NULL。
AXV_API const char* axv_last_error(void);

/* ---------- 像素格式 ---------- */

enum {
    AXV_FMT_NV12 = 1,
    AXV_FMT_RGB24 = 2,
    AXV_FMT_BGR24 = 3,
};

/* ---------- 帧(设备侧图像) ---------- */

typedef struct axv_frame axv_frame_t;

typedef struct axv_frame_info {
    int32_t format;        /* AXV_FMT_* */
    uint32_t width;
    uint32_t height;
    uint32_t plane_count;
    size_t strides[3];     /* 每 plane 字节跨度 */
    size_t plane_sizes[3];
    size_t byte_size;      /* 各 plane 总字节数 */
    uint64_t phys_addr[3]; /* CMM/pool 物理地址,供 NPU 等硬件模块直连 */
    uint64_t virt_addr[3]; /* 库侧虚拟地址;AXCL 下为设备侧地址,host 不可直接读写 */
    int32_t device_id;
} axv_frame_info_t;

AXV_API int axv_frame_get_info(const axv_frame_t* f, axv_frame_info_t* out);
// 释放引用。来自 reader 的帧是解码缓冲池的引用,用完尽快释放,
// 长期持有请先 axv_frame_clone()。
AXV_API void axv_frame_release(axv_frame_t* f);
// 深拷贝到独立 CMM 内存,生命周期与缓冲池解耦。
AXV_API axv_frame_t* axv_frame_clone(const axv_frame_t* f);

// 设备帧 → host 内存(dst 至少 byte_size 字节,平面按顺序紧凑排列,保留 stride)。
AXV_API int axv_frame_to_host(const axv_frame_t* f, void* dst, size_t dst_size);
// host 内存 → 设备帧(data 按 fmt/width/height 紧凑排列;stride 传 0 = 无 padding)。
AXV_API axv_frame_t* axv_frame_from_host(int32_t fmt, uint32_t width, uint32_t height,
                                         const void* data, size_t size, size_t stride0);

/* ---------- 硬件图像处理(IVPS) ---------- */

enum {
    AXV_RESIZE_STRETCH = 0,
    AXV_RESIZE_KEEP_ASPECT = 1, /* 等比缩放,留边填 background(0xRRGGBB) */
};

// resize + 颜色空间转换,输出新的设备帧。
AXV_API axv_frame_t* axv_frame_convert(const axv_frame_t* f, int32_t dst_fmt,
                                       uint32_t dst_w, uint32_t dst_h,
                                       int resize_mode, uint32_t background);
// 硬件裁剪(可同时缩放/转格式:dst_w/dst_h/dst_fmt 传 0 = 保持裁剪区尺寸与原格式)。
AXV_API axv_frame_t* axv_frame_crop(const axv_frame_t* f,
                                    int32_t x, int32_t y, uint32_t w, uint32_t h,
                                    int32_t dst_fmt, uint32_t dst_w, uint32_t dst_h);

/* ---------- JPEG(硬件编解码) ---------- */

// 编码到 host 内存;返回缓冲用 axv_buffer_free 释放。
AXV_API int axv_frame_encode_jpeg(const axv_frame_t* f, uint32_t quality,
                                  uint8_t** out, size_t* out_size);
AXV_API void axv_buffer_free(uint8_t* buf);
// 解码 JPEG(host 内存)为设备帧,默认输出原尺寸 NV12。
AXV_API axv_frame_t* axv_decode_jpeg(const void* data, size_t size);

/* ---------- Reader:demux + 硬件解码(mp4 / mov / rtsp) ---------- */

typedef struct axv_reader axv_reader_t;

typedef struct axv_reader_opts {
    // 0 = 每帧模式(有序有界队列,慢消费丢最旧);1 = 只要最新帧
    int latest_only;
    // 文件输入:1 = 按源帧率节奏送(模拟实时源);0 = 全速解码(离线处理)
    int realtime;
    // 文件输入:1 = 循环播放
    int loop;
    // 每帧模式的队列深度,0 = 默认(8)
    int queue_depth;
} axv_reader_opts_t;

AXV_API axv_reader_t* axv_reader_open(const char* uri, const axv_reader_opts_t* opts);
// 取下一帧。返回 1=拿到帧;0=超时(timeout_ms 内无帧);-1=流结束(EOF);-2=错误。
AXV_API int axv_reader_next(axv_reader_t* r, axv_frame_t** out, int timeout_ms);
// 源视频信息(打开后即有效)。fps 可能为 0(未知)。
AXV_API int axv_reader_get_info(const axv_reader_t* r, int32_t* codec /*1=h264,2=h265*/,
                                uint32_t* width, uint32_t* height, double* fps);
AXV_API void axv_reader_close(axv_reader_t* r);

/* ---------- Writer:硬件编码 + 封装(mp4 文件 / rtsp) ---------- */

typedef struct axv_writer axv_writer_t;

typedef struct axv_writer_opts {
    int32_t codec;          /* 1=h264, 2=h265 */
    uint32_t width;
    uint32_t height;
    double fps;             /* 0 = 默认 30 */
    uint32_t bitrate_kbps;  /* 0 = 自动 */
    uint32_t gop;           /* 0 = 自动 */
} axv_writer_opts_t;

// uri:*.mp4 文件路径,或 rtsp://...(本机起 RTSP server 或向远端推流,语义同 app outputs)。
AXV_API axv_writer_t* axv_writer_open(const char* uri, const axv_writer_opts_t* opts);
// 输入帧尺寸/格式与 writer 不一致时内部自动 resize/CSC。
AXV_API int axv_writer_write(axv_writer_t* w, const axv_frame_t* f);
AXV_API void axv_writer_close(axv_writer_t* w);

#ifdef __cplusplus
}
#endif
