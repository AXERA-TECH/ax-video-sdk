// axvideo_capi.cpp —— C ABI 封装实现(见 include/capi/axvideo_capi.h)
#include "capi/axvideo_capi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec/ax_jpeg_codec.h"
#include "codec/ax_video_decoder.h"
#include "codec/ax_video_encoder.h"
#include "common/ax_image.h"
#include "common/ax_image_processor.h"
#include "common/ax_system.h"
#include "pipeline/ax_demuxer.h"
#include "pipeline/ax_muxer.h"

#include "../common/ax_image_copy.h"
#include "../common/ax_system_internal.h"

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl.h"
#include "axcl_rt_memory.h"
#endif

namespace {

using axvsdk::codec::EncodedPacket;
using axvsdk::codec::VideoCodecType;
using axvsdk::common::AxImage;
using axvsdk::common::ImageDescriptor;
using axvsdk::common::PixelFormat;

thread_local std::string t_last_error;

void SetError(const std::string& msg) { t_last_error = msg; }

int g_device_id = 0;
std::atomic<bool> g_inited{false};

// AXCL 设备操作要求调用线程绑定 runtime context;绑定层的调用可能来自任意
// Python 线程。复用 SDK 内部实现(含设备索引→runtime id 的处理)。
bool EnsureThreadContext() {
#if defined(AXSDK_PLATFORM_AXCL)
    if (!axvsdk::common::internal::EnsureAxclThreadContext(g_device_id)) {
        SetError("bind axcl thread context failed (device " + std::to_string(g_device_id) + ")");
        return false;
    }
#endif
    return true;
}

// 图像处理器:全局懒创建 + 互斥(帧级操作耗时在 IVPS,锁开销可忽略)
std::mutex g_proc_mu;
std::unique_ptr<axvsdk::common::ImageProcessor> g_proc;
axvsdk::common::ImageProcessor* Proc() {
    if (!g_proc) g_proc = axvsdk::common::CreateImageProcessor();
    return g_proc.get();
}

PixelFormat ToPixelFormat(int32_t fmt) {
    switch (fmt) {
    case AXV_FMT_NV12: return PixelFormat::kNv12;
    case AXV_FMT_RGB24: return PixelFormat::kRgb24;
    case AXV_FMT_BGR24: return PixelFormat::kBgr24;
    default: return PixelFormat::kUnknown;
    }
}

int32_t FromPixelFormat(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::kNv12: return AXV_FMT_NV12;
    case PixelFormat::kRgb24: return AXV_FMT_RGB24;
    case PixelFormat::kBgr24: return AXV_FMT_BGR24;
    default: return 0;
    }
}

}  // namespace

struct axv_frame {
    AxImage::Ptr img;
};

namespace {

axv_frame_t* WrapFrame(AxImage::Ptr img) {
    if (!img) return nullptr;
    return new axv_frame{std::move(img)};
}

// host <-> device 逐 plane 拷贝。
// AXCL:CMM/pool 是设备内存,必须走 axclrtMemcpy;板端:CMM 有 CPU 映射,直接 memcpy。
bool CopyPlanes(const AxImage& img, void* host, size_t host_size, bool to_host) {
    size_t total = img.byte_size();
    if (host_size < total) {
        SetError("host buffer too small: need " + std::to_string(total));
        return false;
    }
    if (!EnsureThreadContext()) return false;
    auto* p = static_cast<uint8_t*>(host);
    for (size_t i = 0; i < img.plane_count(); ++i) {
        const size_t sz = img.plane_size(i);
#if defined(AXSDK_PLATFORM_AXCL)
        // AXCL 的 memcpy 以 CMM 物理地址作为 device 指针(SDK 内部同款用法)
        void* dev = reinterpret_cast<void*>(img.physical_address(i));
        const auto ret = to_host ? axclrtMemcpy(p, dev, sz, AXCL_MEMCPY_DEVICE_TO_HOST)
                                 : axclrtMemcpy(dev, p, sz, AXCL_MEMCPY_HOST_TO_DEVICE);
        if (ret != 0) {
            char ec[32];
            std::snprintf(ec, sizeof(ec), "0x%x", static_cast<unsigned>(ret));
            SetError(std::string("axclrtMemcpy failed: ") + ec);
            return false;
        }
#else
        if (to_host) {
            const_cast<AxImage&>(img).InvalidateCache();
            std::memcpy(p, img.plane_data(i), sz);
        } else {
            std::memcpy(const_cast<AxImage&>(img).mutable_plane_data(i), p, sz);
            const_cast<AxImage&>(img).FlushCache();
        }
#endif
        p += sz;
    }
    return true;
}

}  // namespace

