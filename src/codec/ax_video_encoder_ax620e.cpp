#include "ax_video_encoder_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <atomic>
#include <thread>

#include "ax_venc_api.h"
#include "ax_venc_comm.h"

#include "ax_image_internal.h"

namespace axvsdk::codec::internal {

namespace {

constexpr AX_S32 kAxWaitMs = 100;

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

AX_U32 ResolveStreamBufferSize(const ResolvedVideoEncoderConfig& config) noexcept {
    return config.stream_buffer_size == 0 ? 1024U * 1024U : static_cast<AX_U32>(config.stream_buffer_size);
}

void ConfigureCbrRc(const ResolvedVideoEncoderConfig& config, AX_VENC_CHN_ATTR_T* channel_attr) {
    channel_attr->stRcAttr.s32FirstFrameStartQp = -1;
    channel_attr->stRcAttr.stFrameRate.fSrcFrameRate = static_cast<AX_F32>(config.src_frame_rate);
    channel_attr->stRcAttr.stFrameRate.fDstFrameRate = static_cast<AX_F32>(config.dst_frame_rate);

    if (config.codec == VideoCodecType::kH264) {
        channel_attr->stRcAttr.enRcMode = AX_VENC_RC_MODE_H264CBR;
        channel_attr->stRcAttr.stH264Cbr.u32Gop = config.gop;
        channel_attr->stRcAttr.stH264Cbr.u32BitRate = config.bitrate_kbps;
        channel_attr->stRcAttr.stH264Cbr.u32MinQp = 10;
        channel_attr->stRcAttr.stH264Cbr.u32MaxQp = 51;
        channel_attr->stRcAttr.stH264Cbr.u32MinIQp = 10;
        channel_attr->stRcAttr.stH264Cbr.u32MaxIQp = 51;
        channel_attr->stRcAttr.stH264Cbr.u32MaxIprop = 40;
        channel_attr->stRcAttr.stH264Cbr.u32MinIprop = 10;
        channel_attr->stRcAttr.stH264Cbr.s32IntraQpDelta = -2;
        channel_attr->stRcAttr.stH264Cbr.u32IdrQpDeltaRange = 10;
    } else {
        channel_attr->stRcAttr.enRcMode = AX_VENC_RC_MODE_H265CBR;
        channel_attr->stRcAttr.stH265Cbr.u32Gop = config.gop;
        channel_attr->stRcAttr.stH265Cbr.u32BitRate = config.bitrate_kbps;
        channel_attr->stRcAttr.stH265Cbr.u32MinQp = 10;
        channel_attr->stRcAttr.stH265Cbr.u32MaxQp = 51;
        channel_attr->stRcAttr.stH265Cbr.u32MinIQp = 10;
        channel_attr->stRcAttr.stH265Cbr.u32MaxIQp = 51;
        channel_attr->stRcAttr.stH265Cbr.u32MaxIprop = 40;
        channel_attr->stRcAttr.stH265Cbr.u32MinIprop = 30;
        channel_attr->stRcAttr.stH265Cbr.s32IntraQpDelta = -2;
        channel_attr->stRcAttr.stH265Cbr.u32IdrQpDeltaRange = 10;
        channel_attr->stRcAttr.stH265Cbr.s32DeBreathQpDelta = -2;
    }
}

class Ax620eVideoEncoder final : public AxVideoEncoderBase {
public:
    ~Ax620eVideoEncoder() override {
        Close();
    }

protected:
    bool CreateBackend(const ResolvedVideoEncoderConfig& config) override {
        const auto payload = ToAxPayload(config.codec);
        if (payload != PT_H264 && payload != PT_H265) {
            return false;
        }

        AX_VENC_CHN_ATTR_T channel_attr{};
        channel_attr.stVencAttr.enType = payload;
        channel_attr.stVencAttr.u32PicWidthSrc = config.width;
        channel_attr.stVencAttr.u32PicHeightSrc = config.height;
        channel_attr.stVencAttr.u32MaxPicWidth = config.max_width;
        channel_attr.stVencAttr.u32MaxPicHeight = config.max_height;
        channel_attr.stVencAttr.enMemSource = AX_MEMORY_SOURCE_CMM;
        channel_attr.stVencAttr.enLinkMode = AX_UNLINK_MODE;
        channel_attr.stVencAttr.enRotation = AX_ROTATION_0;
        channel_attr.stVencAttr.bDeBreathEffect = AX_FALSE;
        channel_attr.stVencAttr.bRefRingbuf = AX_FALSE;
        channel_attr.stVencAttr.u8InFifoDepth = 4;
        channel_attr.stVencAttr.u8OutFifoDepth = 4;
        channel_attr.stVencAttr.u32BufSize = ResolveStreamBufferSize(config);

        if (config.codec == VideoCodecType::kH264) {
            channel_attr.stVencAttr.enProfile = AX_VENC_H264_MAIN_PROFILE;
            channel_attr.stVencAttr.enLevel = AX_VENC_H264_LEVEL_5_1;
        } else {
            channel_attr.stVencAttr.enProfile = AX_VENC_HEVC_MAIN_PROFILE;
            channel_attr.stVencAttr.enLevel = AX_VENC_HEVC_LEVEL_5_1;
            channel_attr.stVencAttr.enTier = AX_VENC_HEVC_MAIN_TIER;
        }

        ConfigureCbrRc(config, &channel_attr);
        channel_attr.stGopAttr.enGopMode = AX_VENC_GOPMODE_NORMALP;
        channel_attr.stGopAttr.stNormalP.stPicConfig.s32QpOffset = 0;
        channel_attr.stGopAttr.stNormalP.stPicConfig.f32QpFactor = 0.4624F;

        if (AX_VENC_CreateChnEx(&channel_, &channel_attr) != AX_SUCCESS) {
            channel_ = -1;
            return false;
        }

        return true;
    }

