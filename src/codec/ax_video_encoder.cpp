#include "ax_video_encoder_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "common/ax_image_processor.h"
#include "common/ax_system.h"

namespace axvsdk::codec {

std::unique_ptr<VideoEncoder> CreateVideoEncoder() {
    return internal::CreatePlatformVideoEncoder();
}

}  // namespace axvsdk::codec

namespace axvsdk::codec::internal {

namespace {

constexpr int kEncoderStopIdlePollLimit = 10;
constexpr std::size_t kEncoderInputFifoDepth = 4;

std::uint32_t EstimateBitrateKbps(VideoCodecType codec,
                                  std::uint32_t width,
                                  std::uint32_t height,
                                  double frame_rate) noexcept {
    if (width == 0 || height == 0) {
        return 2048;
    }

    const double effective_fps = frame_rate > 0.0 ? frame_rate : 30.0;
    const double bits_per_pixel = codec == VideoCodecType::kH265 ? 0.045 : 0.070;
    const double bits_per_second =
        static_cast<double>(width) * static_cast<double>(height) * effective_fps * bits_per_pixel;
    const auto estimated_kbps = static_cast<std::uint32_t>(bits_per_second / 1000.0);
    return std::max<std::uint32_t>(256, estimated_kbps);
}

ResolvedVideoEncoderConfig ResolveConfig(const VideoEncoderConfig& config) noexcept {
    ResolvedVideoEncoderConfig resolved{};
    resolved.codec = config.codec;
    resolved.width = config.width;
    resolved.height = config.height;
    resolved.max_width = config.width;
    resolved.max_height = config.height;
    resolved.src_frame_rate = config.frame_rate > 0.0 ? config.frame_rate : 30.0;
    resolved.dst_frame_rate = resolved.src_frame_rate;
    resolved.bitrate_kbps = config.bitrate_kbps > 0
                                ? config.bitrate_kbps
                                : EstimateBitrateKbps(config.codec, config.width, config.height, resolved.dst_frame_rate);
    resolved.gop = config.gop > 0 ? config.gop
                                  : static_cast<std::uint32_t>(std::max(1.0, resolved.dst_frame_rate));
    resolved.input_queue_depth = config.input_queue_depth > 0 ? config.input_queue_depth : 10;
    resolved.overflow_policy = config.overflow_policy;
    resolved.stream_buffer_size = std::max<std::size_t>(
        static_cast<std::size_t>(config.width) * static_cast<std::size_t>(config.height), 1024U * 1024U);
    return resolved;
}

}  // namespace

AxVideoEncoderBase::~AxVideoEncoderBase() = default;

bool AxVideoEncoderBase::Open(const VideoEncoderConfig& config) {
    if (open_) {
        Close();
    }

    if (!common::IsSystemInitialized()) {
        return false;
    }

    if ((config.codec != VideoCodecType::kH264 && config.codec != VideoCodecType::kH265) || config.width == 0 ||
        config.height == 0) {
        return false;
    }

    config_ = ResolveConfig(config);
    if (!CreateBackend(config_)) {
        return false;
    }

    open_ = true;
    return true;
}

void AxVideoEncoderBase::Close() noexcept {
    Stop();
    if (open_) {
        DestroyBackend();
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            pending_frames_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(staging_mutex_);
            reusable_frames_.clear();
            inflight_frames_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(packet_mutex_);
            latest_packet_ = {};
            has_latest_packet_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            packet_callback_ = {};
        }
        config_ = {};
        submitted_frames_.store(0, std::memory_order_relaxed);
        dropped_frames_.store(0, std::memory_order_relaxed);
        encoded_packets_.store(0, std::memory_order_relaxed);
        key_packets_.store(0, std::memory_order_relaxed);
        open_ = false;
    }
}

bool AxVideoEncoderBase::Start() {
    if (!open_ || running_) {
        return false;
    }

    stop_requested_ = false;
    if (!StartBackend()) {
        return false;
    }

    send_thread_ = std::thread(&AxVideoEncoderBase::SendLoop, this);
    stream_thread_ = std::thread(&AxVideoEncoderBase::StreamLoop, this);
    running_ = true;
    return true;
}

void AxVideoEncoderBase::Stop() noexcept {
    if (!running_) {
        return;
    }

    stop_requested_ = true;
    input_cv_.notify_all();

    if (send_thread_.joinable()) {
        send_thread_.join();
    }

    StopBackend();

    if (stream_thread_.joinable()) {
        stream_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_frames_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        reusable_frames_.clear();
        inflight_frames_.clear();
    }
    input_cv_.notify_all();

    running_ = false;
}