/* ---------- 系统 ---------- */

extern "C" {

int axv_init(int device_id) {
    g_device_id = device_id;
    axvsdk::common::SystemOptions opt{};
    opt.device_id = device_id;
    if (!axvsdk::common::IsSystemInitialized() && !axvsdk::common::InitializeSystem(opt)) {
        SetError("InitializeSystem failed (device " + std::to_string(device_id) + ")");
        return -1;
    }
    if (!EnsureThreadContext()) return -1;
    g_inited = true;
    return 0;
}

void axv_deinit(void) {
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        g_proc.reset();
    }
    if (g_inited.exchange(false)) axvsdk::common::ShutdownSystem();
}

const char* axv_backend(void) {
#if defined(AXSDK_PLATFORM_AXCL)
    return "axcl";
#else
    return "ax650";
#endif
}

const char* axv_version(void) { return "0.1.1"; }

const char* axv_last_error(void) { return t_last_error.c_str(); }

/* ---------- 帧 ---------- */

int axv_frame_get_info(const axv_frame_t* f, axv_frame_info_t* out) {
    if (!f || !f->img || !out) { SetError("null frame/out"); return -1; }
    const auto& img = *f->img;
    std::memset(out, 0, sizeof(*out));
    out->format = FromPixelFormat(img.format());
    out->width = img.width();
    out->height = img.height();
    out->plane_count = static_cast<uint32_t>(img.plane_count());
    out->byte_size = img.byte_size();
    for (size_t i = 0; i < img.plane_count() && i < 3; ++i) {
        out->strides[i] = img.stride(i);
        out->plane_sizes[i] = img.plane_size(i);
        out->phys_addr[i] = img.physical_address(i);
        out->virt_addr[i] = reinterpret_cast<uint64_t>(img.virtual_address(i));
    }
    out->device_id = g_device_id;
    return 0;
}

void axv_frame_release(axv_frame_t* f) { delete f; }

axv_frame_t* axv_frame_clone(const axv_frame_t* f) {
    if (!f || !f->img) { SetError("null frame"); return nullptr; }
    if (!EnsureThreadContext()) return nullptr;
    axvsdk::common::ImageAllocationOptions alloc{};
    alloc.memory_type = axvsdk::common::MemoryType::kCmm;
    alloc.token = "axv-clone";
    auto dst = AxImage::Create(f->img->descriptor(), alloc);
    if (!dst) { SetError("alloc clone image failed"); return nullptr; }
    if (!axvsdk::common::internal::CopyImage(*f->img, dst.get())) {
        SetError("CopyImage failed");
        return nullptr;
    }
    return WrapFrame(std::move(dst));
}

int axv_frame_to_host(const axv_frame_t* f, void* dst, size_t dst_size) {
    if (!f || !f->img || !dst) { SetError("null frame/dst"); return -1; }
    return CopyPlanes(*f->img, dst, dst_size, /*to_host=*/true) ? 0 : -1;
}

