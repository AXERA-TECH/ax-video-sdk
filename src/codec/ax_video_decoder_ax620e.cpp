#include "ax_video_decoder_internal.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "ax_buffer_tool.h"
#include "ax_sys_api.h"
#include "ax_vdec_api.h"
#include "ax_vdec_type.h"

namespace axvsdk::codec::internal {

namespace {

constexpr AX_S32 kAxWaitMs = 100;
constexpr AX_U32 kStreamAlignment = 0x100;
constexpr AX_U32 kPoolMetaSize = 512;
constexpr AX_U32 kVdecWidthAlign = 16;
constexpr AX_U32 kFrameBufferCount = 8;
constexpr AX_U32 kFrameBufferCountH264 = 32;
// NOTE:
// 20e VDEC can be sensitive to being fed too fast from file sources (MP4 demux).
// We apply a conservative rate limit based on the packet duration (if available).
constexpr std::uint64_t kMinSubmitIntervalUs = 1000;  // 1ms
constexpr std::uint64_t kSubmitSpeedupFactor = 2;     // allow up to ~2x real-time by default

AX_U32 AlignUp(AX_U32 value, AX_U32 alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

AX_U32 ResolvePoolWidth(const Mp4VideoInfo& video_info) noexcept {
    return video_info.width == 0 ? AX_VDEC_MAX_WIDTH : AlignUp(video_info.width, 16U);
}

AX_U32 ResolvePoolHeight(const Mp4VideoInfo& video_info) noexcept {
    return video_info.height == 0 ? AX_VDEC_MAX_HEIGHT : AlignUp(video_info.height, 16U);
}

AX_U32 ResolveGroupWidth(const Mp4VideoInfo& video_info) noexcept {
    // pic size should be the coded/display size (not macroblock-aligned). Padding goes to u32FrameHeight.
    return video_info.width == 0 ? AX_VDEC_MAX_WIDTH : video_info.width;
}

AX_U32 ResolveGroupHeight(const Mp4VideoInfo& video_info) noexcept {
    return video_info.height == 0 ? AX_VDEC_MAX_HEIGHT : video_info.height;
}

std::mutex& GroupMutex() {
    static std::mutex mutex;
    return mutex;
}

bool* GroupSlots() {
    static bool slots[AX_VDEC_MAX_GRP_NUM]{};
    return slots;
}

AX_VDEC_GRP AcquireGroupId() {
    std::lock_guard<std::mutex> lock(GroupMutex());
    auto* slots = GroupSlots();
    for (AX_VDEC_GRP group = 0; group < AX_VDEC_MAX_GRP_NUM; ++group) {
        if (!slots[group]) {
            slots[group] = true;
            return group;
        }
    }
    return -1;
}

bool ReserveGroupId(AX_VDEC_GRP group) {
    if (group < 0 || group >= AX_VDEC_MAX_GRP_NUM) {
        return false;
    }
    std::lock_guard<std::mutex> lock(GroupMutex());
    auto* slots = GroupSlots();
    if (slots[group]) {
        return false;
    }
    slots[group] = true;
    return true;
}

void ReleaseGroupId(AX_VDEC_GRP group) {
    if (group < 0 || group >= AX_VDEC_MAX_GRP_NUM) {
        return;
    }

    std::lock_guard<std::mutex> lock(GroupMutex());
    GroupSlots()[group] = false;
}

AX_PAYLOAD_TYPE_E ToAxPayload(VideoCodecType codec) noexcept {
    switch (codec) {
    case VideoCodecType::kH264:
        return PT_H264;
    case VideoCodecType::kH265:
        return PT_H265;
    case VideoCodecType::kJpeg:
        return PT_JPEG;
    case VideoCodecType::kUnknown:
    default:
        return PT_BUTT;
    }
}

AX_U32 ResolveStreamBufferSize(const Mp4VideoInfo& video_info) noexcept {
    // 20e samples typically use a few MiB stream buffer. Too small can trigger unexpected stalls on file sources.
    const auto fallback = std::max<AX_U32>(video_info.width * video_info.height * 3U / 2U, 4U * 1024U * 1024U);
    return AlignUp(fallback, kStreamAlignment);
}

class Ax620eVideoDecoder final : public AxVideoDecoderBase {
public:
    ~Ax620eVideoDecoder() override {
        Close();
    }

protected:
    struct StreamBuffer {
        AX_U64 phy{0};
        AX_U8* vir{nullptr};
    };

    bool CreateFramePool(AX_PAYLOAD_TYPE_E payload, const Mp4VideoInfo& video_info, AX_U32 frame_buffer_count) {
        if (group_ < 0) {
            return false;
        }

        AX_POOL_CONFIG_T pool_config{};
        const AX_U32 pool_width = ResolvePoolWidth(video_info);
        const AX_U32 pool_height = ResolvePoolHeight(video_info);
        const AX_U32 frame_stride = AlignUp(pool_width, kVdecWidthAlign);
        const AX_U32 frame_size = AX_VDEC_GetPicBufferSize(frame_stride, pool_height, payload);
        if (frame_size == 0) {
            return false;
        }

        pool_config.MetaSize = kPoolMetaSize;
        pool_config.BlkSize = frame_size;
        pool_config.BlkCnt = frame_buffer_count;
        pool_config.CacheMode = AX_POOL_CACHE_MODE_NONCACHE;
        std::snprintf(reinterpret_cast<char*>(pool_config.PartitionName), AX_MAX_PARTITION_NAME_LEN, "anonymous");

        frame_pool_ = AX_POOL_CreatePool(&pool_config);
        if (frame_pool_ == AX_INVALID_POOLID) {
            frame_pool_ = AX_INVALID_POOLID;
            return false;
        }

        const auto attach_ret = AX_VDEC_AttachPool(group_, frame_pool_);
        if (attach_ret != AX_SUCCESS) {
            (void)AX_POOL_DestroyPool(frame_pool_);
            frame_pool_ = AX_INVALID_POOLID;
            return false;
        }

        frame_pool_attached_ = true;
        return true;
    }

    bool CreateBackend(const Mp4VideoInfo& video_info) override {
        if (video_info.codec == VideoCodecType::kH265) {
            std::fprintf(stderr, "ax620e CreateBackend: H.265 decode is not supported on this platform\n");
            return false;
        }

        const auto payload = ToAxPayload(video_info.codec);
        if (payload == PT_BUTT) {
            std::fprintf(stderr, "ax620e CreateBackend: unsupported payload for codec=%d\n",
                         static_cast<int>(video_info.codec));
            return false;
        }

        AX_VDEC_GRP_ATTR_T group_attr{};
        const AX_U32 group_width = ResolveGroupWidth(video_info);
        const AX_U32 group_height = ResolveGroupHeight(video_info);
        const AX_U32 frame_height = video_info.height == 0 ? AX_VDEC_MAX_HEIGHT : AlignUp(video_info.height, 16U);
        group_attr.enCodecType = payload;
        // Use FRAME mode so VDEC can honor per-access-unit PTS for display-order output (B-frame reorder).
        group_attr.enInputMode = AX_VDEC_INPUT_MODE_FRAME;
        // We use AX_VDEC_GetFrame to fetch decoded frames; keep VDEC in unlink mode (same as MSP samples).
        group_attr.enLinkMode = AX_UNLINK_MODE;
        // When enOutOrder=DISP, correct PTS becomes important for B-frame reorder (MP4 H.264 with B-frames).
        group_attr.enOutOrder = AX_VDEC_OUTPUT_ORDER_DISP;
        group_attr.u32PicWidth = group_width;
        group_attr.u32PicHeight = group_height;
        group_attr.u32FrameHeight = frame_height;
        group_attr.u32StreamBufSize = ResolveStreamBufferSize(video_info);
        group_attr.enVdecVbSource = AX_POOL_SOURCE_USER;
        group_attr.u32FrameBufCnt = video_info.codec == VideoCodecType::kH264 ? kFrameBufferCountH264 : kFrameBufferCount;
        group_attr.s32DestroyTimeout = 0;

        // VDEC group ids are global across processes. If another process has already created a group id,
        // AX_VDEC_CreateGrp returns AX_ERR_VDEC_EXIST. Probe for a free id by trying 0..MAX.
        AX_S32 create_ret = AX_ERR_VDEC_NO_AVAILABLE_GRP;
        for (AX_VDEC_GRP candidate = 0; candidate < AX_VDEC_MAX_GRP_NUM; ++candidate) {
            if (!ReserveGroupId(candidate)) {
                continue;
            }

            AX_MOD_INFO_T mod_info{};
            mod_info.enModId = AX_ID_VDEC;
            mod_info.s32GrpId = candidate;
            mod_info.s32ChnId = 0;
            (void)AX_SYS_MemSetConfig(&mod_info, reinterpret_cast<AX_S8*>(const_cast<char*>("VDEC")));

            create_ret = AX_VDEC_CreateGrp(candidate, &group_attr);
            if (create_ret == AX_SUCCESS) {
                group_ = candidate;
                break;
            }

            if (create_ret == AX_ERR_VDEC_EXIST) {
                ReleaseGroupId(candidate);
                continue;
            }

            std::fprintf(stderr,
                         "ax620e CreateBackend: AX_VDEC_CreateGrp grp=%d codec=%d width=%u height=%u frame_h=%u "
                         "stream_buf=%u frame_buf_cnt=%u vb_src=%d ret=0x%x\n",
                         candidate, static_cast<int>(payload), group_attr.u32PicWidth, group_attr.u32PicHeight,
                         group_attr.u32FrameHeight, group_attr.u32StreamBufSize, group_attr.u32FrameBufCnt,
                         static_cast<int>(group_attr.enVdecVbSource), create_ret);
            ReleaseGroupId(candidate);
            group_ = -1;
            return false;
        }
        if (group_ < 0 || create_ret != AX_SUCCESS) {
            std::fprintf(stderr, "ax620e CreateBackend: no available vdec group (ret=0x%x)\n", create_ret);
            return false;
        }

        if (!CreateFramePool(payload, video_info, group_attr.u32FrameBufCnt)) {
            std::fprintf(stderr, "ax620e CreateBackend: failed to create/attach frame pool grp=%d\n", group_);
            DestroyBackend();
            return false;
        }

        stream_buffer_size_ = group_attr.u32StreamBufSize;
        // IMPORTANT:
        // 20e samples pace AX_VDEC_SendStream and reuse a CMM stream buffer. Empirically, if we overwrite the same
        // buffer too quickly (offline MP4), VDEC may read corrupted data and stop early.
        // Keep this small to reduce CMM usage; stability is primarily ensured by rate limiting + backpressure.
        const AX_U32 stream_buf_count = 8;
        stream_buffers_.clear();
        stream_buffers_.reserve(stream_buf_count);
        stream_buffer_index_ = 0;
        submitted_frames_.store(0, std::memory_order_relaxed);
        received_frames_.store(0, std::memory_order_relaxed);
        next_submit_due_ = {};
        next_pts_us_ = 0;
        empty_polls_ = 0;
        last_status_report_ = {};

        const auto* token = reinterpret_cast<const AX_S8*>("VdecInputStream");
        for (AX_U32 i = 0; i < stream_buf_count; ++i) {
            StreamBuffer buf{};
            const auto mem_ret =
                AX_SYS_MemAlloc(&buf.phy, reinterpret_cast<AX_VOID**>(&buf.vir), stream_buffer_size_, kStreamAlignment,
                                token);
            if (mem_ret != AX_SUCCESS || buf.phy == 0 || buf.vir == nullptr) {
                std::fprintf(stderr, "ax620e CreateBackend: AX_SYS_MemAlloc[%u/%u] size=%u ret=0x%x phy=0x%llx\n",
                             i, stream_buf_count, stream_buffer_size_, mem_ret,
                             static_cast<unsigned long long>(buf.phy));
                DestroyBackend();
                return false;
            }
            stream_buffers_.push_back(buf);
        }

        return true;
    }

    void DestroyBackend() noexcept override {
        StopBackend();

        for (auto& buf : stream_buffers_) {
            if (buf.phy != 0 && buf.vir != nullptr) {
                (void)AX_SYS_MemFree(buf.phy, buf.vir);
            }
            buf.phy = 0;
            buf.vir = nullptr;
        }
        stream_buffers_.clear();
        stream_buffer_index_ = 0;
        stream_buffer_size_ = 0;
        submitted_frames_.store(0, std::memory_order_relaxed);
        received_frames_.store(0, std::memory_order_relaxed);
        next_submit_due_ = {};
        next_pts_us_ = 0;
        empty_polls_ = 0;
        last_status_report_ = {};

        if (group_ >= 0) {
            if (frame_pool_attached_) {
                (void)AX_VDEC_DetachPool(group_);
                frame_pool_attached_ = false;
            }

            for (int retry = 0; retry < 10; ++retry) {
                const auto ret = AX_VDEC_DestroyGrp(group_);
                if (ret == AX_SUCCESS) {
                    break;
                }
                if (ret != AX_ERR_VDEC_BUSY) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ReleaseGroupId(group_);
            group_ = -1;
        }

        if (frame_pool_ != AX_INVALID_POOLID) {
            (void)AX_POOL_DestroyPool(frame_pool_);
            frame_pool_ = AX_INVALID_POOLID;
        }
    }

    bool StartBackend() override {
        if (group_ < 0) {
            return false;
        }

        AX_VDEC_GRP_PARAM_T group_param{};
        group_param.enVdecMode = VIDEO_DEC_MODE_IPB;
        (void)AX_VDEC_SetGrpParam(group_, &group_param);
        // Playback mode enables VDEC display-order output for streams with B-frames (MP4 H.264, etc.).
        // PREVIEW mode may output frames in decode order on some 20e firmwares, causing visible "jitter"/rewind.
        if (AX_VDEC_SetDisplayMode(group_, AX_VDEC_DISPLAY_MODE_PLAYBACK) != AX_SUCCESS) {
            return false;
        }

        AX_VDEC_RECV_PIC_PARAM_T recv_param{};
        recv_param.s32RecvPicNum = -1;
        started_ = AX_VDEC_StartRecvStream(group_, &recv_param) == AX_SUCCESS;
        return started_;
    }

    void StopBackend() noexcept override {
        if (group_ >= 0 && started_) {
            (void)AX_VDEC_StopRecvStream(group_);
        }
        started_ = false;
    }

    bool SendEncodedPacket(const EncodedPacket& packet) override {
        if (group_ < 0 || packet.data.empty() || stream_buffers_.empty()) {
            return false;
        }

        if (packet.duration > 0 && kSubmitSpeedupFactor > 0) {
            const auto interval_us =
                std::max<std::uint64_t>(kMinSubmitIntervalUs, packet.duration / kSubmitSpeedupFactor);
            const auto interval = std::chrono::microseconds(interval_us);
            const auto now = std::chrono::steady_clock::now();
            if (next_submit_due_.time_since_epoch().count() == 0) {
                next_submit_due_ = now;
            }
            if (now < next_submit_due_) {
                std::this_thread::sleep_for(next_submit_due_ - now);
            }
            const auto now2 = std::chrono::steady_clock::now();
            next_submit_due_ = (next_submit_due_ > now2 ? next_submit_due_ : now2) + interval;
        }

        if (packet.data.size() > stream_buffer_size_) {
            std::fprintf(stderr, "ax620e SendEncodedPacket: packet too large: %zu > stream_buf=%u\n",
                         packet.data.size(), stream_buffer_size_);
            return false;
        }

        auto& buf = stream_buffers_[stream_buffer_index_ % stream_buffers_.size()];
        std::memcpy(buf.vir, packet.data.data(), packet.data.size());
        // Ensure VDEC can see the latest packet bytes (buffer may be cached depending on platform config).
        (void)AX_SYS_MflushCache(buf.phy, buf.vir, static_cast<AX_U32>(packet.data.size()));

        AX_VDEC_STREAM_T stream{};
        // IMPORTANT:
        // Some 20e firmwares behave poorly when input PTS is non-monotonic (MP4 with B-frames has out-of-order PTS in
        // decode order). Follow MSP samples: always feed a monotonic PTS in microseconds.
        stream.u64PTS = next_pts_us_;
        stream.bEndOfFrame = AX_TRUE;
        stream.bEndOfStream = AX_FALSE;
        stream.bSkipDisplay = AX_FALSE;
        stream.u32StreamPackLen = static_cast<AX_U32>(packet.data.size());
        stream.pu8Addr = buf.vir;
        // Match 20e samples: use virtual address only.
        // Some firmware versions behave incorrectly if u64PhyAddr is provided here.
        stream.u64PhyAddr = 0;

        while (!stop_requested()) {
            const auto ret = AX_VDEC_SendStream(group_, &stream, kAxWaitMs);
            if (ret == AX_SUCCESS) {
                break;
            }
            if (ret != AX_ERR_VDEC_BUF_FULL && ret != AX_ERR_VDEC_QUEUE_FULL) {
                std::fprintf(stderr,
                             "ax620e SendEncodedPacket: AX_VDEC_SendStream grp=%d ret=0x%x pts=%llu len=%u eof=%d eos=%d\n",
                             group_, ret, static_cast<unsigned long long>(stream.u64PTS), stream.u32StreamPackLen,
                             stream.bEndOfFrame, stream.bEndOfStream);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        stream_buffer_index_ = (stream_buffer_index_ + 1) % stream_buffers_.size();
        // Increment PTS cursor by the provided duration (already in us), with a conservative fallback.
        next_pts_us_ += packet.duration > 0 ? packet.duration : 33333ULL;
        submitted_frames_.fetch_add(1, std::memory_order_relaxed);

        // Light-touch status probe to catch decode errors early on 20e.
        if ((submitted_frames_.load(std::memory_order_relaxed) % 30) == 0) {
            AX_VDEC_GRP_STATUS_T st{};
            if (AX_VDEC_QueryStatus(group_, &st) == AX_SUCCESS) {
                const auto& e = st.stVdecDecErr;
                if (e.s32FormatErr || e.s32PicSizeErrSet || e.s32StreamUnsprt || e.s32PackErr || e.s32RefErrSet ||
                    e.s32PicBufSizeErrSet || e.s32StreamSizeOver || e.s32VdecStreamNotRelease) {
                    std::fprintf(stderr,
                                 "ax620e vdec status grp=%d recv=%u dec=%u left_bytes=%u left_frames=%u left_pics=%u "
                                 "fifo_full=%d err{fmt=%d size=%d unsup=%d pack=%d ref=%d picbuf=%d over=%d notrel=%d}\n",
                                 group_, st.u32RecvStreamFrames, st.u32DecodeStreamFrames, st.u32LeftStreamBytes,
                                 st.u32LeftStreamFrames, st.u32LeftPics, st.bInputFifoIsFull, e.s32FormatErr,
                                 e.s32PicSizeErrSet, e.s32StreamUnsprt, e.s32PackErr, e.s32RefErrSet,
                                 e.s32PicBufSizeErrSet, e.s32StreamSizeOver, e.s32VdecStreamNotRelease);
                }
            }
        }

        // Backpressure: do not allow the submitter to outrun the decoder indefinitely.
        // This makes "offline" MP4 ingestion stable on 20e even when realtime_playback=false.
        while (!stop_requested()) {
            const auto submitted = submitted_frames_.load(std::memory_order_relaxed);
            const auto received = received_frames_.load(std::memory_order_relaxed);
            const auto inflight = submitted > received ? (submitted - received) : 0;
            if (inflight < stream_buffers_.size()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return !stop_requested();
    }

    bool SendEndOfStream() override {
        if (group_ < 0) {
            return false;
        }

        // Give VDEC a chance to drain queued access units before signaling EOS.
        // Without this, some 20e firmware versions may stop early for file sources that are fed too fast.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stop_requested()) {
            const auto submitted = submitted_frames_.load(std::memory_order_relaxed);
            const auto received = received_frames_.load(std::memory_order_relaxed);
            if (received >= submitted) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        AX_VDEC_STREAM_T stream{};
        stream.bEndOfFrame = AX_TRUE;
        stream.bEndOfStream = AX_TRUE;
        // Matches 20e samples: EOS is sent with no payload.
        stream.pu8Addr = nullptr;
        stream.u32StreamPackLen = 0;
        stream.u64PhyAddr = 0;
        const auto ret = AX_VDEC_SendStream(group_, &stream, kAxWaitMs);
        if (ret != AX_SUCCESS && ret != AX_ERR_VDEC_FLOW_END) {
            std::fprintf(stderr, "ax620e SendEndOfStream: AX_VDEC_SendStream grp=%d ret=0x%x\n", group_, ret);
        }
        return ret == AX_SUCCESS || ret == AX_ERR_VDEC_FLOW_END;
    }

    bool ReceiveDecodedFrame(AX_VIDEO_FRAME_INFO_T* frame_info, bool* flow_end) override {
        if (frame_info == nullptr || flow_end == nullptr || group_ < 0) {
            return false;
        }

        *flow_end = false;
        const auto ret = AX_VDEC_GetFrame(group_, frame_info, kAxWaitMs);
        if (ret == AX_SUCCESS) {
            received_frames_.fetch_add(1, std::memory_order_relaxed);
            empty_polls_ = 0;
            return true;
        }
        if (ret == AX_ERR_VDEC_FLOW_END) {
            *flow_end = true;
            return false;
        }
        if (ret == AX_ERR_VDEC_QUEUE_EMPTY || ret == AX_ERR_VDEC_TIMED_OUT || ret == AX_ERR_VDEC_STRM_ERROR) {
            // If decode stalls, periodically dump VDEC status for debugging.
            ++empty_polls_;
            const auto now = std::chrono::steady_clock::now();
            const auto submitted = submitted_frames_.load(std::memory_order_relaxed);
            const auto received = received_frames_.load(std::memory_order_relaxed);
            const auto inflight = submitted > received ? (submitted - received) : 0ULL;

            // Avoid log spam: only report when we seem to be behind (still have inflight packets) and we've been
            // polling empty for a while.
            if (inflight > 0 && empty_polls_ >= 10 &&
                (last_status_report_.time_since_epoch().count() == 0 ||
                 now - last_status_report_ > std::chrono::seconds(1))) {
                last_status_report_ = now;
                AX_VDEC_GRP_STATUS_T st{};
                if (AX_VDEC_QueryStatus(group_, &st) == AX_SUCCESS) {
                    const auto& e = st.stVdecDecErr;
                    std::fprintf(stderr,
                                 "ax620e vdec poll grp=%d ret=0x%x polls=%llu recv=%u dec=%u left_bytes=%u left_frames=%u "
                                 "left_pics=%u fifo_full=%d err{fmt=%d size=%d unsup=%d pack=%d ref=%d picbuf=%d over=%d notrel=%d}\n",
                                 group_, ret, static_cast<unsigned long long>(empty_polls_), st.u32RecvStreamFrames,
                                 st.u32DecodeStreamFrames, st.u32LeftStreamBytes, st.u32LeftStreamFrames, st.u32LeftPics,
                                 st.bInputFifoIsFull, e.s32FormatErr, e.s32PicSizeErrSet, e.s32StreamUnsprt, e.s32PackErr,
                                 e.s32RefErrSet, e.s32PicBufSizeErrSet, e.s32StreamSizeOver, e.s32VdecStreamNotRelease);
                }
            }
            return false;
        }

        if (ret != AX_ERR_VDEC_NOT_PERM && ret != AX_ERR_VDEC_UNEXIST) {
            std::fprintf(stderr, "ax620e ReceiveDecodedFrame: AX_VDEC_GetFrame grp=%d ret=0x%x\n", group_, ret);
        }
        return false;
    }

    void ReleaseDecodedFrame(const AX_VIDEO_FRAME_INFO_T& frame_info) noexcept override {
        if (group_ >= 0) {
            (void)AX_VDEC_ReleaseFrame(group_, &frame_info);
        }
    }

private:
    AX_VDEC_GRP group_{-1};
    AX_POOL frame_pool_{AX_INVALID_POOLID};
    bool frame_pool_attached_{false};
    std::vector<StreamBuffer> stream_buffers_{};
    std::size_t stream_buffer_index_{0};
    AX_U32 stream_buffer_size_{0};
    bool started_{false};
    std::atomic<std::uint64_t> submitted_frames_{0};
    std::atomic<std::uint64_t> received_frames_{0};
    std::chrono::steady_clock::time_point next_submit_due_{};
    std::uint64_t next_pts_us_{0};
    std::uint64_t empty_polls_{0};
    std::chrono::steady_clock::time_point last_status_report_{};
};

}  // namespace

std::unique_ptr<VideoDecoder> CreatePlatformVideoDecoder() {
    return std::make_unique<Ax620eVideoDecoder>();
}

}  // namespace axvsdk::codec::internal