bool AxVideoEncoderBase::SubmitFrame(common::AxImage::Ptr frame) {
    if (!running_ || !frame || !ValidateInputFrame(*frame)) {
        return false;
    }

    std::unique_lock<std::mutex> lock(input_mutex_);
    while (pending_frames_.size() >= config_.input_queue_depth) {
        if (config_.overflow_policy == QueueOverflowPolicy::kDropNewest) {
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (config_.overflow_policy == QueueOverflowPolicy::kBlock) {
            input_cv_.wait(lock, [this] {
                return stop_requested_ || pending_frames_.size() < config_.input_queue_depth;
            });
            if (stop_requested_) {
                return false;
            }
            continue;
        }

        pending_frames_.pop_front();
        dropped_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    pending_frames_.push_back(std::move(frame));
    submitted_frames_.fetch_add(1, std::memory_order_relaxed);
    input_cv_.notify_one();
    return true;
}

bool AxVideoEncoderBase::GetLatestPacket(EncodedPacket* packet) {
    if (packet == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(packet_mutex_);
    if (!has_latest_packet_) {
        return false;
    }

    *packet = latest_packet_;
    return true;
}

VideoEncoderStats AxVideoEncoderBase::GetStats() const {
    VideoEncoderStats stats{};
    stats.submitted_frames = submitted_frames_.load(std::memory_order_relaxed);
    stats.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    stats.encoded_packets = encoded_packets_.load(std::memory_order_relaxed);
    stats.key_packets = key_packets_.load(std::memory_order_relaxed);
    stats.queue_capacity = config_.input_queue_depth;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        stats.current_queue_depth = pending_frames_.size();
    }
    return stats;
}

void AxVideoEncoderBase::SetPacketCallback(PacketCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    packet_callback_ = std::move(callback);
}

const ResolvedVideoEncoderConfig& AxVideoEncoderBase::config() const noexcept {
    return config_;
}

bool AxVideoEncoderBase::ValidateInputFrame(const common::AxImage& frame) const noexcept {
    return frame.width() != 0 && frame.height() != 0;
}

common::AxImage::Ptr AxVideoEncoderBase::AcquireReusableFrame(const common::ImageDescriptor& descriptor,
                                                              const char* token) {
    std::lock_guard<std::mutex> lock(staging_mutex_);
    for (auto it = reusable_frames_.begin(); it != reusable_frames_.end(); ++it) {
        const auto& candidate = *it;
        if (!candidate) {
            continue;
        }
        if (candidate->descriptor().format == descriptor.format && candidate->width() == descriptor.width &&
            candidate->height() == descriptor.height && candidate->stride(0) == descriptor.strides[0] &&
            candidate->stride(1) == descriptor.strides[1]) {
            auto frame = std::move(*it);
            reusable_frames_.erase(it);
            return frame;
        }
    }

    common::ImageAllocationOptions alloc{};
    alloc.memory_type = common::MemoryType::kCmm;
    alloc.cache_mode = common::CacheMode::kNonCached;
    alloc.alignment = 0x1000;
    alloc.token = token;
    return common::AxImage::Create(descriptor, alloc);
}

void AxVideoEncoderBase::RecyclePreparedFrame(common::AxImage::Ptr frame) {
    if (!frame) {
        return;
    }
    std::lock_guard<std::mutex> lock(staging_mutex_);
    reusable_frames_.push_back(std::move(frame));
}

void AxVideoEncoderBase::ReleaseOldInflightFrame() {
    std::lock_guard<std::mutex> lock(staging_mutex_);
    if (inflight_frames_.size() <= kEncoderInputFifoDepth) {
        return;
    }
    reusable_frames_.push_back(std::move(inflight_frames_.front()));
    inflight_frames_.pop_front();
}

PreparedInputFrame AxVideoEncoderBase::PrepareInputFrame(const common::AxImage& frame) {
    const auto target_stride = static_cast<std::size_t>(config_.width);
    const bool already_hw_ready =
        frame.format() == common::PixelFormat::kNv12 &&
        frame.width() == config_.width &&
        frame.height() == config_.height &&
        frame.physical_address(0) != 0 &&
        frame.virtual_address(0) != nullptr &&
        frame.stride(0) >= target_stride &&
        frame.stride(1) >= target_stride;
    if (already_hw_ready) {
        return {common::AxImage::Ptr(const_cast<common::AxImage*>(&frame), [](common::AxImage*) {}), false};
    }

    common::ImageDescriptor target{};
    target.format = common::PixelFormat::kNv12;
    target.width = config_.width;
    target.height = config_.height;
    target.strides[0] = target_stride;
    target.strides[1] = target_stride;

    const bool needs_processing = frame.format() != target.format || frame.width() != target.width ||
                                  frame.height() != target.height || frame.physical_address(0) == 0;
    if (!needs_processing) {
        auto output = AcquireReusableFrame(target, "VideoEncoderInput");
        if (!output || frame.virtual_address(0) == nullptr || frame.virtual_address(1) == nullptr) {
            return {};
        }
        std::memcpy(output->virtual_address(0), frame.virtual_address(0), std::min(output->plane_size(0), frame.plane_size(0)));
        std::memcpy(output->virtual_address(1), frame.virtual_address(1), std::min(output->plane_size(1), frame.plane_size(1)));
        return {std::move(output), true};
    }

    auto processor = common::CreateImageProcessor();
    if (!processor) {
        return {};
    }

    common::AxImage::Ptr source = common::AxImage::Ptr(const_cast<common::AxImage*>(&frame), [](common::AxImage*) {});
    bool source_reusable = false;
    if (frame.physical_address(0) == 0) {
        source = AcquireReusableFrame(frame.descriptor(), "VideoEncoderSource");
        if (!source || frame.virtual_address(0) == nullptr) {
            return {};
        }
        for (std::size_t plane = 0; plane < frame.plane_count(); ++plane) {
            if (source->virtual_address(plane) == nullptr || frame.virtual_address(plane) == nullptr) {
                return {};
            }
            std::memcpy(source->virtual_address(plane), frame.virtual_address(plane),
                        std::min(source->plane_size(plane), frame.plane_size(plane)));
        }
        (void)source->FlushCache();
        source_reusable = true;
    } else {
        (void)source->FlushCache();
    }

    common::ImageProcessRequest request{};
    request.output_image = target;
    auto output = AcquireReusableFrame(target, "VideoEncoderInput");
    if (!output || !processor->Process(*source, request, *output)) {
        if (source_reusable) {
            RecyclePreparedFrame(std::move(source));
        }
        return {};
    }
    if (source_reusable) {
        RecyclePreparedFrame(std::move(source));
    }
    return {std::move(output), true};
}

void AxVideoEncoderBase::SendLoop() {
    while (true) {
        common::AxImage::Ptr frame;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [this] { return stop_requested_ || !pending_frames_.empty(); });
            if (stop_requested_ && pending_frames_.empty()) {
                return;
            }

            frame = std::move(pending_frames_.front());
            pending_frames_.pop_front();
            input_cv_.notify_all();
        }

        if (!frame || !ValidateInputFrame(*frame)) {
            continue;
        }

        auto prepared = PrepareInputFrame(*frame);
        if (!prepared.frame) {
            continue;
        }

        (void)prepared.frame->FlushCache();
        if (SendFrameToEncoder(*prepared.frame)) {
            if (prepared.reusable) {
                {
                    std::lock_guard<std::mutex> lock(staging_mutex_);
                    inflight_frames_.push_back(std::move(prepared.frame));
                }
                ReleaseOldInflightFrame();
            }
        } else if (prepared.reusable) {
            RecyclePreparedFrame(std::move(prepared.frame));
        }
    }
}

void AxVideoEncoderBase::StreamLoop() {
    int idle_polls_after_stop = 0;
    while (true) {
        EncodedPacket packet;
        bool flow_end = false;
        if (!ReceivePacketFromEncoder(&packet, &flow_end)) {
            if (flow_end) {
                return;
            }
            if (stop_requested_ && ++idle_polls_after_stop >= kEncoderStopIdlePollLimit) {
                return;
            }
            continue;
        }

        idle_polls_after_stop = 0;
        encoded_packets_.fetch_add(1, std::memory_order_relaxed);
        if (packet.key_frame) {
            key_packets_.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(packet_mutex_);
            latest_packet_ = packet;
            has_latest_packet_ = true;
        }

        PacketCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = packet_callback_;
        }
        if (callback) {
            callback(packet);
        }
    }
}

}  // namespace axvsdk::codec::internal
