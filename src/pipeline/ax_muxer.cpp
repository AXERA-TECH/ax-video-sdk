#include "pipeline/ax_muxer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rtsp-publisher/rtsp_publisher.h>
#include <rtsp-server/rtsp_server.h>

#include "ax_mp4_internal.h"
#include "ax_rtsp_internal.h"

namespace axvsdk::pipeline {

namespace {

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool EndsWithIgnoreCase(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }

    return ToLowerCopy(value.substr(value.size() - suffix.size())) == ToLowerCopy(suffix);
}

std::uint32_t ResolveRtspFps(const codec::VideoStreamInfo& stream) noexcept {
    const double fps = stream.frame_rate > 0.0 ? stream.frame_rate : 30.0;
    return static_cast<std::uint32_t>(std::lround(fps));
}

class RtspPtsUnwrapper {
public:
    explicit RtspPtsUnwrapper(double fps) noexcept
        : step_us_(ResolveStepUs(fps)) {}

    std::uint64_t ToMonotonicPtsMs(std::uint64_t pts_us) noexcept {
        const std::uint64_t in_us = pts_us;
        if (!last_out_us_.has_value()) {
            last_out_us_ = in_us;
            return RoundToMs(in_us);
        }

        // VENC may emit packets in coding order with PTS that occasionally goes backwards
        // (B-frames) or discontinuities (loop playback). Some RTSP clients (ffmpeg/ffplay)
        // will tear down the connection when RTP timestamps are non-monotonic. Convert PTS
        // to a monotonically increasing timeline for RTP timestamping.
        std::uint64_t candidate = in_us + offset_us_;
        const std::uint64_t min_next = last_out_us_.value() + step_us_;
        if (candidate < min_next) {
            offset_us_ = min_next > in_us ? (min_next - in_us) : 0U;
            candidate = in_us + offset_us_;
        }

        last_out_us_ = candidate;
        return RoundToMs(candidate);
    }

private:
    static std::uint64_t ResolveStepUs(double fps) noexcept {
        if (fps <= 0.0) {
            return 33333ULL;
        }
        const auto step = static_cast<std::uint64_t>(std::llround(1000000.0 / fps));
        return step == 0 ? 1ULL : step;
    }

    static std::uint64_t RoundToMs(std::uint64_t us) noexcept {
        return (us + 500ULL) / 1000ULL;
    }

    std::optional<std::uint64_t> last_out_us_{};
    std::uint64_t offset_us_{0};
    std::uint64_t step_us_{33333};
};

class PacketSink {
public:
    virtual ~PacketSink() = default;

    virtual void Close() noexcept = 0;
    virtual bool SubmitPacket(const codec::EncodedPacket& packet) = 0;
};

class ElementaryStreamFileSink final : public PacketSink {
public:
    explicit ElementaryStreamFileSink(std::ofstream output)
        : output_(std::move(output)) {}

    void Close() noexcept override {
        if (output_.is_open()) {
            output_.close();
        }
    }

    bool SubmitPacket(const codec::EncodedPacket& packet) override {
        if (!output_.is_open() || packet.data.empty()) {
            return false;
        }

        output_.write(reinterpret_cast<const char*>(packet.data.data()),
                      static_cast<std::streamsize>(packet.data.size()));
        return static_cast<bool>(output_);
    }

private:
    std::ofstream output_;
};

class Mp4FileSink final : public PacketSink {
public:
    explicit Mp4FileSink(std::unique_ptr<codec::internal::Mp4FileMuxer> muxer)
        : muxer_(std::move(muxer)) {}

    void Close() noexcept override {
        if (muxer_) {
            muxer_->Close();
            muxer_.reset();
        }
    }

    bool SubmitPacket(const codec::EncodedPacket& packet) override {
        return muxer_ && muxer_->WritePacket(packet);
    }

private:
    std::unique_ptr<codec::internal::Mp4FileMuxer> muxer_;
};

class RtspServerSink final : public PacketSink {
public:
    RtspServerSink(std::shared_ptr<rtsp::RtspServer> server,
                   internal::RtspUrlTarget target,
                   codec::VideoStreamInfo stream)
        : server_(std::move(server)),
          rtsp_target_(std::move(target)),
          stream_(stream),
          pts_unwrapper_(stream.frame_rate) {}

    ~RtspServerSink() override {
        Close();
    }

