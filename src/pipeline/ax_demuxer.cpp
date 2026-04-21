#include "pipeline/ax_demuxer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rtsp-client/rtsp_client.h>

#include "ax_mp4_demuxer.h"
#include "ax_rtsp_internal.h"

namespace axvsdk::pipeline {

namespace {

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string TrimAsciiWhitespaceCopy(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    if (begin == value.end()) {
        return {};
    }

    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return std::string(begin, end);
}

std::string StripUriQueryAndFragment(const std::string& value) {
    const auto query_pos = value.find('?');
    const auto fragment_pos = value.find('#');
    const auto end_pos = std::min(query_pos, fragment_pos);
    if (end_pos == std::string::npos) {
        return value;
    }
    return value.substr(0, end_pos);
}

class BitReader final {
public:
    explicit BitReader(const std::vector<std::uint8_t>& data) : data_(data) {}

    bool SkipBits(std::size_t count) {
        const std::size_t total_bits = data_.size() * 8U;
        if (bit_offset_ + count > total_bits) {
            return false;
        }
        bit_offset_ += count;
        return true;
    }

    bool ReadBits(std::uint32_t count, std::uint32_t* out) {
        if (out == nullptr || count > 32) {
            return false;
        }
        if (count == 0) {
            *out = 0;
            return true;
        }
        const std::size_t total_bits = data_.size() * 8U;
        if (bit_offset_ + count > total_bits) {
            return false;
        }

        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t bit_index = bit_offset_ + i;
            const std::size_t byte_index = bit_index / 8U;
            const std::size_t bit_in_byte = 7U - (bit_index % 8U);
            value = (value << 1U) | ((data_[byte_index] >> bit_in_byte) & 0x01U);
        }

        bit_offset_ += count;
        *out = value;
        return true;
    }

    bool ReadBit(std::uint32_t* out) {
        return ReadBits(1, out);
    }

    bool ReadUE(std::uint32_t* out) {
        if (out == nullptr) return false;

        std::uint32_t zeros = 0;
        while (true) {
            std::uint32_t bit = 0;
            if (!ReadBit(&bit)) return false;
            if (bit != 0) break;
            ++zeros;
            if (zeros > 31) return false;
        }

        if (zeros == 0) {
            *out = 0;
            return true;
        }

        std::uint32_t suffix = 0;
        if (!ReadBits(zeros, &suffix)) return false;
        *out = ((1U << zeros) - 1U) + suffix;
        return true;
    }

    bool ReadSE(std::int32_t* out) {
        if (out == nullptr) return false;
        std::uint32_t ue = 0;
        if (!ReadUE(&ue)) return false;
        const std::int32_t value =
            (ue % 2U == 0) ? -static_cast<std::int32_t>(ue / 2U) : static_cast<std::int32_t>((ue + 1U) / 2U);
        *out = value;
        return true;
    }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t bit_offset_{0};
};

std::vector<std::uint8_t> RemoveEmulationPreventionBytes(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> rbsp;
    if (data == nullptr || size == 0) {
        return rbsp;
    }
    rbsp.reserve(size);

    std::size_t zero_count = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t b = data[i];
        if (zero_count >= 2 && b == 0x03) {
            zero_count = 0;
            continue;
        }
        rbsp.push_back(b);
        if (b == 0x00) {
            ++zero_count;
        } else {
            zero_count = 0;
        }
    }
    return rbsp;
}