axv_frame_t* axv_frame_from_host(int32_t fmt, uint32_t width, uint32_t height,
                                 const void* data, size_t size, size_t stride0) {
    const auto pf = ToPixelFormat(fmt);
    if (pf == PixelFormat::kUnknown || width == 0 || height == 0 || !data) {
        SetError("bad from_host args");
        return nullptr;
    }
    if (!EnsureThreadContext()) return nullptr;
    ImageDescriptor desc{};
    desc.format = pf;
    desc.width = width;
    desc.height = height;
    if (stride0) desc.strides[0] = stride0;
    axvsdk::common::ImageAllocationOptions alloc{};
    alloc.memory_type = axvsdk::common::MemoryType::kCmm;
    alloc.token = "axv-from-host";
    auto img = AxImage::Create(desc, alloc);
    if (!img) { SetError("alloc image failed"); return nullptr; }
    if (!CopyPlanes(*img, const_cast<void*>(data), size, /*to_host=*/false)) return nullptr;
    return WrapFrame(std::move(img));
}

/* ---------- 图像处理 ---------- */

axv_frame_t* axv_frame_convert(const axv_frame_t* f, int32_t dst_fmt,
                               uint32_t dst_w, uint32_t dst_h,
                               int resize_mode, uint32_t background) {
    if (!f || !f->img) { SetError("null frame"); return nullptr; }
    if (!EnsureThreadContext()) return nullptr;
    axvsdk::common::ImageProcessRequest req{};
    req.output_image.format = dst_fmt ? ToPixelFormat(dst_fmt) : f->img->format();
    req.output_image.width = dst_w ? dst_w : f->img->width();
    req.output_image.height = dst_h ? dst_h : f->img->height();
    req.resize.mode = (resize_mode == AXV_RESIZE_KEEP_ASPECT)
                          ? axvsdk::common::ResizeMode::kKeepAspectRatio
                          : axvsdk::common::ResizeMode::kStretch;
    req.resize.background_color = background;
    std::lock_guard<std::mutex> lk(g_proc_mu);
    auto* proc = Proc();
    if (!proc) { SetError("CreateImageProcessor failed"); return nullptr; }
    auto out = proc->Process(*f->img, req);
    if (!out) { SetError("image process failed"); return nullptr; }
    return WrapFrame(std::move(out));
}

axv_frame_t* axv_frame_crop(const axv_frame_t* f,
                            int32_t x, int32_t y, uint32_t w, uint32_t h,
                            int32_t dst_fmt, uint32_t dst_w, uint32_t dst_h) {
    if (!f || !f->img || w == 0 || h == 0) { SetError("bad crop args"); return nullptr; }
    if (!EnsureThreadContext()) return nullptr;
    axvsdk::common::ImageProcessRequest req{};
    req.enable_crop = true;
    req.crop = {x, y, w, h};
    req.output_image.format = dst_fmt ? ToPixelFormat(dst_fmt) : f->img->format();
    req.output_image.width = dst_w ? dst_w : w;
    req.output_image.height = dst_h ? dst_h : h;
    std::lock_guard<std::mutex> lk(g_proc_mu);
    auto* proc = Proc();
    if (!proc) { SetError("CreateImageProcessor failed"); return nullptr; }
    auto out = proc->Process(*f->img, req);
    if (!out) { SetError("crop failed"); return nullptr; }
    return WrapFrame(std::move(out));
}

/* ---------- JPEG ---------- */

int axv_frame_encode_jpeg(const axv_frame_t* f, uint32_t quality,
                          uint8_t** out, size_t* out_size) {
    if (!f || !f->img || !out || !out_size) { SetError("null args"); return -1; }
    if (!EnsureThreadContext()) return -1;
    auto jpg = axvsdk::codec::EncodeJpegToMemory(*f->img, {quality ? quality : 90U});
    if (jpg.empty()) { SetError("jpeg encode failed"); return -1; }
    auto* buf = static_cast<uint8_t*>(std::malloc(jpg.size()));
    if (!buf) { SetError("oom"); return -1; }
    std::memcpy(buf, jpg.data(), jpg.size());
    *out = buf;
    *out_size = jpg.size();
    return 0;
}

void axv_buffer_free(uint8_t* buf) { std::free(buf); }