    void Close() noexcept override {
        if (server_ != nullptr && !rtsp_target_.path.empty()) {
            server_->removePath(rtsp_target_.path);
        }
        server_.reset();
        rtsp_target_ = {};
        stream_ = {};
    }

    bool SubmitPacket(const codec::EncodedPacket& packet) override {
        if (server_ == nullptr || packet.data.empty()) {
            return false;
        }

        const auto packet_codec =
            packet.codec == codec::VideoCodecType::kUnknown ? stream_.codec : packet.codec;
        const auto pts_ms = pts_unwrapper_.ToMonotonicPtsMs(packet.pts);
        if (packet_codec == codec::VideoCodecType::kH265) {
            return server_->pushH265Data(rtsp_target_.path, packet.data.data(), packet.data.size(), pts_ms,
                                         packet.key_frame);
        }
        if (packet_codec == codec::VideoCodecType::kH264) {
            return server_->pushH264Data(rtsp_target_.path, packet.data.data(), packet.data.size(), pts_ms,
                                         packet.key_frame);
        }
        return false;
    }

private:
    std::shared_ptr<rtsp::RtspServer> server_;
    internal::RtspUrlTarget rtsp_target_{};
    codec::VideoStreamInfo stream_{};
    RtspPtsUnwrapper pts_unwrapper_;
};

class RtspPublisherSink final : public PacketSink {
public:
    RtspPublisherSink(codec::VideoStreamInfo stream, std::string publisher_url)
        : stream_(stream),
          publisher_url_(std::move(publisher_url)),
          pts_unwrapper_(stream.frame_rate) {
        rtsp::RtspPublishConfig publisher_config{};
        publisher_config.local_rtp_port = 0;
        publisher_.setConfig(publisher_config);
    }

    ~RtspPublisherSink() override {
        Close();
    }

    void Close() noexcept override {
        if (publisher_.isConnected()) {
            publisher_.close();
        }
        output_vps_.clear();
        output_sps_.clear();
        output_pps_.clear();
    }

