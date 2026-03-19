#include "ax_video_decoder_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "ax_buffer_tool.h"
#include "axcl_vdec.h"
#include "ax_system_internal.h"

namespace axvsdk::codec::internal {

namespace {

constexpr AX_S32 kAxWaitMs = 100;
constexpr AX_VDEC_CHN kOutputChannel = 0;
constexpr AX_U32 kFrameBufferCount = 8;

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
    return std::max<AX_U32>(video_info.width * video_info.height * 2U, 1024U * 1024U);
}

AX_U32 ResolveFrameStride(AX_U32 width) noexcept {
    return ((width + 255U) / 256U) * 256U;
}

class AxclVideoDecoder final : public AxVideoDecoderBase {
public:
    ~AxclVideoDecoder() override {
        Close();
    }

protected:
    bool CreateBackend(const Mp4VideoInfo& video_info) override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        const auto payload = ToAxPayload(video_info.codec);
        if (payload == PT_BUTT) {
            return false;
        }

        AX_VDEC_GRP_ATTR_T group_attr{};
        group_attr.enCodecType = payload;
        group_attr.enInputMode = AX_VDEC_INPUT_MODE_FRAME;
        group_attr.u32MaxPicWidth = video_info.width;
        group_attr.u32MaxPicHeight = video_info.height;
        group_attr.u32StreamBufSize = ResolveStreamBufferSize(video_info);
        group_attr.bSdkAutoFramePool = AX_TRUE;

        const auto create_ret = AXCL_VDEC_CreateGrpEx(&group_, &group_attr);
        if (create_ret != AX_SUCCESS) {
            group_ = -1;
            return false;
        }

        AX_VDEC_GRP_PARAM_T group_param{};
        group_param.stVdecVideoParam.enOutputOrder = AX_VDEC_OUTPUT_ORDER_DISP;
        group_param.stVdecVideoParam.enVdecMode = VIDEO_DEC_MODE_IPB;
        const auto set_grp_ret = AXCL_VDEC_SetGrpParam(group_, &group_param);
        if (set_grp_ret != AX_SUCCESS) {
            DestroyBackend();
            return false;
        }

        AX_VDEC_CHN_ATTR_T channel_attr{};
        channel_attr.u32PicWidth = video_info.width;
        channel_attr.u32PicHeight = video_info.height;
        channel_attr.u32FrameStride = ResolveFrameStride(video_info.width);
        channel_attr.u32OutputFifoDepth = 3;
        channel_attr.u32FrameBufCnt = kFrameBufferCount;
        channel_attr.enOutputMode = AX_VDEC_OUTPUT_ORIGINAL;
        channel_attr.enImgFormat = AX_FORMAT_YUV420_SEMIPLANAR;
        channel_attr.stCompressInfo.enCompressMode = AX_COMPRESS_MODE_NONE;
        channel_attr.u32FrameBufSize = AX_VDEC_GetPicBufferSize(channel_attr.u32FrameStride, channel_attr.u32PicHeight,
                                                                channel_attr.enImgFormat,
                                                                &channel_attr.stCompressInfo, payload);

        const auto set_chn_ret = AXCL_VDEC_SetChnAttr(group_, kOutputChannel, &channel_attr);
        if (set_chn_ret != AX_SUCCESS) {
            DestroyBackend();
            return false;
        }

        const auto enable_ret = AXCL_VDEC_EnableChn(group_, kOutputChannel);
        if (enable_ret != AX_SUCCESS) {
            DestroyBackend();
            return false;
        }

        return true;
    }

    void DestroyBackend() noexcept override {
        StopBackend();
        if (group_ >= 0) {
            (void)AXCL_VDEC_DisableChn(group_, kOutputChannel);
            (void)AXCL_VDEC_DestroyGrp(group_);
            group_ = -1;
        }
    }

    bool StartBackend() override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (group_ < 0) {
            return false;
        }

        if (AXCL_VDEC_SetDisplayMode(group_, AX_VDEC_DISPLAY_MODE_PREVIEW) != AX_SUCCESS) {
            return false;
        }

        AX_VDEC_RECV_PIC_PARAM_T recv_param{};
        recv_param.s32RecvPicNum = -1;
        started_ = AXCL_VDEC_StartRecvStream(group_, &recv_param) == AX_SUCCESS;
        return started_;
    }

    void StopBackend() noexcept override {
        if (group_ >= 0 && started_) {
            (void)AXCL_VDEC_StopRecvStream(group_);
            (void)AXCL_VDEC_ResetGrp(group_);
        }
        started_ = false;
    }

    bool SendEncodedPacket(const EncodedPacket& packet) override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (group_ < 0 || packet.data.empty()) {
            return false;
        }

        AX_VDEC_STREAM_T stream{};
        stream.u64PTS = packet.pts;
        stream.bEndOfFrame = AX_TRUE;
        stream.bEndOfStream = AX_FALSE;
        stream.bSkipDisplay = AX_FALSE;
        stream.u32StreamPackLen = static_cast<AX_U32>(packet.data.size());
        stream.pu8Addr = const_cast<AX_U8*>(packet.data.data());

        while (!stop_requested()) {
            const auto ret = AXCL_VDEC_SendStream(group_, &stream, kAxWaitMs);
            if (ret == AX_SUCCESS) {
                return true;
            }
            if (ret != AX_ERR_VDEC_BUF_FULL && ret != AX_ERR_VDEC_QUEUE_FULL) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return false;
    }

    bool SendEndOfStream() override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (group_ < 0) {
            return false;
        }

        AX_VDEC_STREAM_T stream{};
        stream.bEndOfFrame = AX_TRUE;
        stream.bEndOfStream = AX_TRUE;
        const auto ret = AXCL_VDEC_SendStream(group_, &stream, kAxWaitMs);
        return ret == AX_SUCCESS || ret == AX_ERR_VDEC_FLOW_END;
    }

    bool ReceiveDecodedFrame(AX_VIDEO_FRAME_INFO_T* frame_info, bool* flow_end) override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (frame_info == nullptr || flow_end == nullptr || group_ < 0) {
            return false;
        }

        *flow_end = false;
        const auto ret = AXCL_VDEC_GetChnFrame(group_, kOutputChannel, frame_info, kAxWaitMs);
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
            (void)AXCL_VDEC_ReleaseChnFrame(group_, kOutputChannel, &frame_info);
        }
    }

private:
    AX_VDEC_GRP group_{-1};
    bool started_{false};
};

}  // namespace

std::unique_ptr<VideoDecoder> CreatePlatformVideoDecoder() {
    return std::make_unique<AxclVideoDecoder>();
}

}  // namespace axvsdk::codec::internal
