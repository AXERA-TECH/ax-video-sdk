#include "ax_video_encoder_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <utility>

#include "common/ax_image_processor.h"
#include "common/ax_system.h"
#include "ax_image_copy.h"
#if defined(AXSDK_PLATFORM_AXCL)
#include "ax_system_internal.h"
#else
#include "ax_sys_api.h"
#endif

namespace axvsdk::codec {

std::unique_ptr<VideoEncoder> CreateVideoEncoder() {
    return internal::CreatePlatformVideoEncoder();
}

}  // namespace axvsdk::codec

namespace axvsdk::codec::internal {

namespace {

constexpr int kEncoderStopIdlePollLimit = 10;
constexpr std::size_t kEncoderInputFifoDepth = 4;

#if defined(AXSDK_CHIP_AX620E_FAMILY)
constexpr AX_U32 kEncoderPoolBlockCount = 8;
constexpr AX_U64 kEncoderPoolMetaSize = 512;
#endif

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
    resolved.device_id = config.device_id;
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
    resolved.resize = config.resize;
    return resolved;
}

}  // namespace

#if defined(AXSDK_CHIP_AX620E_FAMILY)
// ax620e/ax630c family: VENC and some firmware paths are more robust with pool-backed buffers (u32BlkId valid).
// Use a small private pool per encoder instance for reusable intermediate frames.
static std::size_t PlaneHeight(const common::ImageDescriptor& descriptor, std::size_t plane_index) noexcept {
    switch (descriptor.format) {
    case common::PixelFormat::kNv12:
        return plane_index == 0 ? descriptor.height : descriptor.height / 2U;
    case common::PixelFormat::kRgb24:
    case common::PixelFormat::kBgr24:
        return descriptor.height;
    case common::PixelFormat::kUnknown:
    default:
        return 0;
    }
}

static AX_U64 ComputeImageByteSize(const common::ImageDescriptor& descriptor) noexcept {
    const auto h0 = PlaneHeight(descriptor, 0);
    if (h0 == 0) {
        return 0;
    }
    AX_U64 total = static_cast<AX_U64>(descriptor.strides[0]) * static_cast<AX_U64>(h0);
    if (descriptor.format == common::PixelFormat::kNv12) {
        const auto h1 = PlaneHeight(descriptor, 1);
        total += static_cast<AX_U64>(descriptor.strides[1]) * static_cast<AX_U64>(h1);
    }
    return total;
}

static bool SameDescriptor(const common::ImageDescriptor& a, const common::ImageDescriptor& b) noexcept {
    if (a.format != b.format || a.width != b.width || a.height != b.height) {
        return false;
    }
    for (std::size_t i = 0; i < common::kMaxImagePlanes; ++i) {
        if (a.strides[i] != b.strides[i]) {
            return false;
        }
    }
    return true;
}
#endif  // AXSDK_CHIP_AX620E_FAMILY

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
            inflight_hold_frames_.clear();
#if defined(AXSDK_CHIP_AX620E_FAMILY)
            for (auto& entry : pools_) {
                if (entry.pool_id != common::kInvalidPoolId) {
                    (void)AX_POOL_DestroyPool(entry.pool_id);
                }
            }
