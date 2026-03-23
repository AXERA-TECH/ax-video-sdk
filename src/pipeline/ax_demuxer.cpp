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
            if (!PrepareRtspSession()) {
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

                const auto packet_duration_us = ToMicroseconds(packet->duration, video_info_.timescale, video_info_.fps);
                const auto effective_duration_us =
                    packet_duration_us == 0 ? ResolveFallbackFrameDurationUs() : packet_duration_us;
                const auto packet_pts_us = ToMicroseconds(packet->pts, video_info_.timescale, video_info_.fps);

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
            rtsp::VideoFrame frame{};
            if (rtsp_client_.receiveFrame(frame, 200)) {
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
            if (!rtsp_client_.isPlaying()) {
                return false;
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
        if ((stream_codec != codec::VideoCodecType::kH264 && stream_codec != codec::VideoCodecType::kH265) || media.width == 0 ||
            media.height == 0) {
            CloseRtspSession();
            return false;
        }

        stream_info_.codec = stream_codec;
        stream_info_.width = media.width;
        stream_info_.height = media.height;
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

    void CloseRtspSession() noexcept {
        rtsp_client_.interrupt();
        (void)rtsp_client_.closeWithTimeout(2000);
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
