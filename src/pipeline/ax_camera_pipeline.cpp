#include "pipeline/ax_camera_pipeline.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "ax_v4l2_capture.h"
#include "common/ax_image.h"
#include "common/ax_image_processor.h"
#include "codec/ax_video_encoder.h"
#include "pipeline/ax_muxer.h"
#include "common/ax_drawer.h"
#include "common/ax_system.h"

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_rt_device.h"
#include "axcl_rt_memory.h"
#endif

namespace axvsdk::pipeline {

namespace {

class AxCameraPipeline final : public CameraPipeline {
public:
    ~AxCameraPipeline() override {
        Close();
    }

    bool Open(const CameraPipelineConfig& config) override {
        std::cerr << "camera_pipeline: Open() started\n";
        if (open_) {
            Close();
        }

        // 1. 打开 V4L2 摄像头
        std::cerr << "camera_pipeline: creating V4L2 capture\n";
        v4l2_capture_ = CreateV4l2Capture();
        if (!v4l2_capture_) {
            std::cerr << "camera_pipeline: failed to create V4L2 capture\n";
            return false;
        }

        V4l2CaptureConfig v4l2_config{};
        v4l2_config.device_path = config.device_path;
        v4l2_config.width = config.width;
        v4l2_config.height = config.height;
        v4l2_config.fps = static_cast<std::uint32_t>(config.fps);

        std::cerr << "camera_pipeline: opening V4L2 device " << config.device_path << "\n";
        if (!v4l2_capture_->Open(v4l2_config)) {
            std::cerr << "camera_pipeline: failed to open v4l2 capture\n";
            return false;
        }

        actual_width_ = v4l2_capture_->GetWidth();
        actual_height_ = v4l2_capture_->GetHeight();
        actual_fps_ = static_cast<double>(v4l2_capture_->GetFps());
        std::cerr << "camera_pipeline: V4L2 opened: " << actual_width_ << "x" << actual_height_ 
                  << " @ " << actual_fps_ << "fps\n";

        // 2. 创建编码器和 Muxer
        std::cerr << "camera_pipeline: creating encoders and muxers\n";
        if (!CreateEncodersAndMuxers(config)) {
            std::cerr << "camera_pipeline: failed to create encoders/muxers\n";
            Close();
            return false;
        }

        // 3. 创建绘图器
        std::cerr << "camera_pipeline: creating drawer\n";
        drawer_ = common::CreateDrawer();
        if (!drawer_) {
            std::cerr << "camera_pipeline: warning: failed to create drawer, OSD will not work\n";
        }

        config_ = config;
        open_ = true;
        std::cerr << "camera_pipeline: Open() succeeded\n";
        return true;
    }

    void Close() noexcept override {
        Stop();

        if (!open_) {
            return;
        }

        // 停止 V4L2 捕获
        if (v4l2_capture_) {
            v4l2_capture_->Close();
            v4l2_capture_.reset();
        }

        // 关闭编码器和 Muxer
        for (auto& branch : branches_) {
            if (branch.muxer) {
                branch.muxer->Close();
            }
            branch.encoder->Close();
        }
        branches_.clear();

        // 清理资源
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            pending_callback_frame_.reset();
            frame_callback_ = {};
        }
        {
            std::lock_guard<std::mutex> lock(osd_mutex_);
            pending_osd_.reset();
            active_osd_.reset();
            active_osd_remaining_frames_ = 0;
        }

        drawer_.reset();
        frame_processor_.reset();

        captured_frames_.store(0, std::memory_order_relaxed);
        submit_failures_.store(0, std::memory_order_relaxed);
        open_ = false;
    }

    bool Start() override {
        std::cerr << "camera_pipeline: Start() started\n";
        if (!open_ || running_) {
            std::cerr << "camera_pipeline: Start() failed: not open or already running\n";
            return false;
        }

        // 启动帧回调线程
        std::cerr << "camera_pipeline: starting callback thread\n";
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            pending_callback_frame_.reset();
        }
        callback_stop_ = false;
        frame_callback_thread_ = std::thread(&AxCameraPipeline::FrameCallbackLoop, this);

        // 启动所有编码器
        std::cerr << "camera_pipeline: starting encoders\n";
        for (auto& branch : branches_) {
            if (!branch.encoder->Start()) {
                std::cerr << "camera_pipeline: failed to start encoder\n";
                for (auto& started_branch : branches_) {
                    started_branch.encoder->Stop();
                }
                StopFrameCallbackThread();
                return false;
            }
        }