bool SkipH264ScalingList(BitReader* br, std::uint32_t size) {
    if (br == nullptr) return false;
    int last_scale = 8;
    int next_scale = 8;
    for (std::uint32_t j = 0; j < size; ++j) {
        if (next_scale != 0) {
            std::int32_t delta_scale = 0;
            if (!br->ReadSE(&delta_scale)) return false;
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
    return true;
}

bool ParseH264SpsCodedSize(const std::vector<std::uint8_t>& sps_nalu,
                           std::uint32_t* width,
                           std::uint32_t* height) {
    if (width == nullptr || height == nullptr) {
        return false;
    }
    *width = 0;
    *height = 0;
    if (sps_nalu.size() < 2) {
        return false;
    }

    // Skip NAL header byte.
    const auto rbsp = RemoveEmulationPreventionBytes(sps_nalu.data() + 1U, sps_nalu.size() - 1U);
    BitReader br(rbsp);

    std::uint32_t profile_idc = 0;
    std::uint32_t constraint_flags = 0;
    std::uint32_t level_idc = 0;
    std::uint32_t sps_id = 0;
    if (!br.ReadBits(8, &profile_idc) ||
        !br.ReadBits(8, &constraint_flags) ||
        !br.ReadBits(8, &level_idc) ||
        !br.ReadUE(&sps_id)) {
        return false;
    }

    std::uint32_t chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 144) {
        if (!br.ReadUE(&chroma_format_idc)) return false;
        if (chroma_format_idc == 3) {
            std::uint32_t separate_colour_plane_flag = 0;
            if (!br.ReadBit(&separate_colour_plane_flag)) return false;
        }
        std::uint32_t bit_depth_luma_minus8 = 0;
        std::uint32_t bit_depth_chroma_minus8 = 0;
        std::uint32_t qpprime_y_zero_transform_bypass_flag = 0;
        if (!br.ReadUE(&bit_depth_luma_minus8) ||
            !br.ReadUE(&bit_depth_chroma_minus8) ||
            !br.ReadBit(&qpprime_y_zero_transform_bypass_flag)) {
            return false;
        }
        std::uint32_t seq_scaling_matrix_present_flag = 0;
        if (!br.ReadBit(&seq_scaling_matrix_present_flag)) return false;
        if (seq_scaling_matrix_present_flag != 0) {
            const std::uint32_t scaling_list_count = (chroma_format_idc != 3) ? 8U : 12U;
            for (std::uint32_t i = 0; i < scaling_list_count; ++i) {
                std::uint32_t seq_scaling_list_present_flag = 0;
                if (!br.ReadBit(&seq_scaling_list_present_flag)) return false;
                if (seq_scaling_list_present_flag != 0) {
                    if (!SkipH264ScalingList(&br, i < 6 ? 16U : 64U)) return false;
                }
            }
        }
    }

    std::uint32_t log2_max_frame_num_minus4 = 0;
    if (!br.ReadUE(&log2_max_frame_num_minus4)) return false;

    std::uint32_t pic_order_cnt_type = 0;
    if (!br.ReadUE(&pic_order_cnt_type)) return false;
    if (pic_order_cnt_type == 0) {
        std::uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
        if (!br.ReadUE(&log2_max_pic_order_cnt_lsb_minus4)) return false;
    } else if (pic_order_cnt_type == 1) {
        std::uint32_t delta_pic_order_always_zero_flag = 0;
        std::int32_t offset_for_non_ref_pic = 0;
        std::int32_t offset_for_top_to_bottom_field = 0;
        std::uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
        if (!br.ReadBit(&delta_pic_order_always_zero_flag) ||
            !br.ReadSE(&offset_for_non_ref_pic) ||
            !br.ReadSE(&offset_for_top_to_bottom_field) ||
            !br.ReadUE(&num_ref_frames_in_pic_order_cnt_cycle)) {
            return false;
        }
        for (std::uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; ++i) {
            std::int32_t offset_for_ref_frame = 0;
            if (!br.ReadSE(&offset_for_ref_frame)) return false;
        }
    }

    std::uint32_t max_num_ref_frames = 0;
    std::uint32_t gaps_in_frame_num_value_allowed_flag = 0;
    std::uint32_t pic_width_in_mbs_minus1 = 0;
    std::uint32_t pic_height_in_map_units_minus1 = 0;
    std::uint32_t frame_mbs_only_flag = 0;
    if (!br.ReadUE(&max_num_ref_frames) ||
        !br.ReadBit(&gaps_in_frame_num_value_allowed_flag) ||
        !br.ReadUE(&pic_width_in_mbs_minus1) ||
        !br.ReadUE(&pic_height_in_map_units_minus1) ||
        !br.ReadBit(&frame_mbs_only_flag)) {
        return false;
    }
    if (frame_mbs_only_flag == 0) {
        std::uint32_t mb_adaptive_frame_field_flag = 0;
        if (!br.ReadBit(&mb_adaptive_frame_field_flag)) return false;
    }
    std::uint32_t direct_8x8_inference_flag = 0;
    if (!br.ReadBit(&direct_8x8_inference_flag)) return false;

    // We intentionally ignore cropping offsets here. The coded size is a safe upper bound for buffer allocation.
    const std::uint32_t w = (pic_width_in_mbs_minus1 + 1U) * 16U;
    const std::uint32_t h =
        (pic_height_in_map_units_minus1 + 1U) * 16U * (2U - static_cast<std::uint32_t>(frame_mbs_only_flag));

    if (w == 0 || h == 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

bool SkipH265ProfileTierLevel(BitReader* br, std::uint32_t max_sub_layers_minus1) {
    if (br == nullptr) return false;

    // general_profile_space(2), general_tier_flag(1), general_profile_idc(5)
    if (!br->SkipBits(2 + 1 + 5)) return false;
    // general_profile_compatibility_flag[32]
    if (!br->SkipBits(32)) return false;
    // general_progressive_source_flag, general_interlaced_source_flag,
    // general_non_packed_constraint_flag, general_frame_only_constraint_flag
    if (!br->SkipBits(4)) return false;
    // general_reserved_zero_44bits (remaining bits of general_constraint_indicator_flags)
    if (!br->SkipBits(44)) return false;
    // general_level_idc (8)
    if (!br->SkipBits(8)) return false;

    std::uint32_t sub_layer_profile_present[8] = {0};
    std::uint32_t sub_layer_level_present[8] = {0};
    for (std::uint32_t i = 0; i < max_sub_layers_minus1 && i < 8; ++i) {
        if (!br->ReadBit(&sub_layer_profile_present[i])) return false;
        if (!br->ReadBit(&sub_layer_level_present[i])) return false;
    }
    if (max_sub_layers_minus1 > 0) {
        for (std::uint32_t i = max_sub_layers_minus1; i < 8; ++i) {
            // reserved_zero_2bits
            if (!br->SkipBits(2)) return false;
        }
    }

    for (std::uint32_t i = 0; i < max_sub_layers_minus1 && i < 8; ++i) {
        if (sub_layer_profile_present[i] != 0) {
            if (!br->SkipBits(2 + 1 + 5)) return false;  // profile_space/tier/profile_idc
            if (!br->SkipBits(32)) return false;         // profile_compatibility_flag[32]
            if (!br->SkipBits(4)) return false;          // progressive/interlaced/non_packed/frame_only
            if (!br->SkipBits(44)) return false;         // reserved_zero_44bits (constraint_indicator_flags)
        }
        if (sub_layer_level_present[i] != 0) {
            if (!br->SkipBits(8)) return false;  // sub_layer_level_idc
        }
    }
    return true;
}

bool ParseH265SpsCodedSize(const std::vector<std::uint8_t>& sps_nalu,
                           std::uint32_t* width,
                           std::uint32_t* height) {
    if (width == nullptr || height == nullptr) {
        return false;
    }
    *width = 0;
    *height = 0;
    if (sps_nalu.size() < 3) {
        return false;
    }

    // Skip the 2-byte NAL header.
    const auto rbsp = RemoveEmulationPreventionBytes(sps_nalu.data() + 2U, sps_nalu.size() - 2U);
    BitReader br(rbsp);

    std::uint32_t tmp = 0;
    std::uint32_t max_sub_layers_minus1 = 0;
    if (!br.ReadBits(4, &tmp) ||          // sps_video_parameter_set_id
        !br.ReadBits(3, &max_sub_layers_minus1) ||
        !br.ReadBits(1, &tmp)) {          // sps_temporal_id_nesting_flag
        return false;
    }

    if (!SkipH265ProfileTierLevel(&br, max_sub_layers_minus1)) {
        return false;
    }

    std::uint32_t sps_seq_parameter_set_id = 0;
    std::uint32_t chroma_format_idc = 0;
    if (!br.ReadUE(&sps_seq_parameter_set_id) ||
        !br.ReadUE(&chroma_format_idc)) {
        return false;
    }
    if (chroma_format_idc == 3) {
        if (!br.ReadBit(&tmp)) return false;  // separate_colour_plane_flag
    }

    std::uint32_t pic_width_in_luma_samples = 0;
    std::uint32_t pic_height_in_luma_samples = 0;
    if (!br.ReadUE(&pic_width_in_luma_samples) ||
        !br.ReadUE(&pic_height_in_luma_samples)) {
        return false;
    }

    if (pic_width_in_luma_samples == 0 || pic_height_in_luma_samples == 0) {
        return false;
    }

    *width = pic_width_in_luma_samples;
    *height = pic_height_in_luma_samples;
    return true;
}

bool EndsWithIgnoreCase(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }

    return ToLowerCopy(value.substr(value.size() - suffix.size())) == ToLowerCopy(suffix);
}

std::uint64_t ToMicroseconds(std::uint64_t value, std::uint32_t timescale, double fps_fallback) noexcept {
    if (timescale != 0U) {
        return value * 1000000ULL / timescale;
    }
    if (fps_fallback > 0.0) {
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::duration<double>(static_cast<double>(value) / fps_fallback));
        return static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0));
    }
    return value;
}

