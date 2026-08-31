#include "ax_v4l2_capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "common/ax_image.h"
#include "ax_image_copy.h"
#include "codec/ax_jpeg_codec.h"
#include "ax_system_internal.h"

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_rt_memory.h"
#endif

// 性能计时工具
struct PerfTimer {
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::microseconds;
    
    struct Stats {
        std::atomic<uint64_t> total_us{0};
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> max_us{0};
        
        void Add(uint64_t us) {
            total_us.fetch_add(us, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
            uint64_t prev_max = max_us.load(std::memory_order_relaxed);
            while (us > prev_max && !max_us.compare_exchange_weak(prev_max, us, std::memory_order_relaxed)) {}
        }
        
        void Print(const char* name) {
            uint64_t c = count.load(std::memory_order_relaxed);
            if (c == 0) return;
            uint64_t avg = total_us.load(std::memory_order_relaxed) / c;
            uint64_t mx = max_us.load(std::memory_order_relaxed);
            std::cerr << "[PERF] " << name << ": avg=" << avg << "us max=" << mx << "us count=" << c << "\n";
        }
    };
    
    static Stats& GetV4l2CaptureStats() {
        static Stats s;
        return s;
    }
    static Stats& GetConversionStats() {
        static Stats s;
        return s;
    }
    static Stats& GetCopyStats() {
        static Stats s;
        return s;
    }
    static Stats& GetTotalStats() {
        static Stats s;
        return s;
    }
    
    TimePoint start;
    Stats* stats;
    
    explicit PerfTimer(Stats& s) : start(Clock::now()), stats(&s) {}
    ~PerfTimer() {
        auto us = std::chrono::duration_cast<Duration>(Clock::now() - start).count();
        stats->Add(static_cast<uint64_t>(us));
    }
};

#define PERF_SCOPE(name) PerfTimer _perf_timer_##__LINE__(PerfTimer::Get##name##Stats())
#define PERF_PRINT(name) PerfTimer::Get##name##Stats().Print(#name)

namespace axvsdk::pipeline {

namespace {

// V4L2 缓冲区结构
struct V4l2Buffer {
    void* start{nullptr};
    size_t length{0};
};

// 转换 V4L2 格式到内部格式
V4l2PixelFormat V4l2FmtToInternal(__u32 v4l2_fmt) {
    switch (v4l2_fmt) {
        case V4L2_PIX_FMT_NV12:
            return V4l2PixelFormat::kNv12;
        case V4L2_PIX_FMT_YUYV:
            return V4l2PixelFormat::kYuyv;
        case V4L2_PIX_FMT_MJPEG:
            return V4l2PixelFormat::kMjpeg;
        default:
            return V4l2PixelFormat::kUnknown;
    }
}

// 转换内部格式到 V4L2 格式
__u32 InternalFmtToV4l2(V4l2PixelFormat fmt) {
    switch (fmt) {
        case V4l2PixelFormat::kNv12:
            return V4L2_PIX_FMT_NV12;
        case V4l2PixelFormat::kYuyv:
            return V4L2_PIX_FMT_YUYV;
        case V4l2PixelFormat::kMjpeg:
            return V4L2_PIX_FMT_MJPEG;
        default:
            return 0;
    }
}

// 转换 V4L2 格式到 AxImage 格式
common::PixelFormat V4l2FmtToAxImageFmt(V4l2PixelFormat fmt) {
    switch (fmt) {
        case V4l2PixelFormat::kNv12:
            return common::PixelFormat::kNv12;
        case V4l2PixelFormat::kYuyv:
            // YUYV 需要转换，这里暂时返回 Unknown，后续转换
            return common::PixelFormat::kUnknown;
        case V4l2PixelFormat::kMjpeg:
            // MJPEG 需要解码
            return common::PixelFormat::kUnknown;
        default:
            return common::PixelFormat::kUnknown;
    }
}

// YUYV 转 NV12 - 优化版本
void YuyvToNv12(const uint8_t* yuyv, uint8_t* nv12_y, uint8_t* nv12_uv,
                std::uint32_t width, std::uint32_t height) {
    const std::size_t yuyv_stride = width * 2;
    const std::size_t nv12_y_stride = width;
    const std::size_t nv12_uv_stride = width;
    
    // 提取 Y plane - 直接拷贝每对像素的第一个字节
    for (std::uint32_t row = 0; row < height; ++row) {
        const uint8_t* yuyv_row = yuyv + row * yuyv_stride;
        uint8_t* y_row = nv12_y + row * nv12_y_stride;
        
        std::uint32_t col = 0;
        // 每次处理 2 个像素
        for (; col + 1 < width; col += 2) {
            const std::size_t yuyv_idx = col * 2;
            y_row[col] = yuyv_row[yuyv_idx];
            y_row[col + 1] = yuyv_row[yuyv_idx + 2];
        }
        // 处理剩余像素
        if (col < width) {
            y_row[col] = yuyv_row[col * 2];
        }
    }
    
    // 提取 UV plane - 平均两行
    const std::uint32_t uv_height = height / 2;
    for (std::uint32_t row = 0; row < uv_height; ++row) {
        const uint8_t* yuyv_row0 = yuyv + (row * 2) * yuyv_stride;
        const uint8_t* yuyv_row1 = yuyv_row0 + yuyv_stride;
        uint8_t* dst_uv = nv12_uv + row * nv12_uv_stride;
        
        std::uint32_t col = 0;
        for (; col < width; col += 2) {
            const std::size_t yuyv_idx0 = col * 2;
            // U 在 YUYV 的第 1 个位置 (索引 1)，V 在第 3 个位置 (索引 3)
            dst_uv[col] = (yuyv_row0[yuyv_idx0 + 1] + yuyv_row1[yuyv_idx0 + 1]) >> 1;
            dst_uv[col + 1] = (yuyv_row0[yuyv_idx0 + 3] + yuyv_row1[yuyv_idx0 + 3]) >> 1;
        }
    }
}

class AxV4l2Capture final : public V4l2Capture {
public:
    ~AxV4l2Capture() override {
        Close();
    }