        // 设置 V4L2 帧回调
        std::cerr << "camera_pipeline: setting V4L2 frame callback\n";
        v4l2_capture_->SetFrameCallback(
            [this](common::AxImage::Ptr frame, std::uint64_t seq) {
                this->OnV4l2Frame(std::move(frame), seq);
            });

        // 启动 V4L2 捕获
        std::cerr << "camera_pipeline: starting V4L2 capture\n";
        if (!v4l2_capture_->Start()) {
            std::cerr << "camera_pipeline: failed to start V4L2 capture\n";
            for (auto& branch : branches_) {
                branch.encoder->Stop();
            }
            StopFrameCallbackThread();
            return false;
        }

        running_ = true;
        std::cerr << "camera_pipeline: Start() succeeded\n";
        return true;
    }

    void Stop() noexcept override {
        if (!running_) {
            return;
        }

        // 停止 V4L2 捕获
        if (v4l2_capture_) {
            v4l2_capture_->Stop();
            v4l2_capture_->SetFrameCallback(nullptr);
        }

        // 停止编码器
        for (auto& branch : branches_) {
            branch.encoder->Stop();
        }

        // 清理帧数据
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            pending_callback_frame_.reset();
        }

        StopFrameCallbackThread();
        running_ = false;
    }

    std::uint32_t GetWidth() const noexcept override { return actual_width_; }
    std::uint32_t GetHeight() const noexcept override { return actual_height_; }
    double GetFps() const noexcept override { return actual_fps_; }

    common::AxImage::Ptr GetLatestFrame() override {
        common::AxImage::Ptr source_frame;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            source_frame = latest_frame_;
        }
        if (!source_frame) {
            return nullptr;
        }

        // 如果有 frame_output 配置，进行转换
        const auto& output_cfg = config_.frame_output.output_image;
        if (output_cfg.format == common::PixelFormat::kUnknown &&
            output_cfg.width == 0 && output_cfg.height == 0) {
            return source_frame;  // 无需转换
        }

        return ConvertFrame(source_frame);
    }

    void SetFrameCallback(CameraFrameCallback callback) override {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        frame_callback_ = std::move(callback);
    }

    bool SetOsd(const common::DrawFrame& osd) override {
        if (!open_ || !drawer_) {
            return false;
        }

        const bool empty = osd.lines.empty() && osd.polygons.empty() && 
                          osd.rects.empty() && osd.mosaics.empty() && osd.bitmaps.empty();
        if (empty) {
            ClearOsd();
            return true;
        }

        auto prepared_osd = drawer_->Prepare(osd);
        if (!prepared_osd) {
            return false;
        }

        std::lock_guard<std::mutex> lock(osd_mutex_);
        pending_osd_ = std::move(prepared_osd);
        return true;
    }

    void ClearOsd() noexcept override {
        std::lock_guard<std::mutex> lock(osd_mutex_);
        pending_osd_.reset();
        active_osd_.reset();
        active_osd_remaining_frames_ = 0;
    }

    CameraPipelineStats GetStats() const override {
        CameraPipelineStats stats{};
        stats.captured_frames = captured_frames_.load(std::memory_order_relaxed);
        stats.submit_failures = submit_failures_.load(std::memory_order_relaxed);
        stats.output_stats.reserve(branches_.size());
        for (const auto& branch : branches_) {
            stats.output_stats.push_back(branch.encoder->GetStats());
        }
        return stats;
    }