bool ShouldOverrideStreamSize(std::uint32_t current_w,
                              std::uint32_t current_h,
                              std::uint32_t probed_w,
                              std::uint32_t probed_h) noexcept {
    if (probed_w == 0 || probed_h == 0) {
        return false;
    }
    if (current_w == 0 || current_h == 0) {
        return true;
    }

    const auto ratio = [](std::uint32_t a, std::uint32_t b) noexcept -> double {
        const auto hi = static_cast<double>(std::max(a, b));
        const auto lo = static_cast<double>(std::min(a, b));
        if (lo <= 0.0) {
            return hi;
        }
        return hi / lo;
    };

    static constexpr double kOverrideRatio = 1.2;
    return ratio(current_w, probed_w) >= kOverrideRatio || ratio(current_h, probed_h) >= kOverrideRatio;
}

codec::VideoStreamInfo ToVideoStreamInfo(const codec::Mp4VideoInfo& video_info) noexcept {
    codec::VideoStreamInfo stream{};
    stream.codec = video_info.codec;
    stream.width = video_info.width;
    stream.height = video_info.height;
    stream.frame_rate = video_info.fps > 0.0 ? video_info.fps : 30.0;
    return stream;
}

class AxDemuxer final : public Demuxer {
public:
    ~AxDemuxer() override {
        Close();
    }

