#include "ax_video_decoder_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

#include "ax_image_copy.h"
#include "ax_image_internal.h"
#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_sys.h"
#define AX_POOL_IncreaseRefCnt AXCL_POOL_IncreaseRefCnt
#define AX_POOL_DecreaseRefCnt AXCL_POOL_DecreaseRefCnt
#include "ax_system_internal.h"
#else
#include "ax_sys_api.h"
#endif

#include "common/ax_system.h"

namespace axvsdk::codec {

std::unique_ptr<VideoDecoder> CreateVideoDecoder() {
    return internal::CreatePlatformVideoDecoder();
}

}  // namespace axvsdk::codec

namespace axvsdk::codec::internal {

namespace {

constexpr std::size_t kDecodeInputQueueDepth = 16;
constexpr std::size_t kCallbackQueueDepth = 8;  // non-blocking; drop oldest when slow consumer

common::ImageDescriptor MakeNativeOutputDescriptor(const Mp4VideoInfo& video_info) noexcept {
    common::ImageDescriptor descriptor{};
    descriptor.format = common::PixelFormat::kNv12;
    descriptor.width = video_info.width;
    descriptor.height = video_info.height;
    return descriptor;
}

Mp4VideoInfo MakeDecoderVideoInfo(const VideoStreamInfo& stream) noexcept {
    Mp4VideoInfo video_info{};
    video_info.codec = stream.codec;
    video_info.width = stream.width;
    video_info.height = stream.height;
    video_info.timescale = 1000000U;
    video_info.fps = stream.frame_rate > 0.0 ? stream.frame_rate : 30.0;
    return video_info;
}

}  // namespace

AxVideoDecoderBase::~AxVideoDecoderBase() = default;

bool AxVideoDecoderBase::Open(const VideoDecoderConfig& config) {
    if (open_) {
        Close();
    }

    if (!common::IsSystemInitialized()) {
        return false;
    }

    config_ = config;
    if ((config.stream.codec != VideoCodecType::kH264 && config.stream.codec != VideoCodecType::kH265) ||
        config.stream.width == 0 || config.stream.height == 0) {
        return false;
    }

    video_info_ = MakeDecoderVideoInfo(config.stream);
    native_output_descriptor_ = MakeNativeOutputDescriptor(video_info_);
    if (!ValidateRequestedOutput(config_.output_image)) {
        return false;
    }

    if (!CreateBackend(video_info_)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_packets_.clear();
        input_eos_requested_ = false;
    }
    send_finished_ = false;
    callback_stop_ = false;
    open_ = true;
    return true;
}

void AxVideoDecoderBase::Close() noexcept {
    Stop();
    if (open_) {
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            pending_packets_.clear();
            input_eos_requested_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(latest_mutex_);
            latest_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            pending_callback_frame_.reset();
            callback_queue_.clear();
            frame_callback_ = {};
            callback_mode_ = FrameCallbackMode::kLatest;
        }
        DestroyBackend();
        open_ = false;
    }
}

bool AxVideoDecoderBase::Start() {
    if (!open_ || running_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_packets_.clear();
        input_eos_requested_ = false;
    }
    send_finished_ = false;
    stop_requested_ = false;
    callback_stop_ = false;

    if (!StartBackend()) {
        return false;
    }

    callback_thread_ = std::thread(&AxVideoDecoderBase::CallbackLoop, this);
    receive_thread_ = std::thread(&AxVideoDecoderBase::ReceiveLoop, this);
    send_thread_ = std::thread(&AxVideoDecoderBase::SendLoop, this);
    running_ = true;
    return true;
}

void AxVideoDecoderBase::Stop() noexcept {
    if (!running_) {
        return;
    }

    stop_requested_ = true;
    input_cv_.notify_all();
    callback_cv_.notify_all();

    if (send_thread_.joinable()) {
        send_thread_.join();
    }
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    StopBackend();

    callback_stop_ = true;
    callback_cv_.notify_all();
    if (callback_thread_.joinable()) {
        callback_thread_.join();
    }

    running_ = false;
}

bool AxVideoDecoderBase::SubmitPacket(EncodedPacket packet) {
    if (!running_ || packet.data.empty()) {
        return false;
    }

    std::unique_lock<std::mutex> lock(input_mutex_);
    input_cv_.wait(lock, [this] {
        return stop_requested_ || pending_packets_.size() < kDecodeInputQueueDepth;
    });
    if (stop_requested_ || input_eos_requested_) {
        return false;
    }
    pending_packets_.push_back(std::move(packet));
    lock.unlock();
    input_cv_.notify_one();
    return true;
}

bool AxVideoDecoderBase::SubmitEndOfStream() {
    if (!running_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(input_mutex_);
    input_eos_requested_ = true;
    input_cv_.notify_all();
    return true;
}

common::AxImage::Ptr AxVideoDecoderBase::GetLatestFrame() {
    std::lock_guard<std::mutex> lock(latest_mutex_);
    return latest_frame_;
}

bool AxVideoDecoderBase::GetLatestFrame(common::AxImage& output_image) {
    common::AxImage::Ptr latest_frame;
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame = latest_frame_;
    }

    if (!latest_frame) {
        return false;
    }

    return common::internal::CopyImage(*latest_frame, &output_image);
}

void AxVideoDecoderBase::SetFrameCallback(FrameCallback callback) {
    SetFrameCallback(std::move(callback), FrameCallbackMode::kLatest);
}