axv_frame_t* axv_decode_jpeg(const void* data, size_t size) {
    if (!data || size == 0) { SetError("null jpeg data"); return nullptr; }
    if (!EnsureThreadContext()) return nullptr;
    auto img = axvsdk::codec::DecodeJpegMemory(data, size, {});
    if (!img) { SetError("jpeg decode failed"); return nullptr; }
    return WrapFrame(std::move(img));
}

}  // extern "C"

/* ---------- Reader ---------- */

struct axv_reader {
    std::unique_ptr<axvsdk::pipeline::Demuxer> demuxer;
    std::unique_ptr<axvsdk::codec::VideoDecoder> decoder;
    axvsdk::codec::VideoStreamInfo stream{};

    std::thread demux_thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> demux_done{false};

    std::mutex mu;
    std::condition_variable cv;
    std::deque<AxImage::Ptr> queue;
    size_t queue_cap{8};
    bool latest_only{false};

    ~axv_reader() {
        stop = true;
        if (demuxer) demuxer->Interrupt();
        if (demux_thread.joinable()) demux_thread.join();
        if (decoder) {
            decoder->Stop();
            decoder->Close();
        }
        if (demuxer) demuxer->Close();
    }
};

extern "C" {

axv_reader_t* axv_reader_open(const char* uri, const axv_reader_opts_t* opts) {
    if (!uri) { SetError("null uri"); return nullptr; }
    if (!EnsureThreadContext()) return nullptr;
    axv_reader_opts_t o{};
    if (opts) o = *opts;

    auto r = std::make_unique<axv_reader>();
    r->latest_only = o.latest_only != 0;
    if (o.queue_depth > 0) r->queue_cap = static_cast<size_t>(o.queue_depth);

    r->demuxer = axvsdk::pipeline::CreateDemuxer();
    axvsdk::pipeline::DemuxerConfig dc{};
    dc.uri = uri;
    dc.realtime_playback = o.realtime != 0;
    dc.loop_playback = o.loop != 0;
    if (!r->demuxer->Open(dc)) {
        SetError(std::string("demuxer open failed: ") + uri);
        return nullptr;
    }
    r->stream = r->demuxer->GetVideoStreamInfo();

    r->decoder = axvsdk::codec::CreateVideoDecoder();
    axvsdk::codec::VideoDecoderConfig vc{};
    vc.stream = r->stream;
    vc.device_id = g_device_id;
    if (!r->decoder->Open(vc) || !r->decoder->Start()) {
        SetError("decoder open/start failed");
        return nullptr;
    }

    auto* rp = r.get();
    r->decoder->SetFrameCallback(
        [rp](AxImage::Ptr frame) {
            if (!frame || rp->stop) return;
            std::lock_guard<std::mutex> lk(rp->mu);
            if (rp->latest_only) {
                rp->queue.clear();
            } else if (rp->queue.size() >= rp->queue_cap) {
                rp->queue.pop_front();  // 慢消费丢最旧,不阻塞解码
            }
            rp->queue.push_back(std::move(frame));
            rp->cv.notify_one();
        },
        rp->latest_only ? axvsdk::codec::FrameCallbackMode::kLatest
                        : axvsdk::codec::FrameCallbackMode::kQueue);

    r->demux_thread = std::thread([rp] {
        EnsureThreadContext();
        EncodedPacket pkt;
        while (!rp->stop && rp->demuxer->ReadPacket(&pkt)) {
            if (!rp->decoder->SubmitPacket(std::move(pkt))) break;
            pkt = {};
        }
        if (!rp->stop) rp->decoder->SubmitEndOfStream();
        rp->demux_done = true;
        rp->cv.notify_all();
    });

    return r.release();
}

int axv_reader_next(axv_reader_t* r, axv_frame_t** out, int timeout_ms) {
    if (!r || !out) { SetError("null reader/out"); return -2; }
    *out = nullptr;
    std::unique_lock<std::mutex> lk(r->mu);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        if (!r->queue.empty()) {
            *out = WrapFrame(std::move(r->queue.front()));
            r->queue.pop_front();
            return 1;
        }
        if (r->demux_done) {
            // demux 结束后给解码器 500ms 排空尾帧
            if (r->cv.wait_for(lk, std::chrono::milliseconds(500),
                               [r] { return !r->queue.empty(); })) {
                continue;
            }
            return -1;  // EOF
        }
        if (timeout_ms <= 0) {
            r->cv.wait(lk, [r] { return !r->queue.empty() || r->demux_done || r->stop; });
        } else if (r->cv.wait_until(lk, deadline, [r] {
                       return !r->queue.empty() || r->demux_done || r->stop;
                   })) {
            continue;
        } else {
            return 0;  // timeout
        }
        if (r->stop) return -2;
    }
}