    bool Open(const DemuxerConfig& config) override {
        Close();

        DemuxerInputType type = DemuxerInputType::kUnknown;
        if (!DetectDemuxerInputType(config.uri, &type)) {
            return false;
        }

        config_ = config;
        type_ = type;
        interrupted_.store(false, std::memory_order_relaxed);

        if (type_ == DemuxerInputType::kMp4File) {
            auto demuxer = codec::AxMp4Demuxer::Open(config.uri);
            if (!demuxer) {
                Close();
                return false;
            }

            const auto& video_info = demuxer->video_info();
            if ((video_info.codec != codec::VideoCodecType::kH264 &&
                 video_info.codec != codec::VideoCodecType::kH265) ||
                video_info.width == 0 || video_info.height == 0) {
                Close();
                return false;
            }

            video_info_ = video_info;
            stream_info_ = ToVideoStreamInfo(video_info_);
            demuxer_ = std::move(demuxer);
            ResetPlaybackState();
            return true;
        }

        if (type_ == DemuxerInputType::kRtspPull) {
            if (!StartRtspPlayback()) {
                Close();
                return false;
            }
            MarkRtspActivity();
            (void)PrimeRtspStreamInfo();
            if (stream_info_.width == 0 || stream_info_.height == 0) {
                Close();
                return false;
            }
            return true;
        }

        Close();
        return false;
    }

