#include "pipeline/ax_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "codec/ax_video_decoder.h"
#include "codec/ax_video_encoder.h"
#include "ax_image_copy.h"
#include "ax_drawer_internal.h"
#include "common/ax_drawer.h"
#include "common/ax_image.h"
#include "common/ax_image_processor.h"

namespace axvsdk::pipeline {

namespace {

constexpr std::size_t kFrameStrideAlignment = 16;

std::size_t AlignUp(std::size_t value, std::size_t alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

std::size_t PlaneCount(common::PixelFormat format) noexcept {
    switch (format) {
    case common::PixelFormat::kNv12:
        return 2;
    case common::PixelFormat::kRgb24:
    case common::PixelFormat::kBgr24:
        return 1;
    case common::PixelFormat::kUnknown:
    default:
        return 0;
    }
}

std::size_t MinStride(common::PixelFormat format, std::uint32_t width, std::size_t plane_index) noexcept {
    switch (format) {
    case common::PixelFormat::kNv12:
        return plane_index < 2 ? width : 0;
    case common::PixelFormat::kRgb24:
    case common::PixelFormat::kBgr24:
        return plane_index == 0 ? static_cast<std::size_t>(width) * 3U : 0;
    case common::PixelFormat::kUnknown:
    default:
        return 0;
    }
}

bool AllStridesUnset(const common::ImageDescriptor& descriptor) noexcept {
    for (const auto stride : descriptor.strides) {
        if (stride != 0) {
            return false;
        }
    }
    return true;
}

bool ResolveOutputDescriptor(const common::ImageDescriptor& source,
                             const common::ImageDescriptor& requested,
                             common::ImageDescriptor* resolved) noexcept {
    if (resolved == nullptr || source.format == common::PixelFormat::kUnknown ||
        source.width == 0 || source.height == 0) {
        return false;
    }

    *resolved = requested;
    if (resolved->format == common::PixelFormat::kUnknown) {
        resolved->format = source.format;
    }
    if (resolved->width == 0) {
        resolved->width = source.width;
    }
    if (resolved->height == 0) {
        resolved->height = source.height;
    }

    const auto plane_count = PlaneCount(resolved->format);
    if (plane_count == 0 || resolved->width == 0 || resolved->height == 0) {
        return false;
    }

    if (resolved->format == common::PixelFormat::kNv12 &&
        ((resolved->width % 2U) != 0U || (resolved->height % 2U) != 0U)) {
        return false;
    }

    const bool same_geometry_and_format =
        resolved->format == source.format &&
        resolved->width == source.width &&
        resolved->height == source.height;
    if (same_geometry_and_format && AllStridesUnset(requested)) {
        *resolved = source;
        return true;
    }

    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        const auto min_stride = MinStride(resolved->format, resolved->width, plane);
        if (min_stride == 0) {
            return false;
        }

        if (resolved->strides[plane] == 0) {
            resolved->strides[plane] = AlignUp(min_stride, kFrameStrideAlignment);
        }

        if (resolved->strides[plane] < min_stride) {
            return false;
        }
    }

    return true;
}

bool NeedsImageProcessing(const common::AxImage& source, const common::ImageDescriptor& output) noexcept {
    return source.format() != output.format || source.width() != output.width || source.height() != output.height;
}

common::AxImage::Ptr CreateOutputImage(const common::ImageDescriptor& descriptor) {
    common::ImageAllocationOptions options{};
    options.memory_type = common::MemoryType::kCmm;
    options.cache_mode = common::CacheMode::kNonCached;
    options.alignment = 0x1000;
    options.token = "PipelineFrame";
    return common::AxImage::Create(descriptor, options);
}

class AxPipeline final : public Pipeline {
public:
    ~AxPipeline() override {
        Close();
    }

