#include "ax_video_decoder_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

#include "ax_buffer_tool.h"
#include "ax_sys_api.h"
#include "ax_vdec_api.h"
#include "ax_vdec_type.h"

namespace axvsdk::codec::internal {

namespace {

constexpr AX_S32 kAxWaitMs = 100;
constexpr AX_VDEC_CHN kOutputChannel = 0;
constexpr AX_U32 kFrameBufferCount = 8;
constexpr AX_U32 kFrameBufferCountH264 = 32;
constexpr AX_U32 kOutputFifoDepth = 5;

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
    const auto fallback = std::max<AX_U32>(video_info.width * video_info.height * 3U / 2U, 1024U * 1024U);
    return fallback;
}

AX_U32 ResolveFrameStride(AX_U32 width) noexcept {
    return AX_COMM_ALIGN(width * 8U, 256U * 8U) / 8U;
}

AX_U32 AlignToMacroblock(AX_U32 value) noexcept {
    return AX_COMM_ALIGN(value, 16U);
}

class Ax650VideoDecoder final : public AxVideoDecoderBase {
public:
    ~Ax650VideoDecoder() override {
        Close();
    }

protected:
    bool CreateBackend(const Mp4VideoInfo& video_info) override {
        const auto payload = ToAxPayload(video_info.codec);
        if (payload == PT_BUTT) {
            return false;
        }

        const AX_U32 aligned_width = AlignToMacroblock(video_info.width);
        const AX_U32 aligned_height = AlignToMacroblock(video_info.height);
        const AX_U32 frame_buffer_count = video_info.codec == VideoCodecType::kH264 ? kFrameBufferCountH264 : kFrameBufferCount;

        AX_VDEC_GRP_ATTR_T group_attr{};
        group_attr.enCodecType = payload;
        group_attr.enInputMode = AX_VDEC_INPUT_MODE_FRAME;
        group_attr.u32MaxPicWidth = aligned_width;
        group_attr.u32MaxPicHeight = aligned_height;
        group_attr.u32StreamBufSize = ResolveStreamBufferSize(video_info);
        group_attr.bSdkAutoFramePool = AX_TRUE;
        group_attr.bSkipSdkStreamPool = AX_FALSE;
        // Match MSP sample defaults: let VDEC decide reference buffer count.
        group_attr.u32RefNum = 0;

        if (AX_VDEC_CreateGrpEx(&group_, &group_attr) != AX_SUCCESS) {
            group_ = -1;
            return false;
        }

        AX_VDEC_CHN_ATTR_T channel_attr{};
        channel_attr.u32PicWidth = aligned_width;
        channel_attr.u32PicHeight = aligned_height;
        channel_attr.u32FrameStride = ResolveFrameStride(aligned_width);
        channel_attr.u32OutputFifoDepth = kOutputFifoDepth;
        channel_attr.u32FrameBufCnt = frame_buffer_count;
        channel_attr.enOutputMode = AX_VDEC_OUTPUT_ORIGINAL;
        channel_attr.enImgFormat = AX_FORMAT_YUV420_SEMIPLANAR;
        channel_attr.stCompressInfo.enCompressMode = AX_COMPRESS_MODE_NONE;
        channel_attr.u32FrameBufSize = AX_VDEC_GetPicBufferSize(channel_attr.u32FrameStride, channel_attr.u32PicHeight,
                                                                channel_attr.enImgFormat,
                                                                &channel_attr.stCompressInfo, payload);

        if (AX_VDEC_SetChnAttr(group_, kOutputChannel, &channel_attr) != AX_SUCCESS) {
            DestroyBackend();
            return false;
        }

        if (AX_VDEC_EnableChn(group_, kOutputChannel) != AX_SUCCESS) {
            DestroyBackend();
            return false;
        }

        stream_buffer_size_ = group_attr.u32StreamBufSize;
        const auto* token = reinterpret_cast<const AX_S8*>("AXSDK_VDEC650");
        if (AX_SYS_MemAlloc(&stream_phy_addr_, reinterpret_cast<AX_VOID**>(&stream_vir_addr_), stream_buffer_size_,
                            0x1000, token) != AX_SUCCESS) {
            DestroyBackend();
            return false;
        }

        return true;
    }

    void DestroyBackend() noexcept override {
        StopBackend();

        if (stream_phy_addr_ != 0 && stream_vir_addr_ != nullptr) {
            (void)AX_SYS_MemFree(stream_phy_addr_, stream_vir_addr_);
        }
        stream_phy_addr_ = 0;
        stream_vir_addr_ = nullptr;
        stream_buffer_size_ = 0;

        if (group_ >= 0) {
            (void)AX_VDEC_DisableChn(group_, kOutputChannel);
            (void)AX_VDEC_DestroyGrp(group_);
            group_ = -1;
        }
    }