    bool SubmitPacket(const codec::EncodedPacket& packet) override {
        if (packet.data.empty()) {
            return false;
        }

        const auto packet_codec =
            packet.codec == codec::VideoCodecType::kUnknown ? stream_.codec : packet.codec;
        if (packet_codec != codec::VideoCodecType::kH264 && packet_codec != codec::VideoCodecType::kH265) {
            return false;
        }

        internal::UpdateCodecConfig(packet_codec, packet.data, &output_vps_, &output_sps_, &output_pps_);
        if (!publisher_.isRecording()) {
            if (!packet.key_frame ||
                !internal::HasCodecConfig(packet_codec, output_vps_, output_sps_, output_pps_)) {
                return true;
            }

            rtsp::PublishMediaInfo media_info{};
            media_info.codec = internal::ToRtspCodec(packet_codec);
            media_info.width = stream_.width;
            media_info.height = stream_.height;
            media_info.fps = ResolveRtspFps(stream_);
            media_info.payload_type = packet_codec == codec::VideoCodecType::kH265 ? 97 : 96;
            media_info.vps = output_vps_;
            media_info.sps = output_sps_;
            media_info.pps = output_pps_;
            media_info.control_track = "streamid=0";

            if (!publisher_.isConnected() && !publisher_.open(publisher_url_)) {
                return false;
            }
            if (!publisher_.announce(media_info) || !publisher_.setup() || !publisher_.record()) {
                return false;
            }
        }

        const auto pts_ms = pts_unwrapper_.ToMonotonicPtsMs(packet.pts);
        return packet_codec == codec::VideoCodecType::kH265
                   ? publisher_.pushH265Data(packet.data.data(), packet.data.size(), pts_ms, packet.key_frame)
                   : publisher_.pushH264Data(packet.data.data(), packet.data.size(), pts_ms, packet.key_frame);
    }

private:
    codec::VideoStreamInfo stream_{};
    rtsp::RtspPublisher publisher_;
    std::string publisher_url_;
    RtspPtsUnwrapper pts_unwrapper_;
    std::vector<std::uint8_t> output_vps_;
    std::vector<std::uint8_t> output_sps_;
    std::vector<std::uint8_t> output_pps_;
};

std::unique_ptr<PacketSink> OpenElementaryStreamFileSink(const std::string& uri) {
    std::ofstream output(uri, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return nullptr;
    }
    return std::make_unique<ElementaryStreamFileSink>(std::move(output));
}

std::unique_ptr<PacketSink> OpenMp4Sink(const codec::VideoStreamInfo& stream, const std::string& uri) {
    if ((stream.codec != codec::VideoCodecType::kH264 && stream.codec != codec::VideoCodecType::kH265) ||
        stream.width == 0 || stream.height == 0) {
        return nullptr;
    }

    auto muxer = codec::internal::Mp4FileMuxer::Open(uri, stream);
    if (!muxer) {
        return nullptr;
    }

    return std::make_unique<Mp4FileSink>(std::move(muxer));
}

std::unique_ptr<PacketSink> OpenRtspSink(const codec::VideoStreamInfo& stream, const std::string& uri) {
    if ((stream.codec != codec::VideoCodecType::kH264 && stream.codec != codec::VideoCodecType::kH265) ||
        stream.width == 0 || stream.height == 0) {
        return nullptr;
    }

    internal::RtspUrlTarget target{};
    if (!internal::ParseRtspUrl(uri, &target)) {
        return nullptr;
    }

    auto server = rtsp::getOrCreateRtspServer(target.port, target.host.empty() ? "0.0.0.0" : target.host);
    if (server != nullptr) {
        rtsp::PathConfig path_config{};
        path_config.path = target.path;
        path_config.codec = internal::ToRtspCodec(stream.codec);
        path_config.width = stream.width;
        path_config.height = stream.height;
        path_config.fps = ResolveRtspFps(stream);

        if (server->addPath(path_config)) {
            if (server->isRunning() || server->start()) {
                return std::make_unique<RtspServerSink>(std::move(server), std::move(target), stream);
            }
            server->removePath(target.path);
        }
    }

    return std::make_unique<RtspPublisherSink>(stream, internal::MakePublisherUrl(target));
}

std::unique_ptr<PacketSink> OpenSink(const MuxerConfig& config, const std::string& uri) {
    if (uri.empty()) {
        return nullptr;
    }

    if (internal::IsRtspUrl(uri)) {
        return OpenRtspSink(config.stream, uri);
    }

    if (EndsWithIgnoreCase(uri, ".mp4")) {
        return OpenMp4Sink(config.stream, uri);
    }

    return OpenElementaryStreamFileSink(uri);
}

class AxMuxer final : public Muxer {
public:
    ~AxMuxer() override {
        Close();
    }

    bool Open(const MuxerConfig& config) override {
        Close();

        if (config.uris.empty()) {
            return false;
        }

        std::vector<std::unique_ptr<PacketSink>> sinks;
        sinks.reserve(config.uris.size());
        for (const auto& uri : config.uris) {
            auto sink = OpenSink(config, uri);
            if (!sink) {
                CloseSinks(&sinks);
                return false;
            }
            sinks.push_back(std::move(sink));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        sinks_ = std::move(sinks);
        return true;
    }

    void Close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseSinks(&sinks_);
        config_ = {};
    }

    bool SubmitPacket(codec::EncodedPacket packet) override {
        if (packet.data.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (sinks_.empty()) {
            return false;
        }

        if (sinks_.size() == 1U) {
            return sinks_.front()->SubmitPacket(packet);
        }

        bool success = true;
        for (auto& sink : sinks_) {
            // Keep sink packet ownership isolated. RTSP/server and file muxers are
            // third-party or opaque implementations, so sharing one packet buffer
            // across multiple sink submissions is not a safe assumption.
            codec::EncodedPacket sink_packet = packet;
            if (!sink->SubmitPacket(sink_packet)) {
                success = false;
            }
        }
        return success;
    }

private:
    static void CloseSinks(std::vector<std::unique_ptr<PacketSink>>* sinks) noexcept {
        if (sinks == nullptr) {
            return;
        }

        for (auto& sink : *sinks) {
            if (sink) {
                sink->Close();
            }
        }
        sinks->clear();
    }

    MuxerConfig config_{};
    std::mutex mutex_;
    std::vector<std::unique_ptr<PacketSink>> sinks_;
};

}  // namespace

std::unique_ptr<Muxer> CreateMuxer() {
    return std::make_unique<AxMuxer>();
}

}  // namespace axvsdk::pipeline