    bool Open(const PipelineConfig& config) override {
        if (open_) {
            Close();
        }

        if (config.input.uri.empty() || config.outputs.empty()) {
            return false;
        }

        auto demuxer = CreateDemuxer();
        if (!demuxer) {
            return false;
        }
        if (!demuxer->Open(config.input)) {
            return false;
        }

        const auto input_stream = demuxer->GetVideoStreamInfo();
        if ((input_stream.codec != codec::VideoCodecType::kH264 &&
             input_stream.codec != codec::VideoCodecType::kH265) ||
            input_stream.width == 0 || input_stream.height == 0) {
            return false;
        }

        auto decoder = codec::CreateVideoDecoder();
        if (!decoder) {
            return false;
        }

        codec::VideoDecoderConfig decoder_config{};
        decoder_config.stream = input_stream;
        decoder_config.device_id = config.device_id;
        if (!decoder->Open(decoder_config)) {
            return false;
        }

        std::vector<OutputBranch> branches;
        branches.reserve(config.outputs.size());
        bool needs_branch_processing = false;
        const double input_frame_rate = input_stream.frame_rate > 0.0 ? input_stream.frame_rate : 30.0;
        for (const auto& output : config.outputs) {
            if (output.uris.empty() && !output.packet_callback) {
                decoder->Close();
                return false;
            }

            auto encoder = codec::CreateVideoEncoder();
            if (!encoder) {
                decoder->Close();
                return false;
            }

            codec::VideoEncoderConfig encoder_config{};
            encoder_config.codec = output.codec;
            encoder_config.width = output.width == 0 ? input_stream.width : output.width;
            encoder_config.height = output.height == 0 ? input_stream.height : output.height;
            encoder_config.device_id = config.device_id;
            encoder_config.frame_rate = output.frame_rate > 0.0 ? output.frame_rate : input_frame_rate;
            encoder_config.bitrate_kbps = output.bitrate_kbps;
            encoder_config.gop = output.gop;
            if (output.input_queue_depth > 0) {
                encoder_config.input_queue_depth = output.input_queue_depth;
            }
            encoder_config.overflow_policy = output.overflow_policy;

            if (!encoder->Open(encoder_config)) {
                decoder->Close();
                for (auto& branch : branches) {
                    if (branch.muxer) {
                        branch.muxer->Close();
                    }
                    branch.encoder->Close();
                }
                return false;
            }

            std::unique_ptr<Muxer> muxer;
            if (!output.uris.empty()) {
                muxer = CreateMuxer();
                if (!muxer) {
                    decoder->Close();
                    encoder->Close();
                    for (auto& branch : branches) {
                        if (branch.muxer) {
                            branch.muxer->Close();
                        }
                        branch.encoder->Close();
                    }
                    return false;
                }

                MuxerConfig muxer_config{};
                muxer_config.stream.codec = output.codec;
                muxer_config.stream.width = encoder_config.width;
                muxer_config.stream.height = encoder_config.height;
                muxer_config.stream.frame_rate = encoder_config.frame_rate > 0.0 ? encoder_config.frame_rate
                                                                                  : input_frame_rate;
                muxer_config.uris = output.uris;
                if (!muxer->Open(muxer_config)) {
                    decoder->Close();
                    encoder->Close();
                    for (auto& branch : branches) {
                        if (branch.muxer) {
                            branch.muxer->Close();
                        }
                        branch.encoder->Close();
                    }
                    return false;
                }
            }

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

            common::ImageProcessRequest process_request{};
            process_request.output_image.format = common::PixelFormat::kNv12;
            process_request.output_image.width = encoder_config.width;
            process_request.output_image.height = encoder_config.height;
            process_request.resize = output.resize;

            const bool branch_needs_processing =
                encoder_config.width != input_stream.width || encoder_config.height != input_stream.height;
            needs_branch_processing = needs_branch_processing || branch_needs_processing;

            branches.push_back(
                OutputBranch{output, std::move(encoder), std::move(muxer), process_request, branch_needs_processing});
        }

        if (needs_branch_processing) {
            branch_processor_ = common::CreateImageProcessor();
            if (!branch_processor_) {
                decoder->Close();
                for (auto& branch : branches) {
                    if (branch.muxer) {
                        branch.muxer->Close();
                    }
                    branch.encoder->Close();
                }
                return false;
            }
        } else {
            branch_processor_.reset();
        }

        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            latest_source_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> callback_lock(frame_callback_mutex_);
            pending_callback_source_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> processor_lock(frame_processor_mutex_);
            frame_processor_.reset();
        }
        {
            std::lock_guard<std::mutex> osd_lock(osd_mutex_);
            pending_osd_.reset();
            active_osd_.reset();
            active_osd_remaining_frames_ = 0;
        }