    bool StartBackend() override {
        if (group_ < 0) {
            return false;
        }

        AX_VDEC_GRP_PARAM_T group_param{};
        group_param.stVdecVideoParam.enOutputOrder = AX_VDEC_OUTPUT_ORDER_DISP;
        group_param.stVdecVideoParam.enVdecMode = VIDEO_DEC_MODE_IPB;
        group_param.f32SrcFrmRate = static_cast<AX_F32>(video_info().fps > 0.0 ? video_info().fps : 30.0);
        (void)AX_VDEC_SetGrpParam(group_, &group_param);
        if (AX_VDEC_SetDisplayMode(group_, AX_VDEC_DISPLAY_MODE_PREVIEW) != AX_SUCCESS) {
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
        if (group_ < 0 || stream_vir_addr_ == nullptr || packet.data.empty()) {
            return false;
        }

        if (packet.data.size() > stream_buffer_size_) {
            std::fprintf(stderr, "ax650 SendEncodedPacket: packet too large: %zu > stream_buf=%u\n",
                         packet.data.size(), stream_buffer_size_);
            return false;
        }

        // FRAME mode expects one access unit per SendStream call with bEndOfFrame=AX_TRUE.
        std::memcpy(stream_vir_addr_, packet.data.data(), packet.data.size());
        // Ensure VDEC can see the latest bytes. Some firmwares/platform configs return cached CMM here.
        (void)AX_SYS_MflushCache(stream_phy_addr_, stream_vir_addr_, static_cast<AX_U32>(packet.data.size()));

        AX_VDEC_STREAM_T stream{};
        stream.u64PTS = packet.pts;
        stream.bEndOfFrame = AX_TRUE;
        stream.bEndOfStream = AX_FALSE;
        stream.bSkipDisplay = AX_FALSE;
        stream.u32StreamPackLen = static_cast<AX_U32>(packet.data.size());
        stream.pu8Addr = stream_vir_addr_;
        // Follow MSP samples: use virtual address only.
        // Some firmware versions behave incorrectly if u64PhyAddr is provided here.
        stream.u64PhyAddr = 0;

        while (!stop_requested()) {
            const auto ret = AX_VDEC_SendStream(group_, &stream, kAxWaitMs);
            if (ret == AX_SUCCESS) {
                return true;
            }
            if (ret != AX_ERR_VDEC_BUF_FULL && ret != AX_ERR_VDEC_QUEUE_FULL) {
                std::fprintf(stderr,
                             "ax650 SendEncodedPacket: AX_VDEC_SendStream grp=%d ret=0x%x pts=%llu len=%u eof=%d\n",
                             group_, ret, static_cast<unsigned long long>(stream.u64PTS), stream.u32StreamPackLen,
                             stream.bEndOfFrame);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return false;
    }

    bool SendEndOfStream() override {
        if (group_ < 0) {
            return false;
        }

        AX_VDEC_STREAM_T stream{};
        stream.bEndOfFrame = AX_TRUE;
        stream.bEndOfStream = AX_TRUE;
        const auto ret = AX_VDEC_SendStream(group_, &stream, kAxWaitMs);
        return ret == AX_SUCCESS || ret == AX_ERR_VDEC_FLOW_END;
    }

    bool ReceiveDecodedFrame(AX_VIDEO_FRAME_INFO_T* frame_info, bool* flow_end) override {
        if (frame_info == nullptr || flow_end == nullptr || group_ < 0) {
            return false;
        }

        *flow_end = false;
        const auto ret = AX_VDEC_GetChnFrame(group_, kOutputChannel, frame_info, kAxWaitMs);
        if (ret == AX_SUCCESS) {
            return true;
        }
        if (ret == AX_ERR_VDEC_FLOW_END) {
            *flow_end = true;
        }
        return false;
    }

    void ReleaseDecodedFrame(const AX_VIDEO_FRAME_INFO_T& frame_info) noexcept override {
        if (group_ >= 0) {
            (void)AX_VDEC_ReleaseChnFrame(group_, kOutputChannel, &frame_info);
        }
    }

private:
    AX_VDEC_GRP group_{-1};
    AX_U64 stream_phy_addr_{0};
    AX_U8* stream_vir_addr_{nullptr};
    AX_U32 stream_buffer_size_{0};
    bool started_{false};
};

}  // namespace

std::unique_ptr<VideoDecoder> CreatePlatformVideoDecoder() {
    return std::make_unique<Ax650VideoDecoder>();
}

}  // namespace axvsdk::codec::internal