    void Close() noexcept override {
        Interrupt();
        CloseRtspSession();
        demuxer_.reset();
        config_ = {};
        type_ = DemuxerInputType::kUnknown;
        video_info_ = {};
        stream_info_ = {};
        ResetPlaybackState();
    }

    bool ReadPacket(codec::EncodedPacket* packet) override {
        if (packet == nullptr) {
            return false;
        }

        if (type_ == DemuxerInputType::kMp4File) {
            if (!demuxer_) {
                return false;
            }

            while (true) {
                if (!demuxer_->ReadNextPacket(packet)) {
                    if (!config_.loop_playback) {
                        return false;
                    }
                    demuxer_->Reset();
                    AdvanceLoopPlaybackState();
                    continue;
                }

                // Mp4Demuxer already normalizes timestamps to microseconds.
                const auto packet_duration_us = packet->duration;
                const auto effective_duration_us =
                    packet_duration_us == 0 ? ResolveFallbackFrameDurationUs() : packet_duration_us;
                const auto packet_pts_us = packet->pts;

                if (config_.realtime_playback) {
                    if (!first_packet_) {
                        std::this_thread::sleep_until(next_due_);
                    }

                    next_due_ += std::chrono::microseconds(effective_duration_us);
                }

                first_packet_ = false;
                // Loop bookkeeping uses a monotonic cursor to keep each loop appended in time,
                // but we preserve the original MP4 packet PTS for correct B-frame reorder in VDEC.
                current_loop_span_us_ = emitted_pts_cursor_us_ + effective_duration_us;
                packet->pts = packet_pts_offset_us_ + packet_pts_us;
                packet->duration = effective_duration_us;
                emitted_pts_cursor_us_ += effective_duration_us;
                return true;
            }
        }

        if (type_ != DemuxerInputType::kRtspPull) {
            return false;
        }

        while (!interrupted_.load(std::memory_order_relaxed)) {
            const auto now = std::chrono::steady_clock::now();
            if (!EnsureRtspPlaying(now)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (now - last_rtsp_keepalive_ >= kRtspKeepaliveInterval) {
                // Some cameras stop RTP delivery after ~60s without RTSP keep-alive.
                // GET_PARAMETER is widely supported and carries the Session header.
                if (!rtsp_client_.sendGetParameter("")) {
                    ScheduleRtspReconnect(now);
                    continue;
                }
                last_rtsp_keepalive_ = now;
            }

            rtsp::VideoFrame frame{};
            if (rtsp_client_.receiveFrame(frame, 200)) {
                last_rtsp_frame_ = now;
                packet->codec = stream_info_.codec;
                packet->pts = frame.pts * 1000ULL;
                packet->duration = frame.fps > 0
                                       ? (1000000ULL / static_cast<std::uint64_t>(frame.fps))
                                       : (stream_info_.frame_rate > 0.0
                                              ? static_cast<std::uint64_t>(1000000.0 / stream_info_.frame_rate)
                                              : 0ULL);
                packet->key_frame = frame.type == rtsp::FrameType::IDR;

                const bool inject_decoder_config = !rtsp_decoder_config_sent_ || packet->key_frame;
                const std::size_t prefix_size = inject_decoder_config ? rtsp_decoder_prefix_.size() : 0U;
                packet->data.clear();
                packet->data.reserve(prefix_size + frame.size);
                if (inject_decoder_config && !rtsp_decoder_prefix_.empty()) {
                    packet->data.insert(packet->data.end(), rtsp_decoder_prefix_.begin(), rtsp_decoder_prefix_.end());
                }
                if (frame.data != nullptr && frame.size != 0) {
                    packet->data.insert(packet->data.end(), frame.data, frame.data + frame.size);
                }
                rtsp_decoder_config_sent_ = rtsp_decoder_config_sent_ || inject_decoder_config;
                return !packet->data.empty();
            }

            if (interrupted_.load(std::memory_order_relaxed)) {
                return false;
            }

            // No video frames arrived in time - if this persists, force a reconnect.
            if (now - last_rtsp_frame_ >= kRtspNoFrameRestartTimeout) {
                ScheduleRtspReconnect(now);
            }
        }

        return false;
    }

    bool Reset() noexcept override {
        interrupted_.store(false, std::memory_order_relaxed);

        if (type_ == DemuxerInputType::kMp4File) {
            if (!demuxer_) {
                return false;
            }
            demuxer_->Reset();
            ResetPlaybackState();
            return true;
        }

        if (type_ != DemuxerInputType::kRtspPull) {
            return false;
        }

        if (rtsp_client_.isPlaying()) {
            return true;
        }
        return StartRtspPlayback();
    }

    void Interrupt() noexcept override {
        interrupted_.store(true, std::memory_order_relaxed);
        if (type_ == DemuxerInputType::kRtspPull) {
            rtsp_client_.interrupt();
        }
    }

    codec::VideoStreamInfo GetVideoStreamInfo() const noexcept override {
        return stream_info_;
    }

private:
    static constexpr auto kRtspKeepaliveInterval = std::chrono::seconds(15);
    static constexpr auto kRtspNoFrameRestartTimeout = std::chrono::seconds(5);
    static constexpr auto kRtspReconnectBackoffMin = std::chrono::milliseconds(200);
    static constexpr auto kRtspReconnectBackoffMax = std::chrono::milliseconds(5000);

    void MarkRtspActivity() noexcept {
        const auto now = std::chrono::steady_clock::now();
        last_rtsp_frame_ = now;
        last_rtsp_keepalive_ = now;
        next_rtsp_reconnect_ = now;
        rtsp_reconnect_backoff_ = kRtspReconnectBackoffMin;
    }

    void ScheduleRtspReconnect(std::chrono::steady_clock::time_point now) noexcept {
        CloseRtspSession();
        next_rtsp_reconnect_ = now;
        rtsp_reconnect_backoff_ = kRtspReconnectBackoffMin;
        last_rtsp_frame_ = now;
        last_rtsp_keepalive_ = now;
    }

    bool EnsureRtspPlaying(std::chrono::steady_clock::time_point now) noexcept {
        if (rtsp_client_.isPlaying()) {
            return true;
        }
        if (now < next_rtsp_reconnect_) {
            return false;
        }

        if (StartRtspPlayback()) {
            MarkRtspActivity();
            (void)PrimeRtspStreamInfo();
            return true;
        }

        next_rtsp_reconnect_ = now + rtsp_reconnect_backoff_;
        rtsp_reconnect_backoff_ = std::min(rtsp_reconnect_backoff_ * 2, kRtspReconnectBackoffMax);
        return false;
    }

    std::uint64_t ResolveFallbackFrameDurationUs() const noexcept {
        const double fps = video_info_.fps > 0.0 ? video_info_.fps : 30.0;
        if (fps <= 0.0) {
            return 33333ULL;
        }

        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::duration<double>(1.0 / fps));
        return static_cast<std::uint64_t>(std::max<std::int64_t>(1, duration.count()));
    }