        config_ = config;
        demuxer_ = std::move(demuxer);
        decoder_ = std::move(decoder);
        branches_ = std::move(branches);
        drawer_ = common::internal::CreatePlatformDrawer();
        decoded_frames_.store(0, std::memory_order_relaxed);
        branch_submit_failures_.store(0, std::memory_order_relaxed);
        demux_stop_ = false;
        open_ = true;

        decoder_->SetFrameCallback([this](common::AxImage::Ptr frame) {
            decoded_frames_.fetch_add(1, std::memory_order_relaxed);
            FanoutFrame(std::move(frame));
        });
        return true;
    }

    void Close() noexcept override {
        Stop();

        if (!open_) {
            return;
        }

        if (decoder_) {
            decoder_->Close();
            decoder_.reset();
        }
        if (demux_thread_.joinable()) {
            demux_thread_.join();
        }
        demuxer_.reset();

        for (auto& branch : branches_) {
            if (branch.muxer) {
                branch.muxer->Close();
            }
            branch.encoder->Close();
        }
        branches_.clear();
        branch_processor_.reset();
        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            latest_source_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> callback_lock(frame_callback_mutex_);
            pending_callback_source_frame_.reset();
            frame_callback_ = {};
        }
        {
            std::lock_guard<std::mutex> processor_lock(frame_processor_mutex_);
            frame_processor_.reset();
        }
        {
            std::lock_guard<std::mutex> osd_lock(osd_mutex_);
            pending_osd_.reset();
            active_osd_.reset();
            active_osd_remaining_frames_ = 0;
        }
        drawer_.reset();
        config_ = {};
        open_ = false;
    }

    bool Start() override {
        if (!open_ || running_) {
            return false;
        }

        {
            std::lock_guard<std::mutex> callback_lock(frame_callback_mutex_);
            pending_callback_source_frame_.reset();
        }
        callback_stop_ = false;
        frame_callback_thread_ = std::thread(&AxPipeline::FrameCallbackLoop, this);

        for (auto& branch : branches_) {
            if (!branch.encoder->Start()) {
                for (auto& started_branch : branches_) {
                    started_branch.encoder->Stop();
                }
                StopFrameCallbackThread();
                return false;
            }
        }

        if (!decoder_ || !decoder_->Start()) {
            for (auto& branch : branches_) {
                branch.encoder->Stop();
            }
            StopFrameCallbackThread();
            return false;
        }

        demux_stop_ = false;
        if (demuxer_) {
            if (!demuxer_->Reset()) {
                demux_stop_ = true;
                decoder_->Stop();
                for (auto& branch : branches_) {
                    branch.encoder->Stop();
                }
                StopFrameCallbackThread();
                return false;
            }
            demux_thread_ = std::thread(&AxPipeline::DemuxLoop, this);
        }

        running_ = true;
        return true;
    }

    void Stop() noexcept override {
        if (!running_) {
            return;
        }

        demux_stop_ = true;
        if (demuxer_) {
            demuxer_->Interrupt();
        }
        if (decoder_) {
            decoder_->Stop();
        }
        if (demux_thread_.joinable()) {
            demux_thread_.join();
        }

        for (auto& branch : branches_) {
            branch.encoder->Stop();
        }

        StopFrameCallbackThread();
        running_ = false;
    }

    common::AxImage::Ptr GetLatestFrame() override {
        common::AxImage::Ptr source_frame;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            source_frame = latest_source_frame_;
        }
        if (!source_frame) {
            return nullptr;
        }

        common::ImageDescriptor output_descriptor{};
        if (!ResolveOutputDescriptor(source_frame->descriptor(), config_.frame_output.output_image, &output_descriptor)) {
            return nullptr;
        }