#endif
            pools_.clear();
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
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_frames_.clear();
    }
    input_cv_.notify_all();

    if (send_thread_.joinable()) {
        send_thread_.join();
    }

    StopBackend();

    if (stream_thread_.joinable()) {
        stream_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        reusable_frames_.clear();
        inflight_frames_.clear();
        inflight_hold_frames_.clear();
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

bool AxVideoEncoderBase::stop_requested() const noexcept {
    return stop_requested_.load(std::memory_order_relaxed);
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

#if defined(AXSDK_CHIP_AX620E_FAMILY)
    if (descriptor.format == common::PixelFormat::kNv12) {
        const auto block_size = ComputeImageByteSize(descriptor);
        if (block_size != 0) {
            std::uint32_t pool_id = common::kInvalidPoolId;
            for (const auto& entry : pools_) {
                if (entry.pool_id != common::kInvalidPoolId && entry.block_size == block_size &&
                    SameDescriptor(entry.descriptor, descriptor)) {
                    pool_id = entry.pool_id;
                    break;
                }
            }

            if (pool_id == common::kInvalidPoolId) {
                AX_POOL_CONFIG_T pool_config{};
                pool_config.MetaSize = kEncoderPoolMetaSize;
                pool_config.BlkCnt = kEncoderPoolBlockCount;
                pool_config.BlkSize = block_size;
                pool_config.CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
                // Partition name must exist in the system CMM partitions (MSP samples use "anonymous").
                std::snprintf(reinterpret_cast<char*>(pool_config.PartitionName), AX_MAX_PARTITION_NAME_LEN,
                              "anonymous");

                const auto created_pool = AX_POOL_CreatePool(&pool_config);
                if (created_pool != AX_INVALID_POOLID) {
                    pool_id = static_cast<std::uint32_t>(created_pool);
                    pools_.push_back(PoolEntry{descriptor, block_size, pool_id});
                }
            }

            if (pool_id != common::kInvalidPoolId) {
                common::ImageAllocationOptions alloc{};
                alloc.memory_type = common::MemoryType::kPool;
                alloc.cache_mode = common::CacheMode::kNonCached;
                alloc.alignment = 0x1000;
                alloc.pool_id = pool_id;
                alloc.token = token;
                auto frame = common::AxImage::Create(descriptor, alloc);
                if (frame) {
                    return frame;
                }
            }
        }
    }
#endif

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
    if (inflight_frames_.size() > kEncoderInputFifoDepth) {
        reusable_frames_.push_back(std::move(inflight_frames_.front()));
        inflight_frames_.pop_front();
    }
    while (inflight_hold_frames_.size() > kEncoderInputFifoDepth) {
        inflight_hold_frames_.pop_front();
    }
}

PreparedInputFrame AxVideoEncoderBase::PrepareInputFrame(const common::AxImage& frame) {
#if defined(AXSDK_PLATFORM_AXCL)
    if (!common::internal::EnsureAxclThreadContext(config_.device_id)) {
        return {};
    }
#endif
    const auto target_stride = static_cast<std::size_t>(config_.width);
    const bool already_hw_ready =
        frame.format() == common::PixelFormat::kNv12 &&
        frame.width() == config_.width &&
        frame.height() == config_.height &&
        frame.physical_address(0) != 0 &&
        frame.stride(0) >= target_stride &&
        frame.stride(1) >= target_stride;
    if (already_hw_ready) {
        auto alias = common::AxImage::Ptr(const_cast<common::AxImage*>(&frame), [](common::AxImage*) {});
        const bool needs_keep_alive = frame.block_id(0) == common::kInvalidPoolId;
        return {std::move(alias), false, needs_keep_alive};
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
        if (!output || !common::internal::CopyImage(frame, output.get())) {
            std::fprintf(stderr,
                         "venc prepare: copy failed fmt=%d %ux%u stride=%zu/%zu phy=%llu blk=0x%x -> %ux%u\n",
                         static_cast<int>(frame.format()), frame.width(), frame.height(),
                         frame.stride(0), frame.stride(1),
                         static_cast<unsigned long long>(frame.physical_address(0)),
                         frame.block_id(0),
                         target.width, target.height);
            return {};
        }
        return {std::move(output), true, true};
    }

    auto processor = common::CreateImageProcessor();
    if (!processor) {
        std::fprintf(stderr, "venc prepare: CreateImageProcessor failed (ivps enabled?)\n");
        return {};
    }

    common::AxImage::Ptr source = common::AxImage::Ptr(const_cast<common::AxImage*>(&frame), [](common::AxImage*) {});
    bool source_reusable = false;
    if (frame.physical_address(0) == 0) {
        source = AcquireReusableFrame(frame.descriptor(), "VideoEncoderSource");
        if (!source || !common::internal::CopyImage(frame, source.get())) {
            std::fprintf(stderr,
                         "venc prepare: acquire/copy host source failed fmt=%d %ux%u\n",
                         static_cast<int>(frame.format()), frame.width(), frame.height());
            return {};
        }
        source_reusable = true;
    } else {
        (void)source->FlushCache();
    }

    common::ImageProcessRequest request{};
    request.output_image = target;
    request.resize = config_.resize;
    // Some decoders output padded heights (e.g. 1088 for 1080p). For encode targets matching the logical stream
    // height, prefer cropping away padding rows instead of scaling, to preserve geometry and quality.
    if (source &&
        source->format() == common::PixelFormat::kNv12 &&
        source->width() == target.width &&
        source->height() > target.height &&
        request.enable_crop == false &&
        request.resize.mode == common::ResizeMode::kStretch) {
        const auto extra = source->height() - target.height;
        if (extra > 0 && extra <= 64 && (extra % 2U) == 0U) {
            request.enable_crop = true;
            request.crop.x = 0;
            request.crop.y = 0;
            request.crop.width = target.width;
            request.crop.height = target.height;
        }
    }
    auto output = AcquireReusableFrame(target, "VideoEncoderInput");
    if (!output) {
        std::fprintf(stderr,
                     "venc prepare: acquire output failed fmt=%d %ux%u stride=%zu/%zu\n",
                     static_cast<int>(target.format), target.width, target.height,
                     target.strides[0], target.strides[1]);
        if (source_reusable) {
            RecyclePreparedFrame(std::move(source));
        }
        return {};
    }
    if (!processor->Process(*source, request, *output)) {
        std::fprintf(stderr,
                     "venc prepare: process failed src fmt=%d %ux%u -> dst %ux%u\n",
                     static_cast<int>(source->format()), source->width(), source->height(),
                     output->width(), output->height());
        if (source_reusable) {
            RecyclePreparedFrame(std::move(source));
        }
        return {};
    }
    if (source_reusable) {
        RecyclePreparedFrame(std::move(source));
    }
    return {std::move(output), true, true};
}

void AxVideoEncoderBase::SendLoop() {
    while (true) {
        common::AxImage::Ptr frame;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [this] { return stop_requested_ || !pending_frames_.empty(); });
            if (stop_requested_) {
                pending_frames_.clear();
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
            } else {
                // Keep the original input frame alive for a small inflight window.
                // Some platforms/drivers do not retain an input buffer reference inside VENC, so releasing
                // the decoder/pool buffer immediately can cause visible corruption/jitter.
                std::lock_guard<std::mutex> lock(staging_mutex_);
                inflight_hold_frames_.push_back(std::move(frame));
            }
            ReleaseOldInflightFrame();
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