    bool PrepareRtspSession() noexcept {
        CloseRtspSession();

        rtsp::RtspClientConfig client_config{};
        client_config.prefer_tcp_transport = true;
        client_config.fallback_to_tcp = true;
        client_config.buffer_size = 120;
        rtsp_client_.setConfig(client_config);

        if (!rtsp_client_.open(config_.uri)) {
            return false;
        }
        if (!rtsp_client_.describe()) {
            CloseRtspSession();
            return false;
        }

        rtsp_session_ = rtsp_client_.getSessionInfo();
        if (!rtsp_session_.has_video || rtsp_session_.media_streams.empty()) {
            CloseRtspSession();
            return false;
        }

        const auto& media = rtsp_session_.media_streams.front();
        const auto stream_codec = internal::ToSdkCodec(media.codec);
        if (stream_codec != codec::VideoCodecType::kH264 && stream_codec != codec::VideoCodecType::kH265) {
            CloseRtspSession();
            return false;
        }

        stream_info_.codec = stream_codec;
        std::uint32_t probed_w = media.width;
        std::uint32_t probed_h = media.height;
        if ((probed_w == 0 || probed_h == 0) && !media.sps.empty()) {
            std::uint32_t parsed_w = 0;
            std::uint32_t parsed_h = 0;
            if (stream_codec == codec::VideoCodecType::kH264) {
                (void)ParseH264SpsCodedSize(media.sps, &parsed_w, &parsed_h);
            } else if (stream_codec == codec::VideoCodecType::kH265) {
                (void)ParseH265SpsCodedSize(media.sps, &parsed_w, &parsed_h);
            }
            if (parsed_w != 0 && parsed_h != 0) {
                probed_w = parsed_w;
                probed_h = parsed_h;
            }
        }
        stream_info_.width = probed_w;
        stream_info_.height = probed_h;
        stream_info_.frame_rate = media.fps > 0 ? static_cast<double>(media.fps) : 30.0;
        rtsp_decoder_prefix_ = internal::BuildDecoderConfigPrefix(media, stream_codec);
        rtsp_decoder_config_sent_ = false;
        return true;
    }