    void DestroyBackend() noexcept override {
        StopBackend();
        if (channel_ >= 0) {
            for (int retry = 0; retry < 10; ++retry) {
                const auto ret = AX_VENC_DestroyChn(channel_);
                if (ret == AX_SUCCESS) {
                    channel_ = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            channel_ = -1;
        }
    }

    bool StartBackend() override {
        if (channel_ < 0) {
            return false;
        }

        AX_VENC_RECV_PIC_PARAM_T recv_param{};
        recv_param.s32RecvPicNum = -1;
        started_ = AX_VENC_StartRecvFrame(channel_, &recv_param) == AX_SUCCESS;
        return started_;
    }

    void StopBackend() noexcept override {
        if (channel_ >= 0 && started_) {
            (void)AX_VENC_StopRecvFrame(channel_);
        }
        started_ = false;
    }

    bool SendFrameToEncoder(const common::AxImage& frame) override {
        if (channel_ < 0) {
            return false;
        }

        const auto& frame_info = common::internal::AxImageAccess::GetAxFrameInfo(frame);
        const auto ret = AX_VENC_SendFrame(channel_, &frame_info, kAxWaitMs);
        if (ret != AX_SUCCESS) {
            static std::atomic<std::uint32_t> s_send_failures{0};
            const auto failures = s_send_failures.fetch_add(1, std::memory_order_relaxed);
            if (failures < 20 || (failures % 200) == 0) {
                std::fprintf(stderr,
                             "ax620e venc: AX_VENC_SendFrame chn=%d ret=0x%x blk=0x%x fmt=%d %ux%u stride=%u/%u pts=%llu seq=%llu\n",
                             channel_, ret,
                             frame_info.stVFrame.u32BlkId[0],
                             static_cast<int>(frame_info.stVFrame.enImgFormat),
                             frame_info.stVFrame.u32Width, frame_info.stVFrame.u32Height,
                             frame_info.stVFrame.u32PicStride[0], frame_info.stVFrame.u32PicStride[1],
                             static_cast<unsigned long long>(frame_info.stVFrame.u64PTS),
                             static_cast<unsigned long long>(frame_info.stVFrame.u64SeqNum));
            }
        }
        return ret == AX_SUCCESS;
    }

    bool ReceivePacketFromEncoder(EncodedPacket* packet, bool* flow_end) override {
        if (packet == nullptr || flow_end == nullptr || channel_ < 0) {
            return false;
        }

        *flow_end = false;
        AX_VENC_STREAM_T stream{};
        const auto ret = AX_VENC_GetStream(channel_, &stream, kAxWaitMs);
        if (ret != AX_SUCCESS) {
            if (ret == AX_ERR_VENC_FLOW_END) {
                *flow_end = true;
            }
            if (ret == AX_ERR_VENC_QUEUE_EMPTY || ret == AX_ERR_VENC_BUF_EMPTY || ret == AX_ERR_VENC_TIMEOUT) {
                return false;
            }
            static std::atomic<std::uint32_t> s_get_failures{0};
            const auto failures = s_get_failures.fetch_add(1, std::memory_order_relaxed);
            if (failures < 20 || (failures % 200) == 0) {
                std::fprintf(stderr, "ax620e venc: AX_VENC_GetStream chn=%d ret=0x%x flow_end=%d\n",
                             channel_, ret, *flow_end ? 1 : 0);
            }
            return false;
        }

        packet->codec = config().codec;
        packet->pts = stream.stPack.u64PTS;
        packet->duration = 0;
        packet->key_frame = stream.stPack.enCodingType == AX_VENC_INTRA_FRAME ||
                            stream.stPack.enCodingType == AX_VENC_VIRTUAL_INTRA_FRAME;
        packet->data.assign(stream.stPack.pu8Addr, stream.stPack.pu8Addr + stream.stPack.u32Len);

        (void)AX_VENC_ReleaseStream(channel_, &stream);
        return true;
    }

private:
    VENC_CHN channel_{-1};
    bool started_{false};
};

}  // namespace

std::unique_ptr<VideoEncoder> CreatePlatformVideoEncoder() {
    return std::make_unique<Ax620eVideoEncoder>();
}

}  // namespace axvsdk::codec::internal
