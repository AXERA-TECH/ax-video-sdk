#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ax_cmdline_utils.h"
#include <rtsp-client/rtsp_client.h>
#include <rtsp-publisher/rtsp_publisher.h>
#include <rtsp-server/rtsp_server.h>

#include "codec/ax_video_decoder.h"
#include "codec/ax_video_encoder.h"
#include "common/ax_image_processor.h"
#include "common/ax_system.h"

namespace {

using namespace axvsdk;

struct AnnexBNalu {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    std::uint8_t type{0};
};

codec::VideoCodecType ParseCodec(const char* value) {
    if (value == nullptr) {
        return codec::VideoCodecType::kH264;
    }

    const std::string text(value);
    if (text == "h265" || text == "265" || text == "hevc") {
        return codec::VideoCodecType::kH265;
    }
    return codec::VideoCodecType::kH264;
}

codec::VideoCodecType ToSdkCodec(rtsp::CodecType codec) {
    return codec == rtsp::CodecType::H265 ? codec::VideoCodecType::kH265 : codec::VideoCodecType::kH264;
}

rtsp::CodecType ToRtspCodec(codec::VideoCodecType codec) {
    return codec == codec::VideoCodecType::kH265 ? rtsp::CodecType::H265 : rtsp::CodecType::H264;
}

struct RtspListenTarget {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8554};
    std::string path{"/live"};
};

bool ParseRtspUrl(const std::string& url, RtspListenTarget* target) {
    if (target == nullptr || url.rfind("rtsp://", 0) != 0) {
        return false;
    }

    const auto remainder = url.substr(7);
    const auto slash = remainder.find('/');
    const auto host_port = slash == std::string::npos ? remainder : remainder.substr(0, slash);
    target->path = slash == std::string::npos ? "/live" : remainder.substr(slash);
    if (target->path.empty()) {
        target->path = "/live";
    }

    const auto colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        target->host = host_port;
        target->port = 554;
        return true;
    }

    target->host = host_port.substr(0, colon);
    target->port = static_cast<std::uint16_t>(std::stoi(host_port.substr(colon + 1)));
    return true;
}

std::string MakePublisherUrl(const RtspListenTarget& target) {
    const std::string host = (target.host.empty() || target.host == "0.0.0.0" || target.host == "::")
                                 ? "127.0.0.1"
                                 : target.host;
    return "rtsp://" + host + ":" + std::to_string(target.port) + target.path;
}

bool IsStartCode3(const std::uint8_t* data) noexcept {
    return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01;
}

bool IsStartCode4(const std::uint8_t* data) noexcept {
    return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01;
}

std::vector<AnnexBNalu> ParseAnnexBNalus(const std::vector<std::uint8_t>& data,
                                         codec::VideoCodecType codec) {
    std::vector<AnnexBNalu> nalus;
    const auto* bytes = data.data();
    const auto size = data.size();
    std::size_t offset = 0;

    auto find_start = [&](std::size_t begin) -> std::pair<std::size_t, std::size_t> {
        for (std::size_t i = begin; i + 3 <= size; ++i) {
            if (i + 4 <= size && IsStartCode4(bytes + i)) {
                return {i, 4};
            }
            if (i + 3 <= size && IsStartCode3(bytes + i)) {
                return {i, 3};
            }
        }
        return {size, 0};
    };

    while (true) {
        const auto [start, prefix] = find_start(offset);
        if (prefix == 0) {
            break;
        }

        const std::size_t payload_start = start + prefix;
        auto next = find_start(payload_start);
        std::size_t next_start = next.first;
        if (next_start == size) {
            next_start = size;
        }
        if (payload_start >= next_start) {
            offset = payload_start;
            continue;
        }

        const auto* nalu = bytes + payload_start;
        const auto nalu_size = next_start - payload_start;
        std::uint8_t type = 0;
        if (codec == codec::VideoCodecType::kH264) {
            type = static_cast<std::uint8_t>(nalu[0] & 0x1FU);
        } else if (codec == codec::VideoCodecType::kH265 && nalu_size >= 2) {
            type = static_cast<std::uint8_t>((nalu[0] >> 1U) & 0x3FU);
        }

        nalus.push_back({nalu, nalu_size, type});
        offset = next_start;
    }

    return nalus;
}

void AppendAnnexBNalu(const std::vector<std::uint8_t>& nalu, std::vector<std::uint8_t>* output) {
    if (output == nullptr || nalu.empty()) {
        return;
    }

    static constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    output->insert(output->end(), std::begin(kStartCode), std::end(kStartCode));
    output->insert(output->end(), nalu.begin(), nalu.end());
}

