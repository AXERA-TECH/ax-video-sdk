#include "ax_video_encoder_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <thread>

#include "axcl_rt_memory.h"
#include "axcl_sys.h"
#include "axcl_venc.h"
#include "ax_venc_comm.h"

#include "ax_image_internal.h"
#include "ax_system_internal.h"
#include "ax_qpmap.h"

namespace axvsdk::codec::internal {

namespace {

constexpr AX_S32 kAxWaitMs = 100;
constexpr AX_S32 kAxSendWaitMs = 0;

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

class AxclVideoEncoder final : public AxVideoEncoderBase {
public:
    ~AxclVideoEncoder() override {
        Close();
    }

protected:
    bool CreateBackend(const ResolvedVideoEncoderConfig& config) override {
        if (!common::internal::EnsureAxclThreadContext(config.device_id)) {
            return false;
        }
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

        if (AXCL_VENC_CreateChnEx(&channel_, &channel_attr) != AX_SUCCESS) {
            channel_ = -1;
            return false;
        }

        if (config.qpmap.enable && !AllocQpMaps(config)) {
            (void)AXCL_VENC_DestroyChn(channel_);
            channel_ = -1;
            return false;
        }

        return true;
    }

    // QPMAP buffer ring:buffer 从 SendFrameEx 到硬件消费完都必须有效,
    // 深度与 VENC 输入 FIFO 对齐。AXCL 下 VENC 在卡上,map 必须落在设备 CMM;
    // host 侧另备 staging 缓冲用于逐帧组装,再 DMA 上卡。
    bool AllocQpMaps(const ResolvedVideoEncoderConfig& config) {
        qpmap_layout_ = MakeQpMapLayout(config.codec, config.width, config.height);
        if (!qpmap_layout_.valid()) return false;
        for (std::size_t i = 0; i < kQpMapRing; ++i) {
            AX_U64 phy = 0;
            AX_VOID* vir = nullptr;
            if (AXCL_SYS_MemAlloc(&phy, &vir, static_cast<AX_U32>(qpmap_layout_.size), 64,
                                  reinterpret_cast<const AX_S8*>("VencQpMap")) != AX_SUCCESS) {
                FreeQpMaps();
                return false;
            }
            qpmap_phy_[i] = phy;
            qpmap_dev_vir_[i] = vir;
        }
        qpmap_host_.assign(qpmap_layout_.size, 0);
        return true;
    }

    void FreeQpMaps() noexcept {
        for (std::size_t i = 0; i < kQpMapRing; ++i) {
            if (qpmap_phy_[i] != 0) {
                (void)AXCL_SYS_MemFree(qpmap_phy_[i], qpmap_dev_vir_[i]);
                qpmap_phy_[i] = 0;
                qpmap_dev_vir_[i] = nullptr;
            }
        }
        qpmap_host_.clear();
        qpmap_layout_ = {};
        qpmap_index_ = 0;
    }

