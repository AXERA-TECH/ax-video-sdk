#include "ax_video_encoder_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <thread>

#include "ax_sys_api.h"
#include "ax_venc_api.h"
#include "ax_venc_comm.h"

#include "ax_image_internal.h"
#include "ax_qpmap.h"

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
    }
}

// ABS QPMAP:实际 QP 完全由每帧的 map 决定,关掉 CTB RC 以免目标码率把 QP 改写回去。
void ConfigureQpMapRc(const ResolvedVideoEncoderConfig& config, AX_VENC_CHN_ATTR_T* channel_attr) {
    auto* rc = &channel_attr->stRcAttr;
    rc->s32FirstFrameStartQp = -1;
    rc->stFrameRate.fSrcFrameRate = static_cast<AX_F32>(config.src_frame_rate);
    rc->stFrameRate.fDstFrameRate = static_cast<AX_F32>(config.dst_frame_rate);

    if (config.codec == VideoCodecType::kH264) {
        rc->enRcMode = AX_VENC_RC_MODE_H264QPMAP;
        auto* qpmap = &rc->stH264QpMap;
        qpmap->u32Gop = config.gop;
        qpmap->u32StatTime = 1;
        qpmap->u32TargetBitRate = config.bitrate_kbps;
        qpmap->stQpmapInfo.enCtbRcMode = AX_VENC_RC_CTBRC_DISABLE;
        qpmap->stQpmapInfo.enQpmapQpType = AX_VENC_QPMAP_QP_ABS;
        qpmap->stQpmapInfo.enQpmapBlockType = AX_VENC_QPMAP_BLOCK_DISABLE;
        qpmap->stQpmapInfo.enQpmapBlockUnit = AX_VENC_QPMAP_BLOCK_UNIT_16x16;
    } else {
        rc->enRcMode = AX_VENC_RC_MODE_H265QPMAP;
        auto* qpmap = &rc->stH265QpMap;
        qpmap->u32Gop = config.gop;
        qpmap->u32StatTime = 1;
        qpmap->u32TargetBitRate = config.bitrate_kbps;
        qpmap->stQpmapInfo.enCtbRcMode = AX_VENC_RC_CTBRC_DISABLE;
        qpmap->stQpmapInfo.enQpmapQpType = AX_VENC_QPMAP_QP_ABS;
        qpmap->stQpmapInfo.enQpmapBlockType = AX_VENC_QPMAP_BLOCK_DISABLE;
        qpmap->stQpmapInfo.enQpmapBlockUnit = AX_VENC_QPMAP_BLOCK_UNIT_16x16;
    }
}

class Ax650VideoEncoder final : public AxVideoEncoderBase {
public:
    ~Ax650VideoEncoder() override {
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
        channel_attr.stVencAttr.enLinkMode = AX_VENC_UNLINK_MODE;
        channel_attr.stVencAttr.u8InFifoDepth = 4;
        channel_attr.stVencAttr.u8OutFifoDepth = 4;
        channel_attr.stVencAttr.u32BufSize = ResolveStreamBufferSize(config);
        channel_attr.stVencAttr.enStrmBitDepth = AX_VENC_STREAM_BIT_8;

        if (config.codec == VideoCodecType::kH264) {
            channel_attr.stVencAttr.enProfile = AX_VENC_H264_MAIN_PROFILE;
            channel_attr.stVencAttr.enLevel = AX_VENC_H264_LEVEL_5_1;
        } else {
            channel_attr.stVencAttr.enProfile = AX_VENC_HEVC_MAIN_PROFILE;
            channel_attr.stVencAttr.enLevel = AX_VENC_HEVC_LEVEL_5_1;
            channel_attr.stVencAttr.enTier = AX_VENC_HEVC_MAIN_TIER;
        }

        if (config.qpmap.enable) {
            ConfigureQpMapRc(config, &channel_attr);
        } else {
            ConfigureCbrRc(config, &channel_attr);
        }
        channel_attr.stGopAttr.enGopMode = AX_VENC_GOPMODE_NORMALP;
        channel_attr.stGopAttr.stNormalP.stPicConfig.s32QpOffset = 0;
        channel_attr.stGopAttr.stNormalP.stPicConfig.f32QpFactor = 0.4624F;

        if (AX_VENC_CreateChnEx(&channel_, &channel_attr) != AX_SUCCESS) {
            channel_ = -1;
            return false;
        }

        if (config.qpmap.enable && !AllocQpMaps(config)) {
            (void)AX_VENC_DestroyChn(channel_);
            channel_ = -1;
            return false;
        }

        return true;
    }

    // QPMAP buffer ring:buffer 从 SendFrameEx 到硬件消费完都必须有效,
    // 深度与 VENC 输入 FIFO 对齐,避免还在用时被下一帧覆写。
    bool AllocQpMaps(const ResolvedVideoEncoderConfig& config) {
        qpmap_layout_ = MakeQpMapLayout(config.codec, config.width, config.height);
        if (!qpmap_layout_.valid()) return false;
        for (std::size_t i = 0; i < kQpMapRing; ++i) {
            AX_U64 phy = 0;
            AX_VOID* vir = nullptr;
            if (AX_SYS_MemAlloc(&phy, &vir, static_cast<AX_U32>(qpmap_layout_.size), 64,
                                reinterpret_cast<const AX_S8*>("VencQpMap")) != AX_SUCCESS ||
                vir == nullptr) {
                FreeQpMaps();
                return false;
            }
            qpmap_phy_[i] = phy;
            qpmap_vir_[i] = static_cast<std::int8_t*>(vir);
        }
        return true;
    }