        common::ImageProcessRequest request{};
        request.output_image = output_descriptor;
        request.resize = config_.frame_output.resize;
        return CreateFrameCopy(*source_frame, request);
    }

    bool GetLatestFrame(common::AxImage& output_image) override {
        common::AxImage::Ptr source_frame;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            source_frame = latest_source_frame_;
        }
        if (!source_frame) {
            return false;
        }

        common::ImageProcessRequest request{};
        request.output_image = output_image.descriptor();
        request.resize = config_.frame_output.resize;
        return CopyOrProcessFrame(*source_frame, request, &output_image);
    }

    void SetFrameCallback(FrameCallback callback) override {
        std::lock_guard<std::mutex> lock(frame_callback_mutex_);
        frame_callback_ = std::move(callback);
        if (!frame_callback_) {
            pending_callback_source_frame_.reset();
        }
    }

    bool SetOsd(const common::DrawFrame& osd) override {
        if (!open_ || !drawer_) {
            return false;
        }

        const bool empty =
            osd.lines.empty() && osd.polygons.empty() && osd.rects.empty() && osd.mosaics.empty() && osd.bitmaps.empty();
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

    PipelineStats GetStats() const override {
        PipelineStats stats{};
        stats.decoded_frames = decoded_frames_.load(std::memory_order_relaxed);
        stats.branch_submit_failures = branch_submit_failures_.load(std::memory_order_relaxed);
        stats.output_stats.reserve(branches_.size());
        for (const auto& branch : branches_) {
            stats.output_stats.push_back(branch.encoder->GetStats());
        }
        return stats;
    }

private:
    struct OutputBranch {
        PipelineOutputConfig config{};
        std::unique_ptr<codec::VideoEncoder> encoder;
        std::unique_ptr<Muxer> muxer;
        common::ImageProcessRequest process_request{};
        bool needs_processing{false};
    };

    void StopFrameCallbackThread() noexcept {
        {
            std::lock_guard<std::mutex> lock(frame_callback_mutex_);
            pending_callback_source_frame_.reset();
            callback_stop_ = true;
        }
        frame_callback_cv_.notify_all();
        if (frame_callback_thread_.joinable()) {
            frame_callback_thread_.join();
        }
    }

    bool EnsureFrameProcessor() {
        std::lock_guard<std::mutex> lock(frame_processor_mutex_);
        if (!frame_processor_) {
            frame_processor_ = common::CreateImageProcessor();
        }
        return frame_processor_ != nullptr;
    }

    common::AxImage::Ptr CreateFrameCopy(const common::AxImage& source,
                                         const common::ImageProcessRequest& request) {
        auto output = CreateOutputImage(request.output_image);
        if (!output) {
            return nullptr;
        }

        if (!CopyOrProcessFrame(source, request, output.get())) {
            return nullptr;
        }

        return output;
    }

    bool CopyOrProcessFrame(const common::AxImage& source,
                            const common::ImageProcessRequest& request,
                            common::AxImage* destination) {
        if (destination == nullptr) {
            return false;
        }

        if (!NeedsImageProcessing(source, request.output_image)) {
            return common::internal::CopyImage(source, destination);
        }

        if (!EnsureFrameProcessor()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(frame_processor_mutex_);
        return frame_processor_ && frame_processor_->Process(source, request, *destination);
    }

    void ApplyPendingOsdUpdateLocked() {
        if (!pending_osd_) {
            return;
        }

        active_osd_ = std::move(pending_osd_);
        active_osd_remaining_frames_ = active_osd_->hold_frames();
    }

    void ApplyOsdIfNeeded(common::AxImage::Ptr* frame) {
        if (frame == nullptr || !*frame) {
            return;
        }

        std::shared_ptr<const common::PreparedDrawCommands> osd_to_apply;
        {
            std::lock_guard<std::mutex> lock(osd_mutex_);
            ApplyPendingOsdUpdateLocked();
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

        auto osd_frame = CreateOutputImage((*frame)->descriptor());
        if (!osd_frame || !common::internal::CopyImage(**frame, osd_frame.get())) {
            std::fprintf(stderr, "pipeline osd: copy frame failed\n");
            return;
        }

        if (!osd_to_apply->Apply(*osd_frame)) {
            std::fprintf(stderr, "pipeline osd: apply failed\n");
            return;
        }

        *frame = std::move(osd_frame);
    }

    void DemuxLoop() {
        if (!demuxer_ || !decoder_) {
            return;
        }

        while (!demux_stop_) {
            codec::EncodedPacket packet;
            if (!demuxer_->ReadPacket(&packet)) {
                break;
            }

            if (!decoder_->SubmitPacket(std::move(packet))) {
                break;
            }
        }

        if (!demux_stop_ && decoder_) {
            (void)decoder_->SubmitEndOfStream();
        }
    }

    void FrameCallbackLoop() {
        while (true) {
            FrameCallback callback;
            common::AxImage::Ptr source_frame;
            {
                std::unique_lock<std::mutex> lock(frame_callback_mutex_);
                frame_callback_cv_.wait(lock, [this] {
                    return callback_stop_ || pending_callback_source_frame_ != nullptr;
                });

                if (callback_stop_ && pending_callback_source_frame_ == nullptr) {
                    return;
                }

                callback = frame_callback_;
                source_frame = std::move(pending_callback_source_frame_);
            }

            if (!callback || !source_frame) {
                continue;
            }

            common::ImageDescriptor output_descriptor{};
            if (!ResolveOutputDescriptor(source_frame->descriptor(), config_.frame_output.output_image, &output_descriptor)) {
                continue;
            }

            common::ImageProcessRequest request{};
            request.output_image = output_descriptor;
            request.resize = config_.frame_output.resize;
            auto callback_frame = CreateFrameCopy(*source_frame, request);
            if (callback_frame) {
                callback(std::move(callback_frame));
            }
        }
    }

    void FanoutFrame(common::AxImage::Ptr frame) {
        if (!frame) {
            return;
        }

        ApplyOsdIfNeeded(&frame);

        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_source_frame_ = frame;
        }

        {
            std::lock_guard<std::mutex> lock(frame_callback_mutex_);
            if (frame_callback_) {
                pending_callback_source_frame_ = frame;
                frame_callback_cv_.notify_one();
            }
        }

        for (auto& branch : branches_) {
            common::AxImage::Ptr frame_for_branch = frame;
            if (branch.needs_processing) {
                if (!branch_processor_) {
                    branch_submit_failures_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                frame_for_branch = branch_processor_->Process(*frame, branch.process_request);
            }

            if (!frame_for_branch || !branch.encoder->SubmitFrame(std::move(frame_for_branch))) {
                branch_submit_failures_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    PipelineConfig config_{};
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<codec::VideoDecoder> decoder_;
    std::unique_ptr<common::ImageProcessor> branch_processor_;
    std::vector<OutputBranch> branches_;
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> demux_stop_{false};
    std::atomic<bool> callback_stop_{false};
    std::atomic<std::uint64_t> decoded_frames_{0};
    std::atomic<std::uint64_t> branch_submit_failures_{0};

    mutable std::mutex frame_mutex_;
    common::AxImage::Ptr latest_source_frame_;

    std::mutex frame_callback_mutex_;
    std::condition_variable frame_callback_cv_;
    FrameCallback frame_callback_;
    common::AxImage::Ptr pending_callback_source_frame_;
    std::thread demux_thread_;
    std::thread frame_callback_thread_;

    std::mutex frame_processor_mutex_;
    std::unique_ptr<common::ImageProcessor> frame_processor_;

    std::mutex osd_mutex_;
    std::unique_ptr<common::AxDrawer> drawer_;
    std::shared_ptr<const common::PreparedDrawCommands> pending_osd_;
    std::shared_ptr<const common::PreparedDrawCommands> active_osd_;
    std::uint32_t active_osd_remaining_frames_{0};
};

}  // namespace

std::unique_ptr<Pipeline> CreatePipeline() {
    return std::make_unique<AxPipeline>();
}

}  // namespace axvsdk::pipeline