    bool StartRtspPlayback() noexcept {
        if (!PrepareRtspSession()) {
            return false;
        }

        if (!rtsp_client_.setup(0) || !rtsp_client_.play(0)) {
            CloseRtspSession();
            return false;
        }

        return true;
    }

    bool PrimeRtspStreamInfo() noexcept {
        // Try to discover parameter sets and coded size from the stream itself.
        // Some RTSP servers (e.g. certain cameras) omit video fmtp/framesize in SDP.
        if (!rtsp_client_.isPlaying()) {
            return false;
        }

        std::vector<std::uint8_t> vps;
        std::vector<std::uint8_t> sps;
        std::vector<std::uint8_t> pps;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        std::uint32_t probed_w = 0;
        std::uint32_t probed_h = 0;
        while (std::chrono::steady_clock::now() < deadline && !interrupted_.load(std::memory_order_relaxed)) {
            rtsp::VideoFrame frame{};
            if (!rtsp_client_.receiveFrame(frame, 100)) {
                continue;
            }
            if (frame.data == nullptr || frame.size == 0) {
                continue;
            }

            std::vector<std::uint8_t> annexb(frame.data, frame.data + frame.size);
            internal::UpdateCodecConfig(stream_info_.codec, annexb, &vps, &sps, &pps);

            if ((probed_w == 0 || probed_h == 0) && !sps.empty()) {
                if (stream_info_.codec == codec::VideoCodecType::kH264) {
                    (void)ParseH264SpsCodedSize(sps, &probed_w, &probed_h);
                } else if (stream_info_.codec == codec::VideoCodecType::kH265) {
                    (void)ParseH265SpsCodedSize(sps, &probed_w, &probed_h);
                }
            }

            if (internal::HasCodecConfig(stream_info_.codec, vps, sps, pps) && probed_w != 0 && probed_h != 0) {
                break;
            }
        }

        if (internal::HasCodecConfig(stream_info_.codec, vps, sps, pps)) {
            rtsp_decoder_prefix_.clear();
            if (stream_info_.codec == codec::VideoCodecType::kH265) {
                internal::AppendAnnexBNalu(vps, &rtsp_decoder_prefix_);
            }
            internal::AppendAnnexBNalu(sps, &rtsp_decoder_prefix_);
            internal::AppendAnnexBNalu(pps, &rtsp_decoder_prefix_);
            rtsp_decoder_config_sent_ = false;
        }

        if (probed_w != 0 && probed_h != 0) {
            if (ShouldOverrideStreamSize(stream_info_.width, stream_info_.height, probed_w, probed_h)) {
                stream_info_.width = probed_w;
                stream_info_.height = probed_h;
            }
        }

        return true;
    }