    void FreeQpMaps() noexcept {
        for (std::size_t i = 0; i < kQpMapRing; ++i) {
            if (qpmap_vir_[i] != nullptr) {
                (void)AX_SYS_MemFree(qpmap_phy_[i], qpmap_vir_[i]);
                qpmap_vir_[i] = nullptr;
                qpmap_phy_[i] = 0;
            }
        }
        qpmap_layout_ = {};
        qpmap_index_ = 0;
    }

    // 按当前 ROI 建图并挂在帧上提交。I 帧整帧 i_qp;P/B 背景 p_qp、ROI 处加 delta。
    bool SendFrameWithQpMap(AX_VIDEO_FRAME_INFO_T* send_info) {
        const auto slot = qpmap_index_ % kQpMapRing;
        auto* map = qpmap_vir_[slot];
        if (map == nullptr) return false;

        const auto& cfg = config();
        const auto gop = cfg.gop == 0 ? 30U : cfg.gop;
        const bool intra = (qpmap_index_ % gop) == 0;
        const std::int32_t base_qp = intra ? cfg.qpmap.i_qp : cfg.qpmap.p_qp;
        FillQpMap(map, qpmap_layout_, base_qp);
        if (!intra) {
            ApplyQpMapRois(map, qpmap_layout_, qp_rois(), base_qp + cfg.qpmap.roi_qp_delta,
                           cfg.width, cfg.height);
        }
        (void)AX_SYS_MflushCache(qpmap_phy_[slot], map, static_cast<AX_U32>(qpmap_layout_.size));

        AX_USER_FRAME_INFO_T user_frame{};
        user_frame.stUserFrame = *send_info;
        user_frame.stUserRcInfo.bQpMapValid = AX_TRUE;
        user_frame.stUserRcInfo.bIPCMMapValid = AX_FALSE;
        user_frame.stUserRcInfo.u32BlkStartQp = static_cast<AX_U32>(base_qp);
        user_frame.stUserRcInfo.u64QpMapPhyAddr = qpmap_phy_[slot];
        user_frame.stUserRcInfo.pQpMapVirAddr = reinterpret_cast<AX_S8*>(map);
        user_frame.stUserRcInfo.enFrameType = AX_FRAME_TYPE_AUTO;
        user_frame.stUserRcInfo.u32RoiMapDeltaSize = static_cast<AX_U32>(qpmap_layout_.size);

        ++qpmap_index_;
        return AX_VENC_SendFrameEx(channel_, &user_frame, kAxWaitMs) == AX_SUCCESS;
    }

    void DestroyBackend() noexcept override {
        StopBackend();
        FreeQpMaps();
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
        // VDEC/IVPS may output coded height (aligned) with crop describing the visible geometry.
        // VENC validates u32Width/u32Height against channel attributes, so feed the visible geometry here.
        AX_VIDEO_FRAME_INFO_T send_info = frame_info;
        if (send_info.stVFrame.s16CropWidth > 0) {
            send_info.stVFrame.u32Width = static_cast<AX_U32>(send_info.stVFrame.s16CropWidth);
        }
        if (send_info.stVFrame.s16CropHeight > 0) {
            send_info.stVFrame.u32Height = static_cast<AX_U32>(send_info.stVFrame.s16CropHeight);
        }

        if (config().qpmap.enable) {
            return SendFrameWithQpMap(&send_info);
        }

        const auto ret = AX_VENC_SendFrame(channel_, &send_info, kAxWaitMs);
        if (ret != AX_SUCCESS) {
            const auto& vf = send_info.stVFrame;
            std::fprintf(stderr,
                         "ax650 venc: AX_VENC_SendFrame chn=%d ret=0x%x fmt=%d %ux%u crop=%d,%d+%dx%d stride=%u/%u blk=0x%x pts=%llu seq=%llu\n",
                         channel_, ret,
                         static_cast<int>(vf.enImgFormat),
                         vf.u32Width, vf.u32Height,
                         static_cast<int>(vf.s16CropX), static_cast<int>(vf.s16CropY),
                         static_cast<int>(vf.s16CropWidth), static_cast<int>(vf.s16CropHeight),
                         vf.u32PicStride[0], vf.u32PicStride[1],
                         vf.u32BlkId[0],
                         static_cast<unsigned long long>(vf.u64PTS),
                         static_cast<unsigned long long>(vf.u64SeqNum));
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
    static constexpr std::size_t kQpMapRing = 4;  // 与 u8InFifoDepth 对齐

    VENC_CHN channel_{-1};
    bool started_{false};

    QpMapLayout qpmap_layout_{};
    AX_U64 qpmap_phy_[kQpMapRing]{};
    std::int8_t* qpmap_vir_[kQpMapRing]{};
    std::uint64_t qpmap_index_{0};
};

}  // namespace

std::unique_ptr<VideoEncoder> CreatePlatformVideoEncoder() {
    return std::make_unique<Ax650VideoEncoder>();
}

}  // namespace axvsdk::codec::internal