void UpdateCodecConfig(codec::VideoCodecType codec,
                       const std::vector<std::uint8_t>& annexb,
                       std::vector<std::uint8_t>* vps,
                       std::vector<std::uint8_t>* sps,
                       std::vector<std::uint8_t>* pps) {
    if (sps == nullptr || pps == nullptr || (codec == codec::VideoCodecType::kH265 && vps == nullptr)) {
        return;
    }

    for (const auto& nalu : ParseAnnexBNalus(annexb, codec)) {
        if (codec == codec::VideoCodecType::kH264) {
            if (nalu.type == 7) {
                sps->assign(nalu.data, nalu.data + nalu.size);
            } else if (nalu.type == 8) {
                pps->assign(nalu.data, nalu.data + nalu.size);
            }
        } else if (codec == codec::VideoCodecType::kH265) {
            if (nalu.type == 32) {
                vps->assign(nalu.data, nalu.data + nalu.size);
            } else if (nalu.type == 33) {
                sps->assign(nalu.data, nalu.data + nalu.size);
            } else if (nalu.type == 34) {
                pps->assign(nalu.data, nalu.data + nalu.size);
            }
        }
    }
}

bool HasCodecConfig(codec::VideoCodecType codec,
                    const std::vector<std::uint8_t>& vps,
                    const std::vector<std::uint8_t>& sps,
                    const std::vector<std::uint8_t>& pps) {
    if (codec == codec::VideoCodecType::kH264) {
        return !sps.empty() && !pps.empty();
    }
    if (codec == codec::VideoCodecType::kH265) {
        return !vps.empty() && !sps.empty() && !pps.empty();
    }
    return false;
}