    bool Open(const V4l2CaptureConfig& config) override {
        if (fd_ >= 0) {
            Close();
        }

        // 打开设备
        fd_ = open(config.device_path.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) {
            std::cerr << "v4l2: failed to open " << config.device_path << "\n";
            return false;
        }

        // 查询设备能力
        struct v4l2_capability cap{};
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
            std::cerr << "v4l2: VIDIOC_QUERYCAP failed\n";
            Close();
            return false;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            std::cerr << "v4l2: device does not support video capture\n";
            Close();
            return false;
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::cerr << "v4l2: device does not support streaming\n";
            Close();
            return false;
        }

        // 设置格式
        if (!TrySetFormat(config)) {
            std::cerr << "v4l2: failed to set format\n";
            Close();
            return false;
        }

        // 初始化 MMAP 缓冲区
        if (!InitMmap()) {
            std::cerr << "v4l2: failed to init mmap\n";
            Close();
            return false;
        }

        config_ = config;
        return true;
    }

    void Close() noexcept override {
        Stop();
        
        // 释放缓冲区
        for (auto& buf : buffers_) {
            if (buf.start && buf.length > 0) {
                munmap(buf.start, buf.length);
            }
        }
        buffers_.clear();

        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }

        actual_format_ = V4l2PixelFormat::kUnknown;
    }

    bool Start() override {
        if (fd_ < 0 || running_) {
            return false;
        }

        // 入队所有缓冲区
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            struct v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                std::cerr << "v4l2: VIDIOC_QBUF failed\n";
                return false;
            }
        }