    // 按当前 ROI 建图并挂在帧上提交。I 帧整帧 i_qp;P/B 背景 p_qp、ROI 处加 delta。
    bool SendFrameWithQpMap(const AX_VIDEO_FRAME_INFO_T& frame_info) {
        if (qpmap_host_.empty()) return false;
        const auto slot = qpmap_index_ % kQpMapRing;
        auto* map = qpmap_host_.data();

        const auto& cfg = config();
        const auto gop = cfg.gop == 0 ? 30U : cfg.gop;
        const bool intra = (qpmap_index_ % gop) == 0;
        const std::int32_t base_qp = QpMapBaseQp(intra);
        FillQpMap(map, qpmap_layout_, base_qp);
        if (!intra) {
            ApplyQpMapRois(map, qpmap_layout_, qp_rois(), QpMapRoiQp(base_qp),
                           cfg.width, cfg.height);
        }
        if (axclrtMemcpy(reinterpret_cast<void*>(qpmap_phy_[slot]), map, qpmap_layout_.size,
                         AXCL_MEMCPY_HOST_TO_DEVICE) != AXCL_SUCC) {
            return false;
        }

        AX_USER_FRAME_INFO_T user_frame{};
        user_frame.stUserFrame = frame_info;
        user_frame.stUserRcInfo.bQpMapValid = AX_TRUE;
        user_frame.stUserRcInfo.bIPCMMapValid = AX_FALSE;
        user_frame.stUserRcInfo.u32BlkStartQp = static_cast<AX_U32>(base_qp);
        user_frame.stUserRcInfo.u64QpMapPhyAddr = qpmap_phy_[slot];
        user_frame.stUserRcInfo.pQpMapVirAddr = reinterpret_cast<AX_S8*>(qpmap_dev_vir_[slot]);
        user_frame.stUserRcInfo.enFrameType = AX_FRAME_TYPE_AUTO;
        user_frame.stUserRcInfo.u32RoiMapDeltaSize = static_cast<AX_U32>(qpmap_layout_.size);

        while (!stop_requested()) {
            const auto ret = AXCL_VENC_SendFrameEx(channel_, &user_frame, kAxSendWaitMs);
            if (ret == AX_SUCCESS) {
                ++qpmap_index_;
                return true;
            }
            if (ret != AX_ERR_VENC_QUEUE_FULL && ret != AX_ERR_VENC_BUF_FULL) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    void DestroyBackend() noexcept override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            channel_ = -1;
            started_ = false;
            return;
        }
        StopBackend();
        if (channel_ >= 0) {
            for (int retry = 0; retry < 10; ++retry) {
                const auto ret = AXCL_VENC_DestroyChn(channel_);
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
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (channel_ < 0) {
            return false;
        }

        AX_VENC_RECV_PIC_PARAM_T recv_param{};
        recv_param.s32RecvPicNum = -1;
        started_ = AXCL_VENC_StartRecvFrame(channel_, &recv_param) == AX_SUCCESS;
        return started_;
    }

    void StopBackend() noexcept override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            started_ = false;
            return;
        }
        if (channel_ >= 0 && started_) {
            (void)AXCL_VENC_StopRecvFrame(channel_);
            (void)AXCL_VENC_ResetChn(channel_);
        }
        started_ = false;
    }

    bool SendFrameToEncoder(const common::AxImage& frame) override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (channel_ < 0) {
            return false;
        }

        const auto& frame_info = common::internal::AxImageAccess::GetAxFrameInfo(frame);
        if (config().qpmap.enable) {
            return SendFrameWithQpMap(frame_info);
        }
        while (!stop_requested()) {
            const auto ret = AXCL_VENC_SendFrame(channel_, &frame_info, kAxSendWaitMs);
            if (ret == AX_SUCCESS) {
                return true;
            }
            if (ret != AX_ERR_VENC_QUEUE_FULL && ret != AX_ERR_VENC_BUF_FULL) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    bool ReceivePacketFromEncoder(EncodedPacket* packet, bool* flow_end) override {
        if (!common::internal::EnsureAxclThreadContext(config().device_id)) {
            return false;
        }
        if (packet == nullptr || flow_end == nullptr || channel_ < 0) {
            return false;
        }

        *flow_end = false;
        AX_VENC_STREAM_T stream{};
        const auto ret = AXCL_VENC_GetStream(channel_, &stream, kAxWaitMs);
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
        packet->data.resize(stream.stPack.u32Len);
        const auto copy_ret = axclrtMemcpy(packet->data.data(),
                                           reinterpret_cast<void*>(stream.stPack.ulPhyAddr),
                                           stream.stPack.u32Len,
                                           AXCL_MEMCPY_DEVICE_TO_HOST);

        (void)AXCL_VENC_ReleaseStream(channel_, &stream);
        return copy_ret == AXCL_SUCC;
    }

private:
    static constexpr std::size_t kQpMapRing = 4;  // 与 u8InFifoDepth 对齐

    VENC_CHN channel_{-1};
    bool started_{false};

    QpMapLayout qpmap_layout_{};
    AX_U64 qpmap_phy_[kQpMapRing]{};
    AX_VOID* qpmap_dev_vir_[kQpMapRing]{};
    std::vector<std::int8_t> qpmap_host_;  // host 组装区,再 DMA 到设备
    std::uint64_t qpmap_index_{0};
};

}  // namespace

std::unique_ptr<VideoEncoder> CreatePlatformVideoEncoder() {
    return std::make_unique<AxclVideoEncoder>();
}

}  // namespace axvsdk::codec::internal