void AxVideoDecoderBase::SetFrameCallback(FrameCallback callback, FrameCallbackMode mode) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(callback);
    callback_mode_ = mode;
    pending_callback_frame_.reset();
    callback_queue_.clear();
    callback_cv_.notify_all();
}

const Mp4VideoInfo& AxVideoDecoderBase::video_info() const noexcept {
    return video_info_;
}

const VideoDecoderConfig& AxVideoDecoderBase::config() const noexcept {
    return config_;
}

const common::ImageDescriptor& AxVideoDecoderBase::native_output_descriptor() const noexcept {
    return native_output_descriptor_;
}

bool AxVideoDecoderBase::stop_requested() const noexcept {
    return stop_requested_.load(std::memory_order_relaxed);
}

void AxVideoDecoderBase::SendLoop() {
    while (!stop_requested_) {
        EncodedPacket packet;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [this] {
                return stop_requested_ || !pending_packets_.empty() || input_eos_requested_;
            });

            if (stop_requested_) {
                break;
            }

            if (pending_packets_.empty()) {
                if (input_eos_requested_) {
                    break;
                }
                continue;
            }

            packet = std::move(pending_packets_.front());
            pending_packets_.pop_front();
        }
        input_cv_.notify_all();

        if (packet.data.empty()) {
            continue;
        }

        if (!SendEncodedPacket(packet)) {
            break;
        }
    }

    if (!stop_requested_) {
        (void)SendEndOfStream();
    }
    send_finished_ = true;
    input_cv_.notify_all();
}

void AxVideoDecoderBase::ReceiveLoop() {
    while (!stop_requested_) {
        AX_VIDEO_FRAME_INFO_T frame_info{};
        bool flow_end = false;
        if (!ReceiveDecodedFrame(&frame_info, &flow_end)) {
            if (flow_end && send_finished_) {
                break;
            }
            continue;
        }

        PublishFrame(frame_info);
        ReleaseDecodedFrame(frame_info);
    }
}

void AxVideoDecoderBase::CallbackLoop() {
    while (true) {
        FrameCallback callback;
        common::AxImage::Ptr frame;
        {
            std::unique_lock<std::mutex> lock(callback_mutex_);
            callback_cv_.wait(lock, [this] {
                return callback_stop_ || pending_callback_frame_ != nullptr || !callback_queue_.empty();
            });

            if (callback_stop_ && pending_callback_frame_ == nullptr && callback_queue_.empty()) {
                return;
            }

            callback = frame_callback_;
            if (callback_mode_ == FrameCallbackMode::kLatest) {
                frame = std::move(pending_callback_frame_);
            } else {
                if (!callback_queue_.empty()) {
                    frame = std::move(callback_queue_.front());
                    callback_queue_.pop_front();
                }
            }
        }

        if (callback && frame) {
            callback(std::move(frame));
        }
    }
}

bool AxVideoDecoderBase::ValidateRequestedOutput(const common::ImageDescriptor& requested) const noexcept {
    if (requested.format != common::PixelFormat::kUnknown &&
        requested.format != native_output_descriptor_.format) {
        return false;
    }
    if (requested.width != 0 && requested.width != native_output_descriptor_.width) {
        return false;
    }
    if (requested.height != 0 && requested.height != native_output_descriptor_.height) {
        return false;
    }
    return true;
}

void AxVideoDecoderBase::PublishFrame(const AX_VIDEO_FRAME_INFO_T& frame_info) {
    common::AxImage::Ptr published_frame;
    const auto block_id = frame_info.stVFrame.u32BlkId[0];
    if (block_id != AX_INVALID_BLOCKID) {
#if defined(AXSDK_PLATFORM_AXCL)
        if (!common::internal::EnsureAxclThreadContext()) {
            ReleaseDecodedFrame(frame_info);
            return;
        }
#endif
        const auto ref_ret = AX_POOL_IncreaseRefCnt(block_id);
        if (ref_ret == AX_SUCCESS) {
            // Keep the raw frame info unchanged. Some modules (notably VENC) may derive plane offsets internally
            // from width/height/stride, and clamping padded dimensions here can corrupt chroma addressing.
            published_frame = common::internal::AxImageAccess::WrapVideoFrame(
                frame_info, [](const AX_VIDEO_FRAME_INFO_T& retained_frame) {
                    if (retained_frame.stVFrame.u32BlkId[0] != AX_INVALID_BLOCKID) {
#if defined(AXSDK_PLATFORM_AXCL)
                        if (!common::internal::EnsureAxclThreadContext()) {
                            return;
                        }
#endif
                        (void)AX_POOL_DecreaseRefCnt(retained_frame.stVFrame.u32BlkId[0]);
                    }
                });
        }
    }

    if (!published_frame) {
        auto fallback = common::AxImage::Create(native_output_descriptor_);
        if (fallback && common::internal::CopyVideoFrameToImage(frame_info, fallback.get())) {
            published_frame = std::move(fallback);
        }
    }

    if (!published_frame) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame_ = published_frame;
    }

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (frame_callback_) {
            if (callback_mode_ == FrameCallbackMode::kLatest) {
                pending_callback_frame_ = published_frame;
            } else {
                callback_queue_.push_back(published_frame);
                if (callback_queue_.size() > kCallbackQueueDepth) {
                    callback_queue_.pop_front();
                }
            }
            callback_cv_.notify_one();
        }
    }
}

}  // namespace axvsdk::codec::internal