        // 开始流
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            std::cerr << "v4l2: VIDIOC_STREAMON failed\n";
            return false;
        }

        running_ = true;
        capture_thread_ = std::thread(&AxV4l2Capture::CaptureLoop, this);
        return true;
    }

    void Stop() noexcept override {
        running_ = false;
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }

        if (fd_ >= 0) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
    }

    std::uint32_t GetWidth() const noexcept override { return actual_width_; }
    std::uint32_t GetHeight() const noexcept override { return actual_height_; }
    std::uint32_t GetFps() const noexcept override { return actual_fps_; }
    V4l2PixelFormat GetFormat() const noexcept override { return actual_format_; }

    void SetFrameCallback(V4l2FrameCallback callback) override {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_ = std::move(callback);
    }

private:
    bool TrySetFormat(const V4l2CaptureConfig& config) {
        // 要尝试的格式优先级
        std::vector<V4l2PixelFormat> try_formats;
        if (!config.preferred_formats.empty()) {
            try_formats = config.preferred_formats;
        } else {
            // 默认优先级：NV12 > YUYV > MJPEG
            // NV12 无需转换，YUYV 有软件转换，MJPEG 需要硬件解码（AXCL 可能不支持）
            try_formats = {V4l2PixelFormat::kNv12, V4l2PixelFormat::kYuyv, V4l2PixelFormat::kMjpeg};
        }

        for (auto fmt : try_formats) {
            struct v4l2_format v4l2_fmt{};
            v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            v4l2_fmt.fmt.pix.width = config.width;
            v4l2_fmt.fmt.pix.height = config.height;
            v4l2_fmt.fmt.pix.pixelformat = InternalFmtToV4l2(fmt);
            v4l2_fmt.fmt.pix.field = V4L2_FIELD_ANY;

            if (ioctl(fd_, VIDIOC_S_FMT, &v4l2_fmt) < 0) {
                continue;
            }

            // 检查实际设置的格式
            if (v4l2_fmt.fmt.pix.pixelformat != InternalFmtToV4l2(fmt)) {
                continue;
            }

            actual_format_ = fmt;
            actual_width_ = v4l2_fmt.fmt.pix.width;
            actual_height_ = v4l2_fmt.fmt.pix.height;

            // 设置帧率
            struct v4l2_streamparm parm{};
            parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = config.fps;
            if (ioctl(fd_, VIDIOC_S_PARM, &parm) >= 0) {
                actual_fps_ = parm.parm.capture.timeperframe.denominator;
            } else {
                actual_fps_ = config.fps;
            }

            std::cout << "v4l2: format set to " << actual_width_ << "x" << actual_height_
                      << " fmt=" << static_cast<int>(actual_format_)
                      << " fps=" << actual_fps_ << "\n";
            return true;
        }

        return false;
    }

    bool InitMmap() {
        struct v4l2_requestbuffers req{};
        req.count = 4;  // 请求4个缓冲区
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            std::cerr << "v4l2: VIDIOC_REQBUFS failed\n";
            return false;
        }

        if (req.count < 2) {
            std::cerr << "v4l2: insufficient buffer memory\n";
            return false;
        }

        buffers_.resize(req.count);
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            struct v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                std::cerr << "v4l2: VIDIOC_QUERYBUF failed\n";
                return false;
            }

            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                std::cerr << "v4l2: mmap failed\n";
                return false;
            }
        }

        return true;
    }

    void CaptureLoop() {
        std::uint64_t seq = 0;
        std::cerr << "v4l2: CaptureLoop started\n";
        
        while (running_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd_, &fds);

            struct timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) continue;  // timeout

            struct v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                break;
            }

            // 检查缓冲区索引是否有效
            if (buf.index >= buffers_.size()) {
                std::cerr << "v4l2: invalid buffer index " << buf.index << ", max=" << buffers_.size() << "\n";
                continue;
            }

            // 处理帧（带计时）
            {
                PERF_SCOPE(V4l2Capture);
                ProcessFrame(buffers_[buf.index].start, buf.bytesused, ++seq);
            }

            // 重新入队
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                break;
            }
        }
    }

    void ProcessFrame(void* data, std::size_t size, std::uint64_t seq) {
        static std::uint64_t process_count = 0;
        static auto last_print = std::chrono::steady_clock::now();
        
        V4l2FrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = callback_;
        }
        if (!callback) {
            return;
        }

        // 每 3 秒打印一次性能统计
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_print).count() >= 3) {
            last_print = now;
            PERF_PRINT(V4l2Capture);
            PERF_PRINT(Conversion);
            PERF_PRINT(Copy);
            PERF_PRINT(Total);
            std::cerr << "---\n";
        }

        common::AxImage::Ptr image;
        {
            PERF_SCOPE(Total);
            image = ConvertToAxImage(data, size);
        }
        
        if (image) {
            callback(std::move(image), seq);
        } else {
            if (++process_count <= 10) {
                std::cerr << "v4l2: ConvertToAxImage failed\n";
            }
        }
    }

    common::AxImage::Ptr ConvertToAxImage(void* v4l2_data, std::size_t size) {
        if (actual_format_ == V4l2PixelFormat::kNv12) {
            // NV12 直接包装或拷贝
            return CreateNv12Image(v4l2_data, size);
        } else if (actual_format_ == V4l2PixelFormat::kYuyv) {
            // YUYV 转换为 NV12
            return ConvertYuyvToNv12(v4l2_data, size);
        } else if (actual_format_ == V4l2PixelFormat::kMjpeg) {
            // MJPEG 解码为 NV12
            return ConvertMjpegToNv12(v4l2_data, size);
        }
        std::cerr << "v4l2: Unsupported format for AxImage conversion\n";
        return nullptr;
    }

    common::AxImage::Ptr CreateNv12Image(void* v4l2_data, std::size_t size) {
        const std::size_t y_size = actual_width_ * actual_height_;
        const std::size_t uv_size = y_size / 2;
        const std::size_t expected_size = y_size + uv_size;

        if (size < expected_size) {
            return nullptr;
        }

        // 创建 AxImage
        common::ImageAllocationOptions options{};
        options.memory_type = common::MemoryType::kCmm;
        options.cache_mode = common::CacheMode::kNonCached;
        
        auto image = common::AxImage::Create(common::PixelFormat::kNv12, 
                                              actual_width_, actual_height_, options);
        if (!image) {
            return nullptr;
        }

        // 拷贝数据
        std::uint8_t* dst_y = image->mutable_plane_data(0);
        std::uint8_t* dst_uv = image->mutable_plane_data(1);
        
        if (!dst_y || !dst_uv) {
            return nullptr;
        }

        const std::uint8_t* src = static_cast<const std::uint8_t*>(v4l2_data);
        std::memcpy(dst_y, src, y_size);
        std::memcpy(dst_uv, src + y_size, uv_size);

        return image;
    }

    common::AxImage::Ptr ConvertYuyvToNv12(void* v4l2_data, std::size_t size) {
        const std::size_t expected_yuyv_size = actual_width_ * actual_height_ * 2;
        if (size < expected_yuyv_size) {
            return nullptr;
        }

        const std::size_t y_size = actual_width_ * actual_height_;
        const std::size_t uv_size = y_size / 2;
        const std::size_t nv12_size = y_size + uv_size;
        
        // 使用预分配的缓冲区避免重复分配
        if (conversion_buffer_.size() < nv12_size) {
            conversion_buffer_.resize(nv12_size);
        }
        
        uint8_t* host_y = conversion_buffer_.data();
        uint8_t* host_uv = conversion_buffer_.data() + y_size;
        
        // 优化的 YUYV->NV12 转换
        {
            PERF_SCOPE(Conversion);
            YuyvToNv12(static_cast<const uint8_t*>(v4l2_data), host_y, host_uv, 
                       actual_width_, actual_height_);
        }
        
        // 创建 device 内存的 AxImage
        common::ImageAllocationOptions options{};
        options.memory_type = common::MemoryType::kCmm;
        options.cache_mode = common::CacheMode::kNonCached;
        
        auto device_image = common::AxImage::Create(common::PixelFormat::kNv12, 
                                                     actual_width_, actual_height_, options);
        if (!device_image) {
            return nullptr;
        }

        // 批量内存拷贝（使用 AXCL Runtime 批量 API）
        {
            PERF_SCOPE(Copy);
            
#if defined(AXSDK_PLATFORM_AXCL)
            // AXCL: 使用 runtime 批量拷贝，避免逐行和设备同步
            if (common::internal::EnsureAxclThreadContext()) {
                void* dst_y = reinterpret_cast<void*>(device_image->physical_address(0));
                void* dst_uv = reinterpret_cast<void*>(device_image->physical_address(1));
                
                if (dst_y && dst_uv) {
                    bool copy_ok = true;
                    // 批量拷贝 Y plane
                    if (axclrtMemcpy(dst_y, host_y, y_size, AXCL_MEMCPY_HOST_TO_DEVICE) != AXCL_SUCC) {
                        copy_ok = false;
                    }
                    // 批量拷贝 UV plane
                    if (copy_ok && axclrtMemcpy(dst_uv, host_uv, uv_size, AXCL_MEMCPY_HOST_TO_DEVICE) != AXCL_SUCC) {
                        copy_ok = false;
                    }
                    if (copy_ok) {
                        return device_image;
                    }
                }
            }
#else
            // 非 AXCL 平台：直接内存拷贝
            std::uint8_t* dst_y = device_image->mutable_plane_data(0);
            std::uint8_t* dst_uv = device_image->mutable_plane_data(1);
            
            if (dst_y && dst_uv) {
                std::memcpy(dst_y, host_y, y_size);
                std::memcpy(dst_uv, host_uv, uv_size);
                device_image->FlushCache();
                return device_image;
            }
#endif
        }
        
        // 回退到 CopyImage
        {
            common::ImageDescriptor host_desc{};
            host_desc.format = common::PixelFormat::kNv12;
            host_desc.width = actual_width_;
            host_desc.height = actual_height_;
            host_desc.strides[0] = actual_width_;
            host_desc.strides[1] = actual_width_;
            
            std::array<common::ExternalImagePlane, common::kMaxImagePlanes> host_planes{};
            host_planes[0].virtual_address = host_y;
            host_planes[1].virtual_address = host_uv;
            
            auto host_image = common::AxImage::WrapExternal(host_desc, host_planes, {});
            if (host_image && common::internal::CopyImage(*host_image, device_image.get())) {
                return device_image;
            }
        }
        
        return nullptr;
    }

    common::AxImage::Ptr ConvertMjpegToNv12(void* v4l2_data, std::size_t size) {
        if (!v4l2_data || size == 0) {
            std::cerr << "v4l2: MJPEG data is null or empty\n";
            return nullptr;
        }

        // 使用 SDK 的 JPEG 解码器
        codec::JpegDecodeOptions options{};
        // 输出为 NV12，保持原始尺寸
        options.output_image.format = common::PixelFormat::kNv12;
        options.output_image.width = actual_width_;
        options.output_image.height = actual_height_;

        auto image = codec::DecodeJpegMemory(v4l2_data, size, options);
        if (!image) {
            std::cerr << "v4l2: Failed to decode MJPEG frame\n";
            return nullptr;
        }

        return image;
    }

    int fd_{-1};
    V4l2CaptureConfig config_{};
    std::vector<V4l2Buffer> buffers_;
    
    std::uint32_t actual_width_{0};
    std::uint32_t actual_height_{0};
    std::uint32_t actual_fps_{30};
    V4l2PixelFormat actual_format_{V4l2PixelFormat::kUnknown};
    
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
    
    std::mutex callback_mutex_;
    V4l2FrameCallback callback_;
    
    // 预分配的转换缓冲区，避免每帧重复分配
    std::vector<uint8_t> conversion_buffer_;
};

}  // namespace

std::unique_ptr<V4l2Capture> CreateV4l2Capture() {
    return std::make_unique<AxV4l2Capture>();
}

}  // namespace axvsdk::pipeline
