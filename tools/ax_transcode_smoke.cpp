#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "ax_cmdline_utils.h"
#include "ax_mp4_decode_util.h"
#include "codec/ax_video_decoder.h"
#include "codec/ax_video_encoder.h"
#include "common/ax_system.h"
#include "pipeline/ax_muxer.h"

namespace {

using namespace axvsdk;

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

int Run(const char* input_path,
        const char* output_path,
        codec::VideoCodecType output_codec,
        int device_id,
        int expected_decoded_frames,
        int timeout_seconds) {
    auto demuxer = tooling::OpenDemuxer(input_path, false);
    if (!demuxer) {
        std::cerr << "OpenDemuxer failed\n";
        return 2;
    }

    const auto video_info = demuxer->GetVideoStreamInfo();
    common::SystemOptions system_options{};
    system_options.device_id = device_id;
    system_options.enable_vdec = true;
    system_options.enable_venc = true;
    system_options.enable_ivps = false;
    if (!common::InitializeSystem(system_options)) {
        std::cerr << "InitializeSystem failed\n";
        return 3;
    }

    struct Guard {
        ~Guard() {
            common::ShutdownSystem();
        }
    } guard;

    auto decoder = codec::CreateVideoDecoder();
    auto encoder = codec::CreateVideoEncoder();
    auto muxer = pipeline::CreateMuxer();
    if (!decoder || !encoder || !muxer) {
        std::cerr << "Create codec instances failed\n";
        return 4;
    }

    auto decoder_config = tooling::MakeDecoderConfigFromStreamInfo(video_info);
    decoder_config.device_id = device_id;
    if (!decoder->Open(decoder_config)) {
        std::cerr << "decoder Open failed\n";
        return 5;
    }

    codec::VideoEncoderConfig encoder_config{};
    encoder_config.codec = output_codec;
    encoder_config.width = video_info.width;
    encoder_config.height = video_info.height;
    encoder_config.device_id = device_id;
    encoder_config.frame_rate = video_info.frame_rate > 0.0 ? video_info.frame_rate : 30.0;
    encoder_config.gop = static_cast<std::uint32_t>(encoder_config.frame_rate > 1.0 ? encoder_config.frame_rate
                                                                                    : 30.0);
    encoder_config.bitrate_kbps = 4096;
    encoder_config.input_queue_depth = 10;
    if (!encoder->Open(encoder_config)) {
        std::cerr << "encoder Open failed\n";
        decoder->Close();
        return 6;
    }

    if (output_path != nullptr && output_path[0] != '\0') {
        pipeline::MuxerConfig muxer_config{};
        muxer_config.stream.codec = output_codec;
        muxer_config.stream.width = encoder_config.width;
        muxer_config.stream.height = encoder_config.height;
        muxer_config.stream.frame_rate = encoder_config.frame_rate;
        muxer_config.uris.push_back(output_path);
        if (!muxer->Open(muxer_config)) {
            std::cerr << "muxer Open failed\n";
            encoder->Close();
            decoder->Close();
            return 7;
        }
    } else {
        muxer.reset();
    }

    std::atomic<int> decoded_frames{0};
    std::atomic<int> submitted_frames{0};
    std::atomic<int> dropped_frames{0};
    std::atomic<int> encoded_packets{0};
    std::atomic<int> key_packets{0};
    std::atomic<std::size_t> latest_packet_bytes{0};
    std::atomic<int> fed_packets{0};
    std::atomic<bool> feed_reached_eos{false};

    encoder->SetPacketCallback([&](codec::EncodedPacket packet) {
        if (muxer) {
            (void)muxer->SubmitPacket(packet);
        }
        latest_packet_bytes.store(packet.data.size(), std::memory_order_relaxed);
        if (packet.key_frame) {
            key_packets.fetch_add(1, std::memory_order_relaxed);
        }
        encoded_packets.fetch_add(1, std::memory_order_relaxed);
    });

    decoder->SetFrameCallback([&](common::AxImage::Ptr frame) {
        decoded_frames.fetch_add(1, std::memory_order_relaxed);
        if (encoder->SubmitFrame(std::move(frame))) {
            submitted_frames.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_frames.fetch_add(1, std::memory_order_relaxed);
        }
    });

    if (!encoder->Start()) {
        std::cerr << "encoder Start failed\n";
        if (muxer) {
            muxer->Close();
        }
        encoder->Close();
        decoder->Close();
        return 8;
    }

    if (!decoder->Start()) {
        std::cerr << "decoder Start failed\n";
        encoder->Stop();
        if (muxer) {
            muxer->Close();
        }
        encoder->Close();
        decoder->Close();
        return 9;
    }

    std::atomic<bool> stop_feed{false};
    std::thread feed_thread([&] {
        while (!stop_feed.load(std::memory_order_relaxed)) {
            codec::EncodedPacket packet;
            if (!demuxer->ReadPacket(&packet)) {
                break;
            }

            fed_packets.fetch_add(1, std::memory_order_relaxed);

            if (!decoder->SubmitPacket(std::move(packet))) {
                std::cerr << "SubmitPacket failed after fed_packets="
                          << fed_packets.load(std::memory_order_relaxed) << "\n";
                return;
            }
        }

        if (!stop_feed.load(std::memory_order_relaxed)) {
            feed_reached_eos.store(true, std::memory_order_relaxed);
            if (!decoder->SubmitEndOfStream()) {
                std::cerr << "SubmitEndOfStream failed\n";
            }
        }
    });

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        codec::EncodedPacket latest_packet;
        if (encoder->GetLatestPacket(&latest_packet)) {
            latest_packet_bytes.store(latest_packet.data.size(), std::memory_order_relaxed);
        }

        const auto decoded = decoded_frames.load(std::memory_order_relaxed);
        const auto encoded = encoded_packets.load(std::memory_order_relaxed);
        if (decoded >= expected_decoded_frames && encoded > 0) {
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_feed.store(true, std::memory_order_relaxed);
    std::cerr << "transcode: decoder stop begin\n";
    decoder->Stop();
    std::cerr << "transcode: decoder stop done\n";
    if (feed_thread.joinable()) {
        std::cerr << "transcode: feed join begin\n";
        feed_thread.join();
        std::cerr << "transcode: feed join done\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cerr << "transcode: encoder stop begin\n";
    encoder->Stop();
    std::cerr << "transcode: encoder stop done\n";
    std::cerr << "transcode: encoder close begin\n";
    encoder->Close();
    std::cerr << "transcode: encoder close done\n";
    std::cerr << "transcode: decoder close begin\n";
    decoder->Close();
    std::cerr << "transcode: decoder close done\n";
    if (muxer) {
        std::cerr << "transcode: muxer close begin\n";
        muxer->Close();
        std::cerr << "transcode: muxer close done\n";
    }

    const auto decoded = decoded_frames.load(std::memory_order_relaxed);
    const auto submitted = submitted_frames.load(std::memory_order_relaxed);
    const auto dropped = dropped_frames.load(std::memory_order_relaxed);
    const auto encoded = encoded_packets.load(std::memory_order_relaxed);
    const auto key = key_packets.load(std::memory_order_relaxed);
    const auto latest_bytes = latest_packet_bytes.load(std::memory_order_relaxed);

    std::cout << "decoded_frames=" << decoded << "\n";
    std::cout << "submitted_frames=" << submitted << "\n";
    std::cout << "dropped_frames=" << dropped << "\n";
    std::cout << "encoded_packets=" << encoded << "\n";
    std::cout << "key_packets=" << key << "\n";
    std::cout << "latest_packet_bytes=" << latest_bytes << "\n";
    std::cout << "fed_packets=" << fed_packets.load(std::memory_order_relaxed) << "\n";
    std::cout << "feed_reached_eos=" << (feed_reached_eos.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

    if (decoded <= 0 || submitted <= 0 || encoded <= 0 || latest_bytes == 0) {
        return 10;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_transcode_smoke");
    parser.add<std::string>("input", 'i', "input MP4 path", false, "");
    parser.add<std::string>("output", 'o', "output URI/path", false, "");
    parser.add<std::string>("codec", 'c', "output codec: h264|h265", false, "h264");
    parser.add<int>("device-id", 'd', "AXCL device id/index", false, -1);
    parser.add<int>("expected-frames", 'n', "expected decoded frames", false, 10);
    parser.add<int>("timeout", 't', "timeout seconds", false, 20);

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_path;
    std::string output_uri;
    std::string codec_name{"h264"};
    int device_id = -1;
    int expected_decoded_frames = 10;
    int timeout_seconds = 20;
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_path, std::cerr) ||
        !tooling::GetRequiredArgument(parser, "output", 1, "output", &output_uri, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "codec", 2, std::string("h264"), &codec_name, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "device-id", 3, -1, &device_id, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "expected-frames", 4, 10, &expected_decoded_frames, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "timeout", 5, 20, &timeout_seconds, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }

    return Run(input_path.c_str(),
               output_uri.c_str(),
               ParseCodec(codec_name.c_str()),
               device_id,
               expected_decoded_frames,
               timeout_seconds);
}