int Run(const char* pull_url,
        const char* publish_url,
        codec::VideoCodecType output_codec,
        std::uint32_t output_width,
        std::uint32_t output_height,
        int expected_frames,
        int timeout_seconds) {
    common::SystemOptions system_options{};
    system_options.enable_vdec = true;
    system_options.enable_venc = true;
    system_options.enable_ivps = true;
    if (!common::InitializeSystem(system_options)) {
        std::cerr << "InitializeSystem failed\n";
        return 2;
    }

    struct Guard {
        ~Guard() {
            common::ShutdownSystem();
        }
    } guard;

    rtsp::RtspClient client;
    rtsp::RtspClientConfig client_config{};
    client_config.prefer_tcp_transport = true;
    client_config.fallback_to_tcp = true;
    client_config.buffer_size = 120;
    client.setConfig(client_config);

    if (!client.open(pull_url)) {
        std::cerr << "rtsp client open failed\n";
        return 3;
    }
    if (!client.describe()) {
        client.close();
        std::cerr << "rtsp client describe failed\n";
        return 4;
    }

    const auto session = client.getSessionInfo();
    if (!session.has_video || session.media_streams.empty()) {
        client.close();
        std::cerr << "rtsp session has no video stream\n";
        return 5;
    }

    const auto& media = session.media_streams.front();
    auto decoder = codec::CreateVideoDecoder();
    auto encoder = codec::CreateVideoEncoder();
    if (!decoder || !encoder) {
        client.close();
        std::cerr << "Create codec instances failed\n";
        return 6;
    }

    codec::VideoDecoderConfig decoder_config{};
    decoder_config.stream.codec = ToSdkCodec(media.codec);
    decoder_config.stream.width = media.width;
    decoder_config.stream.height = media.height;
    decoder_config.stream.frame_rate = media.fps > 0 ? static_cast<double>(media.fps) : 30.0;
    if (!decoder->Open(decoder_config)) {
        client.close();
        std::cerr << "decoder Open failed\n";
        return 7;
    }

    codec::VideoEncoderConfig encoder_config{};
    encoder_config.codec = output_codec;
    encoder_config.width = output_width == 0 ? media.width : output_width;
    encoder_config.height = output_height == 0 ? media.height : output_height;
    encoder_config.frame_rate = media.fps > 0 ? static_cast<double>(media.fps) : 30.0;
    encoder_config.gop = static_cast<std::uint32_t>(encoder_config.frame_rate > 1.0 ? encoder_config.frame_rate
                                                                                    : 30.0);
    encoder_config.bitrate_kbps = 4096;
    encoder_config.input_queue_depth = 10;
    if (!encoder->Open(encoder_config)) {
        decoder->Close();
        client.close();
        std::cerr << "encoder Open failed\n";
        return 8;
    }

    auto processor = common::CreateImageProcessor();
    if (!processor) {
        encoder->Close();
        decoder->Close();
        client.close();
        std::cerr << "CreateImageProcessor failed\n";
        return 8;
    }

    RtspListenTarget publish_target{};
    if (!ParseRtspUrl(publish_url, &publish_target)) {
        encoder->Close();
        decoder->Close();
        client.close();
        std::cerr << "invalid publish_url\n";
        return 9;
    }

    rtsp::RtspServer server;
    rtsp::PathConfig path_config{};
    path_config.path = publish_target.path;
    path_config.codec = ToRtspCodec(output_codec);
    path_config.width = encoder_config.width;
    path_config.height = encoder_config.height;
    path_config.fps = static_cast<std::uint32_t>(
        std::lround(encoder_config.frame_rate > 0.0 ? encoder_config.frame_rate : 30.0));

    const std::string server_bind_host = publish_target.host.empty() ? "0.0.0.0" : publish_target.host;
    const bool server_ready =
        server.init(server_bind_host, publish_target.port) &&
        server.addPath(path_config) &&
        server.start();

    rtsp::RtspPublisher publisher;
    rtsp::RtspPublishConfig publisher_config{};
    publisher_config.local_rtp_port = 0;
    publisher.setConfig(publisher_config);

    const bool use_server_output = server_ready;
    const std::string publisher_url = MakePublisherUrl(publish_target);
    const char* output_mode = use_server_output ? "server" : "publisher";

    std::atomic<bool> server_failed{false};

    std::atomic<int> received_packets{0};
    std::atomic<int> decoded_frames{0};
    std::atomic<int> submitted_frames{0};
    std::atomic<int> dropped_frames{0};
    std::atomic<int> encoded_packets{0};
    std::atomic<int> pushed_packets{0};
    std::atomic<int> key_packets{0};
    std::atomic<int> input_errors{0};
    std::atomic<int> server_errors{0};
    std::atomic<int> publisher_fallback_used{use_server_output ? 0 : 1};
    std::atomic<std::size_t> latest_packet_bytes{0};
    std::atomic<bool> reader_loop_done{false};
    std::mutex output_mutex;
    std::vector<std::uint8_t> output_vps;
    std::vector<std::uint8_t> output_sps;
    std::vector<std::uint8_t> output_pps;
    bool decoder_config_sent = false;
    std::vector<std::uint8_t> decoder_prefix;
    if (decoder_config.stream.codec == codec::VideoCodecType::kH265) {
        AppendAnnexBNalu(media.vps, &decoder_prefix);
    }
    AppendAnnexBNalu(media.sps, &decoder_prefix);
    AppendAnnexBNalu(media.pps, &decoder_prefix);

    client.setErrorCallback([&](const std::string& error) {
        input_errors.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "rtsp client error: " << error << "\n";
    });

    encoder->SetPacketCallback([&](codec::EncodedPacket packet) {
        latest_packet_bytes.store(packet.data.size(), std::memory_order_relaxed);
        encoded_packets.fetch_add(1, std::memory_order_relaxed);
        if (packet.key_frame) {
            key_packets.fetch_add(1, std::memory_order_relaxed);
        }

        const auto pts_ms = packet.pts / 1000ULL;
        bool pushed = false;

        std::lock_guard<std::mutex> lock(output_mutex);
        if (use_server_output) {
            pushed = output_codec == codec::VideoCodecType::kH265
                         ? server.pushH265Data(publish_target.path,
                                               packet.data.data(),
                                               packet.data.size(),
                                               pts_ms,
                                               packet.key_frame)
                         : server.pushH264Data(publish_target.path,
                                               packet.data.data(),
                                               packet.data.size(),
                                               pts_ms,
                                               packet.key_frame);
        } else {
            UpdateCodecConfig(output_codec, packet.data, &output_vps, &output_sps, &output_pps);
            const bool publisher_ready = publisher.isRecording();
            if (!publisher_ready && packet.key_frame &&
                HasCodecConfig(output_codec, output_vps, output_sps, output_pps)) {
                rtsp::PublishMediaInfo media_info{};
                media_info.codec = ToRtspCodec(output_codec);
                media_info.width = encoder_config.width;
                media_info.height = encoder_config.height;
                media_info.fps = path_config.fps;
                media_info.payload_type = output_codec == codec::VideoCodecType::kH265 ? 97 : 96;
                media_info.vps = output_vps;
                media_info.sps = output_sps;
                media_info.pps = output_pps;
                media_info.control_track = "streamid=0";

                if (!publisher.isConnected() && !publisher.open(publisher_url)) {
                    server_failed.store(true, std::memory_order_relaxed);
                    server_errors.fetch_add(1, std::memory_order_relaxed);
                } else if (!publisher.announce(media_info) || !publisher.setup() || !publisher.record()) {
                    server_failed.store(true, std::memory_order_relaxed);
                    server_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (publisher.isRecording()) {
                pushed = output_codec == codec::VideoCodecType::kH265
                             ? publisher.pushH265Data(packet.data.data(),
                                                      packet.data.size(),
                                                      pts_ms,
                                                      packet.key_frame)
                             : publisher.pushH264Data(packet.data.data(),
                                                      packet.data.size(),
                                                      pts_ms,
                                                      packet.key_frame);
            }
        }

        if (pushed) {
            pushed_packets.fetch_add(1, std::memory_order_relaxed);
        } else if (use_server_output || publisher.isRecording()) {
            server_failed.store(true, std::memory_order_relaxed);
            server_errors.fetch_add(1, std::memory_order_relaxed);
        }
    });

    decoder->SetFrameCallback([&](common::AxImage::Ptr frame) {
        decoded_frames.fetch_add(1, std::memory_order_relaxed);
        common::AxImage::Ptr frame_to_encode = frame;
        if (frame_to_encode &&
            (frame_to_encode->format() != common::PixelFormat::kNv12 ||
             frame_to_encode->width() != encoder_config.width ||
             frame_to_encode->height() != encoder_config.height)) {
            common::ImageProcessRequest request{};
            request.output_image.format = common::PixelFormat::kNv12;
            request.output_image.width = encoder_config.width;
            request.output_image.height = encoder_config.height;
            frame_to_encode = processor->Process(*frame_to_encode, request);
        }

        if (frame_to_encode && encoder->SubmitFrame(std::move(frame_to_encode))) {
            submitted_frames.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }, codec::FrameCallbackMode::kQueue);

    if (!encoder->Start()) {
        publisher.close();
        server.stop();
        encoder->Close();
        decoder->Close();
        client.close();
        std::cerr << "encoder Start failed\n";
        return 11;
    }
    if (!decoder->Start()) {
        encoder->Stop();
        publisher.close();
        server.stop();
        encoder->Close();
        decoder->Close();
        client.close();
        std::cerr << "decoder Start failed\n";
        return 12;
    }
    if (!client.setup(0) || !client.play(0)) {
        decoder->Stop();
        encoder->Stop();
        publisher.close();
        server.stop();
        encoder->Close();
        decoder->Close();
        client.close();
        std::cerr << "rtsp client setup/play failed\n";
        return 13;
    }

    std::thread reader_thread([&]() {
        while (client.isPlaying()) {
            rtsp::VideoFrame frame;
            if (!client.receiveFrame(frame, 500)) {
                continue;
            }

            codec::EncodedPacket packet{};
            packet.codec = ToSdkCodec(frame.codec);
            packet.pts = frame.pts * 1000ULL;
            packet.duration = frame.fps > 0 ? (1000000ULL / frame.fps) : 0;
            packet.key_frame = frame.type == rtsp::FrameType::IDR;
            const bool inject_decoder_config = !decoder_config_sent || packet.key_frame;
            const std::size_t prefix_size = inject_decoder_config ? decoder_prefix.size() : 0U;
            packet.data.reserve(prefix_size + frame.size);
            if (inject_decoder_config && !decoder_prefix.empty()) {
                packet.data.insert(packet.data.end(), decoder_prefix.begin(), decoder_prefix.end());
            }
            if (frame.data != nullptr && frame.size != 0) {
                packet.data.insert(packet.data.end(), frame.data, frame.data + frame.size);
            }
            decoder_config_sent = decoder_config_sent || inject_decoder_config;

            if (decoder->SubmitPacket(std::move(packet))) {
                received_packets.fetch_add(1, std::memory_order_relaxed);
            } else {
                input_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        reader_loop_done.store(true, std::memory_order_relaxed);
    });

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const auto decoded = decoded_frames.load(std::memory_order_relaxed);
        const auto pushed = pushed_packets.load(std::memory_order_relaxed);
        if (decoded >= expected_frames && pushed > 0) {
            break;
        }

        if (server_failed.load(std::memory_order_relaxed)) {
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.closeWithTimeout(2000);
    if (reader_thread.joinable()) {
        reader_thread.join();
    }

    (void)decoder->SubmitEndOfStream();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    decoder->Stop();
    decoder->Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    encoder->Stop();
    encoder->Close();
    publisher.close();
    server.stop();

    const auto stats = client.getStats();
    const auto server_stats = server.getStats();

    std::cout << "input_codec=" << static_cast<int>(ToSdkCodec(media.codec)) << "\n";
    std::cout << "input_width=" << media.width << "\n";
    std::cout << "input_height=" << media.height << "\n";
    std::cout << "input_fps=" << media.fps << "\n";
    std::cout << "output_mode=" << output_mode << "\n";
    std::cout << "publisher_url=" << publisher_url << "\n";
    std::cout << "output_width=" << encoder_config.width << "\n";
    std::cout << "output_height=" << encoder_config.height << "\n";
    std::cout << "received_packets=" << received_packets.load(std::memory_order_relaxed) << "\n";
    std::cout << "decoded_frames=" << decoded_frames.load(std::memory_order_relaxed) << "\n";
    std::cout << "submitted_frames=" << submitted_frames.load(std::memory_order_relaxed) << "\n";
    std::cout << "dropped_frames=" << dropped_frames.load(std::memory_order_relaxed) << "\n";
    std::cout << "encoded_packets=" << encoded_packets.load(std::memory_order_relaxed) << "\n";
    std::cout << "pushed_packets=" << pushed_packets.load(std::memory_order_relaxed) << "\n";
    std::cout << "key_packets=" << key_packets.load(std::memory_order_relaxed) << "\n";
    std::cout << "latest_packet_bytes=" << latest_packet_bytes.load(std::memory_order_relaxed) << "\n";
    std::cout << "client_frames_output=" << stats.frames_output << "\n";
    std::cout << "client_rtp_packets_received=" << stats.rtp_packets_received << "\n";
    std::cout << "client_using_tcp=" << static_cast<int>(stats.using_tcp_transport) << "\n";
    std::cout << "server_frames_pushed=" << server_stats.frames_pushed << "\n";
    std::cout << "server_sessions_created=" << server_stats.sessions_created << "\n";
    std::cout << "publisher_fallback_used=" << publisher_fallback_used.load(std::memory_order_relaxed) << "\n";
    std::cout << "server_failed=" << static_cast<int>(server_failed.load(std::memory_order_relaxed)) << "\n";
    std::cout << "input_errors=" << input_errors.load(std::memory_order_relaxed) << "\n";
    std::cout << "server_errors=" << server_errors.load(std::memory_order_relaxed) << "\n";
    std::cout << "reader_loop_done=" << static_cast<int>(reader_loop_done.load(std::memory_order_relaxed)) << "\n";

    if (server_failed.load(std::memory_order_relaxed)) {
        return 14;
    }
    if (received_packets.load(std::memory_order_relaxed) <= 0 ||
        decoded_frames.load(std::memory_order_relaxed) <= 0 ||
        submitted_frames.load(std::memory_order_relaxed) <= 0 ||
        encoded_packets.load(std::memory_order_relaxed) <= 0 ||
        pushed_packets.load(std::memory_order_relaxed) <= 0) {
        return 15;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_rtsp_transcode_smoke");
    parser.add<std::string>("input", 'i', "pull RTSP URL", false, "");
    parser.add<std::string>("output", 'o', "push RTSP URL", false, "");
    parser.add<std::string>("codec", 'c', "output codec: h264|h265", false, "h264");
    parser.add<int>("width", 'w', "output width", false, 0);
    parser.add<int>("height", 0, "output height", false, 0);
    parser.add<int>("expected-frames", 'n', "expected decoded frames", false, 30);
    parser.add<int>("timeout", 't', "timeout seconds", false, 30);

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_url;
    std::string output_url;
    std::string codec_name{"h264"};
    int output_width = 0;
    int output_height = 0;
    int expected_frames = 30;
    int timeout_seconds = 30;
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_url, std::cerr) ||
        !tooling::GetRequiredArgument(parser, "output", 1, "output", &output_url, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "codec", 2, std::string("h264"), &codec_name, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "width", 3, 0, &output_width, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "height", 4, 0, &output_height, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "expected-frames", 5, 30, &expected_frames, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "timeout", 6, 30, &timeout_seconds, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }

    return Run(input_url.c_str(),
               output_url.c_str(),
               ParseCodec(codec_name.c_str()),
               static_cast<std::uint32_t>(output_width),
               static_cast<std::uint32_t>(output_height),
               expected_frames,
               timeout_seconds);
}