private:
    struct OutputBranch {
        CameraPipelineOutputConfig config{};
        std::unique_ptr<codec::VideoEncoder> encoder;
        std::unique_ptr<Muxer> muxer;
    };

    bool CreateEncodersAndMuxers(const CameraPipelineConfig& config) {
        if (config.outputs.empty()) {
            std::cerr << "camera_pipeline: no outputs configured\n";
            return false;
        }

        branches_.reserve(config.outputs.size());
        
        for (const auto& output : config.outputs) {
            if (output.uris.empty() && !output.packet_callback) {
                std::cerr << "camera_pipeline: output has no uris or callback\n";
                return false;
            }

            // 创建编码器
            auto encoder = codec::CreateVideoEncoder();
            if (!encoder) {
                std::cerr << "camera_pipeline: failed to create encoder\n";
                return false;
            }

            codec::VideoEncoderConfig encoder_config{};
            encoder_config.codec = output.codec;
            encoder_config.width = output.width == 0 ? actual_width_ : output.width;
            encoder_config.height = output.height == 0 ? actual_height_ : output.height;
            encoder_config.device_id = config.device_id;
            encoder_config.frame_rate = output.frame_rate > 0.0 ? output.frame_rate : actual_fps_;
            encoder_config.bitrate_kbps = output.bitrate_kbps;
            encoder_config.gop = output.gop;
            if (output.input_queue_depth > 0) {
                encoder_config.input_queue_depth = output.input_queue_depth;
            }
            encoder_config.overflow_policy = output.overflow_policy;
            encoder_config.resize = output.resize;

            if (!encoder->Open(encoder_config)) {
                std::cerr << "camera_pipeline: failed to open encoder\n";
                return false;
            }

            // 创建 Muxer
            std::unique_ptr<Muxer> muxer;
            if (!output.uris.empty()) {
                muxer = CreateMuxer();
                if (!muxer) {
                    std::cerr << "camera_pipeline: failed to create muxer\n";
                    encoder->Close();
                    return false;
                }

                MuxerConfig muxer_config{};
                muxer_config.stream.codec = output.codec;
                muxer_config.stream.width = encoder_config.width;
                muxer_config.stream.height = encoder_config.height;
                muxer_config.stream.frame_rate = encoder_config.frame_rate;
                muxer_config.uris = output.uris;

                if (!muxer->Open(muxer_config)) {
                    std::cerr << "camera_pipeline: failed to open muxer\n";
                    encoder->Close();
                    return false;
                }
            }

            // 设置编码器回调
            auto* muxer_ptr = muxer.get();
            if (output.packet_callback || muxer_ptr != nullptr) {
                encoder->SetPacketCallback([packet_callback = output.packet_callback,
                                            muxer = muxer_ptr](codec::EncodedPacket packet) mutable {
                    if (packet_callback) {
                        packet_callback(packet);
                    }
                    if (muxer != nullptr) {
                        (void)muxer->SubmitPacket(std::move(packet));
                    }
                });
            }

            branches_.push_back(OutputBranch{output, std::move(encoder), std::move(muxer)});
        }

        // 创建绘图器
        drawer_ = common::CreateDrawer();
        
        return true;
    }

    void OnV4l2Frame(common::AxImage::Ptr frame, std::uint64_t seq) {
        static std::uint64_t frame_count = 0;
        if (!frame) {
            std::cerr << "camera_pipeline: OnV4l2Frame received null frame\n";
            return;
        }
        
        if (++frame_count <= 5 || frame_count % 30 == 0) {
            std::cerr << "camera_pipeline: OnV4l2Frame seq=" << seq 
                      << " count=" << frame_count 
                      << " frame=" << frame->width() << "x" << frame->height() << "\n";
        }

        captured_frames_.fetch_add(1, std::memory_order_relaxed);

        // 保存最新帧
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_frame_ = frame;
        }

        // 触发回调
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (frame_callback_) {
                pending_callback_frame_ = frame;
                frame_callback_cv_.notify_one();
            }
        }

        // 应用 OSD 并送入编码器
        common::AxImage::Ptr encoder_frame = frame;
        ApplyOsdIfNeeded(&encoder_frame);

        for (auto& branch : branches_) {
            if (branch.encoder && encoder_frame) {
                if (!branch.encoder->SubmitFrame(encoder_frame)) {
                    submit_failures_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    void ApplyOsdIfNeeded(common::AxImage::Ptr* frame) {
        if (!frame || !*frame) return;

        std::shared_ptr<const common::PreparedDrawCommands> osd_to_apply;
        {
            std::lock_guard<std::mutex> lock(osd_mutex_);
            if (pending_osd_) {
                active_osd_ = std::move(pending_osd_);
                active_osd_remaining_frames_ = active_osd_->hold_frames();
            }
            if (!active_osd_) {
                return;
            }
            osd_to_apply = active_osd_;
            if (active_osd_remaining_frames_ > 0) {
                --active_osd_remaining_frames_;
                if (active_osd_remaining_frames_ == 0) {
                    active_osd_.reset();
                }
            }
        }

        // 拷贝帧用于 OSD（避免修改原始帧）
        auto osd_frame = CopyFrame(**frame);
        if (!osd_frame) {
            return;
        }

        if (osd_to_apply->Apply(*osd_frame)) {
            *frame = std::move(osd_frame);
        }
    }

    common::AxImage::Ptr CopyFrame(const common::AxImage& source) {
        common::ImageAllocationOptions options{};
        options.memory_type = common::MemoryType::kCmm;
        options.cache_mode = common::CacheMode::kNonCached;

        auto dest = common::AxImage::Create(source.descriptor(), options);
        if (!dest) {
            return nullptr;
        }

        // 拷贝每个 plane
        for (std::size_t i = 0; i < source.plane_count(); ++i) {
            const auto* src_data = source.plane_data(i);
            auto* dst_data = dest->mutable_plane_data(i);
            if (!src_data || !dst_data) {
                return nullptr;
            }
            std::memcpy(dst_data, src_data, source.plane_size(i));
        }

        return dest;
    }

    common::AxImage::Ptr ConvertFrame(const common::AxImage::Ptr& source) {
        if (!source) return nullptr;

        const auto& output_cfg = config_.frame_output.output_image;
        
        // 检查是否需要转换
        if (source->format() == output_cfg.format &&
            source->width() == output_cfg.width &&
            source->height() == output_cfg.height) {
            return source;
        }

        // 使用 ImageProcessor 进行转换
        if (!frame_processor_) {
            frame_processor_ = common::CreateImageProcessor();
            if (!frame_processor_) {
                return nullptr;
            }
        }

        common::ImageAllocationOptions options{};
        options.memory_type = common::MemoryType::kCmm;
        options.cache_mode = common::CacheMode::kNonCached;

        auto dest = common::AxImage::Create(output_cfg, options);
        if (!dest) {
            return nullptr;
        }

        common::ImageProcessRequest request{};
        request.output_image = output_cfg;
        request.resize = config_.frame_output.resize;

        if (!frame_processor_->Process(*source, request, *dest)) {
            return nullptr;
        }

        return dest;
    }

    void FrameCallbackLoop() {
        while (true) {
            CameraFrameCallback callback;
            common::AxImage::Ptr frame;
            {
                std::unique_lock<std::mutex> lock(callback_mutex_);
                frame_callback_cv_.wait(lock, [this] {
                    return callback_stop_ || pending_callback_frame_ != nullptr;
                });

                if (callback_stop_ && pending_callback_frame_ == nullptr) {
                    return;
                }

                callback = frame_callback_;
                frame = std::move(pending_callback_frame_);
            }

            if (!callback || !frame) {
                continue;
            }

            // 转换帧格式（如果需要）
            auto callback_frame = ConvertFrame(frame);
            if (callback_frame) {
                callback(std::move(callback_frame));
            }
        }
    }

    void StopFrameCallbackThread() noexcept {
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            pending_callback_frame_.reset();
            callback_stop_ = true;
        }
        frame_callback_cv_.notify_all();
        if (frame_callback_thread_.joinable()) {
            frame_callback_thread_.join();
        }
    }

    CameraPipelineConfig config_{};
    std::unique_ptr<V4l2Capture> v4l2_capture_;
    std::vector<OutputBranch> branches_;
    
    std::uint32_t actual_width_{0};
    std::uint32_t actual_height_{0};
    double actual_fps_{30.0};

    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> captured_frames_{0};
    std::atomic<std::uint64_t> submit_failures_{0};

    // 最新帧
    mutable std::mutex frame_mutex_;
    common::AxImage::Ptr latest_frame_;

    // 回调
    std::mutex callback_mutex_;
    std::condition_variable frame_callback_cv_;
    CameraFrameCallback frame_callback_;
    common::AxImage::Ptr pending_callback_frame_;
    std::atomic<bool> callback_stop_{false};
    std::thread frame_callback_thread_;

    // OSD
    std::mutex osd_mutex_;
    std::unique_ptr<common::AxDrawer> drawer_;
    std::shared_ptr<const common::PreparedDrawCommands> pending_osd_;
    std::shared_ptr<const common::PreparedDrawCommands> active_osd_;
    std::uint32_t active_osd_remaining_frames_{0};

    // 图像处理
    std::unique_ptr<common::ImageProcessor> frame_processor_;
};

}  // namespace

std::unique_ptr<CameraPipeline> CreateCameraPipeline() {
    return std::make_unique<AxCameraPipeline>();
}

}  // namespace axvsdk::pipeline
