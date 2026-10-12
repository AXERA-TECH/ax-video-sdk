#include "pipeline/ax_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
#include "ax_image_internal.h"
#include "ax_drawer_internal.h"
#include "common/ax_drawer.h"
#include "common/ax_image.h"
#include "common/ax_image_processor.h"

#include "ax_motion_roi.h"

#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_rt_device.h"
#include "axcl_rt_memory.h"
#include "ax_system_internal.h"
#endif

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

bool SameDescriptor(const common::ImageDescriptor& a, const common::ImageDescriptor& b) noexcept {
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

        if (config.input.uri.empty()) {
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
        input_stream_ = input_stream;

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
            encoder_config.qpmap = output.qpmap;
            if (output.input_queue_depth > 0) {
                encoder_config.input_queue_depth = output.input_queue_depth;
            }
            encoder_config.overflow_policy = output.overflow_policy;
            encoder_config.resize = output.resize;

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

            branches.push_back(OutputBranch{output, std::move(encoder), std::move(muxer)});
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
        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            branches_ = std::move(branches);
        }
        drawer_ = common::internal::CreatePlatformDrawer();
        decoded_frames_.store(0, std::memory_order_relaxed);
        frame_seq_.store(0, std::memory_order_relaxed);
        if (config_.motion_roi.enable) {
            // 网格默认取原图 1/8(1080p -> 240x136)。这个上限是实测出来的硬约束:
            // IVPS 单次缩放到 1/16 会直接失败,到 1/32 更会把 IVPS 卡死,
            // 所以把降采样倍率钳在 8 倍以内。网格偏细只是略增噪点敏感度,
            // 而 ROI 本来就追求过量覆盖,无害。NV12 要求偶数边长。
            auto even = [](std::uint32_t v) { return v < 2 ? 2U : (v & ~1U); };
            const auto min_cols = (input_stream_.width + 7) / 8;
            const auto min_rows = (input_stream_.height + 7) / 8;
            const auto cols = even(std::max(min_cols, config_.motion_roi.grid_cols));
            const auto rows = even(std::max(min_rows, config_.motion_roi.grid_rows));
            motion_failures_ = 0;
            if (!motion_.Reset(cols, rows, input_stream_.width, input_stream_.height,
                               config_.motion_roi)) {
                std::fprintf(stderr, "pipeline: motion roi init failed (grid %ux%u)\n", cols, rows);
            }
        }
        det_source_seq_.store(0, std::memory_order_relaxed);
        det_lag_frames_.store(0, std::memory_order_relaxed);
        det_lag_frames_max_.store(0, std::memory_order_relaxed);
        branch_submit_failures_.store(0, std::memory_order_relaxed);
        demux_stop_ = false;
        open_ = true;

        decoder_->SetFrameCallback([this](common::AxImage::Ptr frame) {
            decoded_frames_.fetch_add(1, std::memory_order_relaxed);
            FanoutFrame(std::move(frame));
        }, codec::FrameCallbackMode::kQueue);
        return true;
    }

    codec::VideoStreamInfo GetInputStreamInfo() const noexcept override {
        return input_stream_;
    }

    void Close() noexcept override {
        Stop();

        if (!open_) {
            return;
        }

        motion_.Clear();
        motion_frame_.reset();
        motion_host_.clear();
        {
            std::lock_guard<std::mutex> lock(roi_mutex_);
            det_rois_.clear();
            motion_rois_.clear();
        }

        if (decoder_) {
            decoder_->Close();
            decoder_.reset();
        }
        if (demux_thread_.joinable()) {
            demux_thread_.join();
        }
        demuxer_.reset();

        std::vector<OutputBranch> branches;
        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            branches = std::move(branches_);
            branches_.clear();
        }
        for (auto& branch : branches) {
            if (branch.encoder) {
                if (branch.muxer) {
                    branch.muxer->Close();
                }
                branch.encoder->Close();
            }
        }
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

        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            for (auto& branch : branches_) {
                if (!branch.encoder->Start()) {
                    for (auto& started_branch : branches_) {
                        started_branch.encoder->Stop();
                    }
                    StopFrameCallbackThread();
                    return false;
                }
            }
        }

        if (!decoder_ || !decoder_->Start()) {
            {
                std::lock_guard<std::mutex> lock(branches_mutex_);
                for (auto& branch : branches_) {
                    branch.encoder->Stop();
                }
            }
            StopFrameCallbackThread();
            return false;
        }

        demux_stop_ = false;
        if (demuxer_) {
            if (!demuxer_->Reset()) {
                demux_stop_ = true;
                decoder_->Stop();
                {
                    std::lock_guard<std::mutex> lock(branches_mutex_);
                    for (auto& branch : branches_) {
                        branch.encoder->Stop();
                    }
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

        {
            std::lock_guard<std::mutex> osd_lock(osd_mutex_);
            pending_osd_.reset();
            active_osd_.reset();
            active_osd_remaining_frames_ = 0;
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
            std::lock_guard<std::mutex> lock(branches_mutex_);
            for (auto& branch : branches_) {
                branch.encoder->Stop();
            }
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

        // Zero-copy fast path: when no format/geometry/stride change is requested, return the source frame directly.
        // The returned shared_ptr keeps the underlying buffer alive and does not block the decode thread.
        if (SameDescriptor(output_descriptor, source_frame->descriptor())) {
            return source_frame;
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

    void SetDetectionSourceSeq(std::uint64_t source_seq) override {
        det_source_seq_.store(source_seq, std::memory_order_relaxed);
    }

    void SetQpRoi(const std::vector<codec::QpRoiRect>& rois) override {
        {
            std::lock_guard<std::mutex> lock(roi_mutex_);
            det_rois_ = rois;
        }
        PushCombinedRois();
    }

    // 最终 ROI = 检测框 ∪ 运动区域。检测框保住"认识的目标",
    // 运动区域兜住"检测器不认识但在动的东西",后者否则会被当背景糊掉。
    void PushCombinedRois() {
        std::vector<codec::QpRoiRect> rois;
        {
            std::lock_guard<std::mutex> lock(roi_mutex_);
            rois = det_rois_;
            rois.insert(rois.end(), motion_rois_.begin(), motion_rois_.end());
        }
        PushRoisToEncoders(rois);
    }

    void PushRoisToEncoders(const std::vector<codec::QpRoiRect>& rois) {
        const auto src_w = input_stream_.width;
        const auto src_h = input_stream_.height;
        if (src_w == 0 || src_h == 0) return;
        std::lock_guard<std::mutex> lock(branches_mutex_);
        for (auto& branch : branches_) {
            if (!branch.encoder || !branch.config.qpmap.enable) continue;
            const auto dst_w = branch.config.width == 0 ? src_w : branch.config.width;
            const auto dst_h = branch.config.height == 0 ? src_h : branch.config.height;
            if (dst_w == src_w && dst_h == src_h) {
                branch.encoder->SetQpRoi(rois);
                continue;
            }
            // 输出被缩放过:ROI 跟着缩放到编码坐标系
            std::vector<codec::QpRoiRect> scaled;
            scaled.reserve(rois.size());
            for (const auto& r : rois) {
                codec::QpRoiRect s{};
                s.x = static_cast<std::int32_t>(static_cast<std::int64_t>(r.x) * dst_w / src_w);
                s.y = static_cast<std::int32_t>(static_cast<std::int64_t>(r.y) * dst_h / src_h);
                s.width = static_cast<std::uint32_t>(static_cast<std::uint64_t>(r.width) * dst_w / src_w);
                s.height = static_cast<std::uint32_t>(static_cast<std::uint64_t>(r.height) * dst_h / src_h);
                scaled.push_back(s);
            }
            branch.encoder->SetQpRoi(scaled);
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
        stats.det_lag_frames = det_lag_frames_.load(std::memory_order_relaxed);
        stats.det_lag_frames_max = det_lag_frames_max_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            stats.output_stats.reserve(branches_.size());
            for (const auto& branch : branches_) {
                stats.output_stats.push_back(branch.encoder->GetStats());
            }
        }
        return stats;
    }

    bool AddOutput(const PipelineOutputConfig& output,
                   std::size_t* out_index,
                   std::string* error) override {
        if (!open_) {
            if (error) *error = "pipeline not opened";
            return false;
        }

        const double input_frame_rate = input_stream_.frame_rate > 0.0 ? input_stream_.frame_rate : 30.0;
        OutputBranch branch;
        std::string err;
        if (!CreateOutputBranch(output, config_.device_id, input_frame_rate, &branch, &err)) {
            if (error) *error = err.empty() ? "create output branch failed" : err;
            return false;
        }

        if (running_) {
            if (!branch.encoder->Start()) {
                if (branch.muxer) {
                    branch.muxer->Close();
                }
                branch.encoder->Close();
                if (error) *error = "encoder start failed";
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            branches_.push_back(std::move(branch));
            config_.outputs.push_back(output);
            if (out_index) {
                *out_index = branches_.size() - 1U;
            }
        }
        return true;
    }

    bool RemoveOutput(std::size_t index, std::string* error) override {
        OutputBranch removed;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            if (index >= branches_.size()) {
                if (error) *error = "output index out of range";
                return false;
            }

            removed = std::move(branches_[index]);
            branches_.erase(branches_.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < config_.outputs.size()) {
                config_.outputs.erase(config_.outputs.begin() + static_cast<std::ptrdiff_t>(index));
            }
            found = true;
        }

        if (!found) {
            if (error) *error = "output not found";
            return false;
        }

        // Close outside the lock to avoid blocking the decode thread.
        removed.encoder->Stop();
        if (removed.muxer) {
            removed.muxer->Close();
        }
        removed.encoder->Close();
        return true;
    }

private:
    struct OutputBranch {
        PipelineOutputConfig config{};
        std::unique_ptr<codec::VideoEncoder> encoder;
        std::unique_ptr<Muxer> muxer;
    };

    bool CreateOutputBranch(const PipelineOutputConfig& output,
                            std::int32_t device_id,
                            double input_frame_rate,
                            OutputBranch* out,
                            std::string* error) {
        if (out == nullptr) {
            if (error) *error = "output branch is null";
            return false;
        }
        if (output.uris.empty() && !output.packet_callback) {
            if (error) *error = "output has no uris and no packet_callback";
            return false;
        }

        auto encoder = codec::CreateVideoEncoder();
        if (!encoder) {
            if (error) *error = "CreateVideoEncoder failed";
            return false;
        }

        codec::VideoEncoderConfig encoder_config{};
        encoder_config.codec = output.codec;
        encoder_config.width = output.width == 0 ? input_stream_.width : output.width;
        encoder_config.height = output.height == 0 ? input_stream_.height : output.height;
        encoder_config.device_id = device_id;
        encoder_config.frame_rate = output.frame_rate > 0.0 ? output.frame_rate : input_frame_rate;
        encoder_config.bitrate_kbps = output.bitrate_kbps;
        encoder_config.gop = output.gop;
        if (output.input_queue_depth > 0) {
            encoder_config.input_queue_depth = output.input_queue_depth;
        }
        encoder_config.overflow_policy = output.overflow_policy;
        encoder_config.resize = output.resize;

        if (!encoder->Open(encoder_config)) {
            if (error) *error = "encoder Open failed";
            return false;
        }

        std::unique_ptr<Muxer> muxer;
        if (!output.uris.empty()) {
            muxer = CreateMuxer();
            if (!muxer) {
                encoder->Close();
                if (error) *error = "CreateMuxer failed";
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
                muxer->Close();
                encoder->Close();
                if (error) *error = "muxer Open failed";
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

        out->config = output;
        out->encoder = std::move(encoder);
        out->muxer = std::move(muxer);
        return true;
    }

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

        common::internal::AxImageAccess::CopyFrameMetadata(source, output.get());
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

    common::AxImage::Ptr AcquireOsdFrame(const common::ImageDescriptor& descriptor) {
        std::lock_guard<std::mutex> lock(osd_pool_mutex_);

        for (const auto& candidate : osd_pool_) {
            if (!candidate) {
                continue;
            }
            if (candidate.use_count() != 1) {
                continue;  // in use by encoder path
            }
            if (!SameDescriptor(candidate->descriptor(), descriptor)) {
                continue;
            }
            return candidate;
        }

        auto created = CreateOutputImage(descriptor);
        if (created) {
            osd_pool_.push_back(created);
        }
        return created;
    }

    bool CopyImageForOsd(const common::AxImage& source, common::AxImage* destination) {
        if (destination == nullptr) {
            return false;
        }
#if defined(AXSDK_PLATFORM_AXCL)
        // Avoid routing full-frame copies through IVPS on AXCL: it competes with preprocess CropResize/Csc and can
        // dramatically reduce FPS. Use runtime D2D memcpy instead.
        if (source.format() != destination->format() ||
            source.width() != destination->width() ||
            source.height() != destination->height()) {
            return false;
        }
        if (!common::internal::EnsureAxclThreadContext(config_.device_id)) {
            return false;
        }

        const auto fmt = source.format();
        const auto w = source.width();
        const auto h = source.height();
        const std::size_t plane_count = (fmt == common::PixelFormat::kNv12) ? 2U :
                                        ((fmt == common::PixelFormat::kRgb24 || fmt == common::PixelFormat::kBgr24) ? 1U : 0U);
        if (plane_count == 0 || w == 0 || h == 0) {
            return false;
        }

        for (std::size_t plane = 0; plane < plane_count; ++plane) {
            const auto src_phy = source.physical_address(plane);
            const auto dst_phy = destination->physical_address(plane);
            if (src_phy == 0 || dst_phy == 0) {
                return false;
            }

            const std::size_t rows = (fmt == common::PixelFormat::kNv12 && plane == 1) ? (h / 2U) : h;
            const std::size_t row_bytes =
                (fmt == common::PixelFormat::kNv12) ? static_cast<std::size_t>(w)
                                                    : static_cast<std::size_t>(w) * 3U;
            const std::size_t src_stride = source.stride(plane);
            const std::size_t dst_stride = destination->stride(plane);
            if (rows == 0 || row_bytes == 0 || src_stride < row_bytes || dst_stride < row_bytes) {
                return false;
            }

            const auto* src = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(src_phy));
            auto* dst = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(dst_phy));

            if (src_stride == dst_stride) {
                const std::size_t bytes = src_stride * rows;
                if (axclrtMemcpy(dst, src, bytes, AXCL_MEMCPY_DEVICE_TO_DEVICE) != AXCL_SUCC) {
                    return false;
                }
            } else {
                for (std::size_t r = 0; r < rows; ++r) {
                    if (axclrtMemcpy(dst + r * dst_stride, src + r * src_stride, row_bytes,
                                     AXCL_MEMCPY_DEVICE_TO_DEVICE) != AXCL_SUCC) {
                        return false;
                    }
                }
            }
        }

        return true;
#else
        return common::internal::CopyImage(source, destination);
#endif
    }

    bool ApplyOsdIfNeeded(const common::AxImage* source_frame, common::AxImage::Ptr* frame) {
        if (frame == nullptr || !*frame) {
            return false;
        }

        std::shared_ptr<const common::PreparedDrawCommands> osd_to_apply;
        {
            std::lock_guard<std::mutex> lock(osd_mutex_);
            ApplyPendingOsdUpdateLocked();
            if (!active_osd_) {
                return false;
            }

            osd_to_apply = active_osd_;
            if (active_osd_remaining_frames_ > 0) {
                --active_osd_remaining_frames_;
                if (active_osd_remaining_frames_ == 0) {
                    active_osd_.reset();
                }
            }
        }

        // If the encoder frame aliases the source frame that we publish to callbacks/GetLatestFrame,
        // avoid drawing in-place by materializing a CMM copy first. If upstream already provided a
        // dedicated encoder buffer (e.g. AX650 pool->CMM canonicalization), draw in-place to avoid
        // an extra full-frame copy.
        common::AxImage::Ptr target = *frame;
        if (source_frame != nullptr && target.get() == source_frame) {
            auto osd_frame = AcquireOsdFrame(target->descriptor());
            if (!osd_frame || !CopyImageForOsd(*target, osd_frame.get())) {
                std::fprintf(stderr, "pipeline osd: copy frame failed\n");
                return false;
            }
            common::internal::AxImageAccess::CopyFrameMetadata(*target, osd_frame.get());
            target = std::move(osd_frame);
        }

        if (!osd_to_apply->Apply(*target)) {
            std::fprintf(stderr, "pipeline osd: apply failed\n");
            return false;
        }

#if defined(AXSDK_PLATFORM_AXCL)
        // Avoid global axclrtSynchronizeDevice() here: it can stall VDEC/VENC and collapse FPS.
        // IVPS draw APIs are synchronous enough for the encoder submission path.
        (void)common::internal::EnsureAxclThreadContext(config_.device_id);
#endif

        *frame = std::move(target);
        return true;
    }

    void DemuxLoop() {
        if (!demuxer_ || !decoder_) {
            return;
        }

        std::uint64_t packet_count = 0;
        while (!demux_stop_) {
            codec::EncodedPacket packet;
            if (!demuxer_->ReadPacket(&packet)) {
                if (packet_count == 0) {
                    std::fprintf(stderr, "pipeline demux: ReadPacket returned false (no packets)\n");
                }
                break;
            }

            if (packet_count < 3) {
                std::fprintf(stderr, "pipeline demux: pkt=%llu bytes=%zu pts=%llu dur=%llu key=%d\n",
                             static_cast<unsigned long long>(packet_count),
                             packet.data.size(),
                             static_cast<unsigned long long>(packet.pts),
                             static_cast<unsigned long long>(packet.duration),
                             packet.key_frame ? 1 : 0);
            }
            ++packet_count;

            if (!decoder_->SubmitPacket(std::move(packet))) {
                std::fprintf(stderr, "pipeline demux: SubmitPacket failed\n");
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

            // Zero-copy fast path: hand out the same frame when caller didn't request any conversion.
            // This avoids per-frame CMM allocations and improves AXCL/NPU performance.
            if (SameDescriptor(output_descriptor, source_frame->descriptor())) {
                callback(std::move(source_frame));
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

    // 连续失败就关掉运动检测:静默地每帧失败是最糟的结果(功能没生效却毫无提示)。
    void NoteMotionFailure(const char* stage) {
        if (++motion_failures_ < 3) return;
        std::fprintf(stderr,
                     "pipeline: motion roi disabled after repeated %s failures (grid %ux%u)\n",
                     stage, motion_.cols(), motion_.rows());
        motion_.Clear();
        motion_frame_.reset();
        std::lock_guard<std::mutex> lock(roi_mutex_);
        motion_rois_.clear();
    }

    // 把整帧缩成很小的灰度网格做帧差。缩放走 IVPS(硬件),读回的只有几 KB。
    void UpdateMotionRoi(const common::AxImage& source) {
        if (!motion_.valid() || !EnsureFrameProcessor()) return;

        if (!motion_frame_) {
            common::ImageDescriptor desc{};
            desc.format = common::PixelFormat::kNv12;
            desc.width = motion_.cols();
            desc.height = motion_.rows();
            common::ImageAllocationOptions alloc{};
            alloc.memory_type = common::MemoryType::kCmm;
            alloc.token = "MotionRoi";
            motion_frame_ = common::AxImage::Create(desc, alloc);
            if (!motion_frame_) { NoteMotionFailure("alloc"); return; }
        }

        common::ImageProcessRequest request{};
        request.output_image = motion_frame_->descriptor();
        {
            // ImageProcessor 非线程安全,而 FrameCallbackLoop 也在用它,必须共用同一把锁
            std::lock_guard<std::mutex> lock(frame_processor_mutex_);
            if (!frame_processor_ || !frame_processor_->Process(source, request, *motion_frame_)) {
                NoteMotionFailure("ivps resize");
                return;
            }
        }

        const auto stride = motion_frame_->stride(0);
        const auto bytes = stride * motion_.rows();
        if (motion_host_.size() < bytes) motion_host_.resize(bytes);
#if defined(AXSDK_PLATFORM_AXCL)
        if (!common::internal::EnsureAxclThreadContext(config_.device_id)) return;
        if (axclrtMemcpy(motion_host_.data(),
                         reinterpret_cast<void*>(motion_frame_->physical_address(0)),
                         bytes, AXCL_MEMCPY_DEVICE_TO_HOST) != AXCL_SUCC) {
            NoteMotionFailure("readback");
            return;
        }
#else
        motion_frame_->InvalidateCache();
        const auto* src = motion_frame_->plane_data(0);
        if (src == nullptr) return;
        std::memcpy(motion_host_.data(), src, bytes);
#endif
        motion_failures_ = 0;
        motion_.Update(motion_host_.data(), stride);
        auto rects = motion_.rects();
        if (const char* dbg = std::getenv("AXVSDK_MOTION_DEBUG"); dbg != nullptr && dbg[0] == '1') {
            static std::atomic<std::uint64_t> n{0};
            if ((n.fetch_add(1) % 30) == 0) {
                std::uint64_t sum = 0;
                std::uint8_t lo = 255, hi = 0;
                for (std::uint32_t r = 0; r < motion_.rows(); ++r) {
                    for (std::uint32_t c = 0; c < motion_.cols(); ++c) {
                        const auto v = motion_host_[r * stride + c];
                        sum += v; lo = std::min(lo, v); hi = std::max(hi, v);
                    }
                }
                std::fprintf(stderr,
                             "motion: grid=%ux%u stride=%zu luma avg=%llu min=%u max=%u rects=%zu\n",
                             motion_.cols(), motion_.rows(), stride,
                             (unsigned long long)(sum / (motion_.cols() * motion_.rows())),
                             lo, hi, rects.size());
            }
        }
        {
            std::lock_guard<std::mutex> lock(roi_mutex_);
            motion_rois_ = std::move(rects);
        }
        PushCombinedRois();
    }

    void FanoutFrame(common::AxImage::Ptr frame) {
        if (!frame) {
            return;
        }

        // 给每个解码帧盖单调序号:消费者(NPU 侧异步推理、编码侧 OSD/QPMAP)凭它配对,
        // 知道手上的检测结果是基于哪一帧算出来的。
        const auto frame_seq = frame_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        frame->set_seq(frame_seq);

        // 检测结果滞后 = 当前帧号 - 这组结果所基于的帧号。异步推理下天然 > 0,
        // 是 ROI 编码等"按位置使用检测框"的功能做补偿的依据。
        // 必须在 branches_ 为空的提前返回之前算:仅解码+AI 模式同样需要这个指标。
        const auto det_seq = det_source_seq_.load(std::memory_order_relaxed);
        if (det_seq != 0 && frame_seq >= det_seq) {
            const auto lag = frame_seq - det_seq;
            det_lag_frames_.store(lag, std::memory_order_relaxed);
            auto prev_max = det_lag_frames_max_.load(std::memory_order_relaxed);
            while (lag > prev_max &&
                   !det_lag_frames_max_.compare_exchange_weak(prev_max, lag, std::memory_order_relaxed)) {
            }
        }

        if (config_.motion_roi.enable && motion_.valid()) {
            const auto interval = config_.motion_roi.detect_interval == 0
                                      ? 1U : config_.motion_roi.detect_interval;
            if ((frame_seq % interval) == 0) UpdateMotionRoi(*frame);
        }

        // Publish the raw decoded frame to callback/GetLatestFrame first. OSD is applied only on
        // the encoder path to avoid mutating frames consumed by the user/NPU thread.
        common::AxImage::Ptr source_frame = frame;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_source_frame_ = source_frame;
        }
        {
            std::lock_guard<std::mutex> lock(frame_callback_mutex_);
            if (frame_callback_) {
                pending_callback_source_frame_ = source_frame;
                frame_callback_cv_.notify_one();
            }
        }

        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            if (branches_.empty()) {
                return;
            }
        }

        common::AxImage::Ptr encoder_frame = source_frame;

#if defined(AXSDK_CHIP_AX650)
        // AX650 VENC expects CMM-backed frames. Passing VDEC pool frames directly can yield chroma corruption
        // for some streams. Canonicalize to a CMM image once per decoded frame and fan-out to all encoders.
        if (encoder_frame && encoder_frame->memory_type() == common::MemoryType::kPool) {
            common::ImageProcessRequest request{};
            request.output_image = encoder_frame->descriptor();
            auto copied = CreateFrameCopy(*encoder_frame, request);
            if (copied) {
                copied->set_seq(frame_seq);
                encoder_frame = std::move(copied);
            } else {
                std::fprintf(stderr, "pipeline: copy decoded frame to CMM failed\n");
            }
        }
#endif

        (void)ApplyOsdIfNeeded(source_frame.get(), &encoder_frame);

        {
            std::lock_guard<std::mutex> lock(branches_mutex_);
            for (auto& branch : branches_) {
                if (!branch.encoder->SubmitFrame(encoder_frame)) {
                    branch_submit_failures_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    PipelineConfig config_{};
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<codec::VideoDecoder> decoder_;
    mutable std::mutex branches_mutex_;
    std::vector<OutputBranch> branches_;
    codec::VideoStreamInfo input_stream_{};
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> demux_stop_{false};
    std::atomic<bool> callback_stop_{false};
    std::atomic<std::uint64_t> decoded_frames_{0};
    // 帧-检测配对:每个解码帧盖一个单调号;OSD 带上它来源帧的号,
    // 二者之差就是"这组框相对当前画面过期了几帧"。
    std::atomic<std::uint64_t> frame_seq_{0};
    std::atomic<std::uint64_t> det_source_seq_{0};

    mutable std::mutex roi_mutex_;
    std::vector<codec::QpRoiRect> det_rois_;
    std::vector<codec::QpRoiRect> motion_rois_;
    internal::MotionRoiDetector motion_;
    int motion_failures_{0};
    common::AxImage::Ptr motion_frame_;
    std::vector<std::uint8_t> motion_host_;
    std::atomic<std::uint64_t> det_lag_frames_{0};
    std::atomic<std::uint64_t> det_lag_frames_max_{0};
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

    std::mutex osd_pool_mutex_;
    std::vector<common::AxImage::Ptr> osd_pool_;
};

}  // namespace

std::unique_ptr<Pipeline> CreatePipeline() {
    return std::make_unique<AxPipeline>();
}

}  // namespace axvsdk::pipeline