    void CloseRtspSession() noexcept {
        // Avoid spamming RTSP close logs for non-RTSP inputs (local MP4) where the client was never connected.
        if (rtsp_client_.isConnected() || rtsp_client_.isPlaying()) {
            rtsp_client_.interrupt();
            (void)rtsp_client_.closeWithTimeout(2000);
        }
        rtsp_session_ = {};
        rtsp_decoder_prefix_.clear();
        rtsp_decoder_config_sent_ = false;
    }

    void ResetPlaybackState() noexcept {
        next_due_ = std::chrono::steady_clock::now();
        first_packet_ = true;
        packet_pts_offset_us_ = 0;
        current_loop_span_us_ = 0;
        emitted_pts_cursor_us_ = 0;
    }

    void AdvanceLoopPlaybackState() noexcept {
        packet_pts_offset_us_ += current_loop_span_us_ == 0 ? ResolveFallbackFrameDurationUs() : current_loop_span_us_;
        current_loop_span_us_ = 0;
        emitted_pts_cursor_us_ = 0;
        next_due_ = std::chrono::steady_clock::now();
        first_packet_ = true;
    }

    DemuxerConfig config_{};
    DemuxerInputType type_{DemuxerInputType::kUnknown};
    codec::Mp4VideoInfo video_info_{};
    codec::VideoStreamInfo stream_info_{};
    std::unique_ptr<codec::AxMp4Demuxer> demuxer_;
    std::chrono::steady_clock::time_point next_due_{};
    bool first_packet_{true};
    std::uint64_t packet_pts_offset_us_{0};
    std::uint64_t current_loop_span_us_{0};
    std::uint64_t emitted_pts_cursor_us_{0};

    rtsp::RtspClient rtsp_client_;
    rtsp::SessionInfo rtsp_session_{};
    std::vector<std::uint8_t> rtsp_decoder_prefix_;
    bool rtsp_decoder_config_sent_{false};
    std::chrono::steady_clock::time_point last_rtsp_frame_{};
    std::chrono::steady_clock::time_point last_rtsp_keepalive_{};
    std::chrono::steady_clock::time_point next_rtsp_reconnect_{};
    std::chrono::milliseconds rtsp_reconnect_backoff_{kRtspReconnectBackoffMin};
    std::atomic<bool> interrupted_{false};
};

}  // namespace

bool DetectDemuxerInputType(const std::string& uri, DemuxerInputType* type) noexcept {
    if (type == nullptr) {
        return false;
    }

    *type = DemuxerInputType::kUnknown;
    const auto normalized = TrimAsciiWhitespaceCopy(uri);
    if (normalized.empty()) {
        return false;
    }

    const auto scheme_pos = normalized.find("://");
    if (scheme_pos != std::string::npos) {
        const auto scheme = ToLowerCopy(normalized.substr(0, scheme_pos));
        if (scheme == "rtsp" || scheme == "rtsps") {
            *type = DemuxerInputType::kRtspPull;
            return true;
        }

        if (scheme == "file") {
            const auto file_path = StripUriQueryAndFragment(normalized.substr(scheme_pos + 3U));
            if (EndsWithIgnoreCase(file_path, ".mp4")) {
                *type = DemuxerInputType::kMp4File;
                return true;
            }
            return false;
        }
    }

    const auto path = StripUriQueryAndFragment(normalized);
    if (EndsWithIgnoreCase(path, ".mp4")) {
        *type = DemuxerInputType::kMp4File;
        return true;
    }
    return false;
}

std::unique_ptr<Demuxer> CreateDemuxer() {
    return std::make_unique<AxDemuxer>();
}

}  // namespace axvsdk::pipeline