int axv_reader_get_info(const axv_reader_t* r, int32_t* codec,
                        uint32_t* width, uint32_t* height, double* fps) {
    if (!r) { SetError("null reader"); return -1; }
    if (codec) *codec = r->stream.codec == VideoCodecType::kH265 ? 2 : 1;
    if (width) *width = r->stream.width;
    if (height) *height = r->stream.height;
    if (fps) *fps = r->stream.frame_rate;
    return 0;
}

void axv_reader_close(axv_reader_t* r) { delete r; }

}  // extern "C"

/* ---------- Writer ---------- */

struct axv_writer {
    std::unique_ptr<axvsdk::codec::VideoEncoder> encoder;
    std::unique_ptr<axvsdk::pipeline::Muxer> muxer;
    ~axv_writer() {
        if (encoder) {
            encoder->Stop();
            encoder->Close();
        }
        if (muxer) muxer->Close();
    }
};

extern "C" {

axv_writer_t* axv_writer_open(const char* uri, const axv_writer_opts_t* opts) {
    if (!uri || !opts || opts->width == 0 || opts->height == 0) {
        SetError("bad writer args");
        return nullptr;
    }
    if (!EnsureThreadContext()) return nullptr;
    auto w = std::make_unique<axv_writer>();

    axvsdk::codec::VideoEncoderConfig ec{};
    ec.codec = opts->codec == 2 ? VideoCodecType::kH265 : VideoCodecType::kH264;
    ec.width = opts->width;
    ec.height = opts->height;
    ec.device_id = g_device_id;
    ec.frame_rate = opts->fps > 0 ? opts->fps : 30.0;
    ec.bitrate_kbps = opts->bitrate_kbps;
    ec.gop = opts->gop;

    w->encoder = axvsdk::codec::CreateVideoEncoder();
    if (!w->encoder->Open(ec)) { SetError("encoder open failed"); return nullptr; }

    w->muxer = axvsdk::pipeline::CreateMuxer();
    axvsdk::pipeline::MuxerConfig mc{};
    mc.stream.codec = ec.codec;
    mc.stream.width = ec.width;
    mc.stream.height = ec.height;
    mc.stream.frame_rate = ec.frame_rate;
    mc.uris = {uri};
    if (!w->muxer->Open(mc)) { SetError(std::string("muxer open failed: ") + uri); return nullptr; }

    auto* mux = w->muxer.get();
    w->encoder->SetPacketCallback([mux](EncodedPacket pkt) {
        mux->SubmitPacket(std::move(pkt));
    });
    if (!w->encoder->Start()) { SetError("encoder start failed"); return nullptr; }
    return w.release();
}

int axv_writer_write(axv_writer_t* w, const axv_frame_t* f) {
    if (!w || !f || !f->img) { SetError("null writer/frame"); return -1; }
    if (!EnsureThreadContext()) return -1;
    if (!w->encoder->SubmitFrame(f->img)) { SetError("encoder submit failed"); return -1; }
    return 0;
}

void axv_writer_close(axv_writer_t* w) { delete w; }

}  // extern "C"
