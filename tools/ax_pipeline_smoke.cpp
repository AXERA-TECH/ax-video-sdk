#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ax_cmdline_utils.h"
#include "common/ax_system.h"
#include "pipeline/ax_pipeline.h"

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

common::ResizeMode ParseResizeMode(const char* value) {
    if (value == nullptr) {
        return common::ResizeMode::kStretch;
    }

    const std::string text(value);
    if (text == "keep" || text == "keep_aspect" || text == "letterbox") {
        return common::ResizeMode::kKeepAspectRatio;
    }
    return common::ResizeMode::kStretch;
}

std::vector<std::string> SplitOutputUris(const std::string& value) {
    std::vector<std::string> uris;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            uris.push_back(item);
        }
    }
    return uris;
}

int Run(const char* input_path,
        const std::vector<std::string>& output_uris,
        codec::VideoCodecType output_codec,
        int device_id,
        std::uint32_t output_width,
        std::uint32_t output_height,
        common::ResizeMode resize_mode,
        bool loop_playback,
        int expected_packets,
        int timeout_seconds) {
    common::SystemOptions system_options{};
    system_options.device_id = device_id;
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

    auto pipeline = pipeline::CreatePipeline();
    if (!pipeline) {
        std::cerr << "CreatePipeline failed\n";
        return 3;
    }

    std::atomic<int> packet_count{0};
    std::atomic<int> key_count{0};

    pipeline::PipelineConfig config{};
    config.device_id = device_id;
    config.input.uri = input_path;
    config.input.realtime_playback = false;
    config.input.loop_playback = loop_playback;

    pipeline::PipelineOutputConfig output{};
    output.codec = output_codec;
    output.width = output_width;
    output.height = output_height;
    output.resize.mode = resize_mode;
    output.uris = output_uris;
    output.packet_callback = [&](codec::EncodedPacket packet) {
        if (packet.key_frame) {
            key_count.fetch_add(1, std::memory_order_relaxed);
        }
        packet_count.fetch_add(1, std::memory_order_relaxed);
    };
    config.outputs.push_back(output);

    if (!pipeline->Open(config)) {
        std::cerr << "pipeline Open failed\n";
        return 4;
    }

    if (!pipeline->Start()) {
        std::cerr << "pipeline Start failed\n";
        pipeline->Close();
        return 5;
    }

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const auto stats = pipeline->GetStats();
        if (!stats.output_stats.empty() &&
            static_cast<int>(stats.output_stats.front().encoded_packets) >= expected_packets) {
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto stats = pipeline->GetStats();
    std::cerr << "pipeline: stop begin\n";
    pipeline->Stop();
    std::cerr << "pipeline: stop done\n";
    std::cerr << "pipeline: close begin\n";
    pipeline->Close();
    std::cerr << "pipeline: close done\n";

    const auto encoded_packets = stats.output_stats.empty() ? 0ULL : stats.output_stats.front().encoded_packets;
    const auto dropped_frames = stats.output_stats.empty() ? 0ULL : stats.output_stats.front().dropped_frames;

    std::cout << "decoded_frames=" << stats.decoded_frames << "\n";
    std::cout << "branch_submit_failures=" << stats.branch_submit_failures << "\n";
    std::cout << "output_width=" << output_width << "\n";
    std::cout << "output_height=" << output_height << "\n";
    std::cout << "resize_mode=" << static_cast<int>(resize_mode) << "\n";
    std::cout << "encoded_packets=" << encoded_packets << "\n";
    std::cout << "dropped_frames=" << dropped_frames << "\n";
    std::cout << "callback_packets=" << packet_count.load(std::memory_order_relaxed) << "\n";
    std::cout << "key_packets=" << key_count.load(std::memory_order_relaxed) << "\n";

    if (stats.decoded_frames == 0 || encoded_packets == 0 || packet_count.load(std::memory_order_relaxed) == 0) {
        return 6;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_pipeline_smoke");
    parser.add<std::string>("input", 'i', "input URI/path", false, "");
    parser.add<std::string>("output", 'o', "output URI/path list, separated by commas", false, "");
    parser.add<std::string>("codec", 'c', "output codec: h264|h265", false, "h264");
    parser.add<int>("device-id", 'd', "AXCL device id/index", false, -1);
    parser.add<int>("width", 'w', "output width", false, 0);
    parser.add<int>("height", 0, "output height", false, 0);
    parser.add("loop", 0, "loop MP4/file input");
    parser.add<int>("expected-packets", 'n', "expected encoded packets", false, 10);
    parser.add<int>("timeout", 't', "timeout seconds", false, 20);
    parser.add<std::string>("resize", 'r', "resize mode: stretch|keep_aspect", false, "stretch");

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_uri;
    std::string output_uri_csv;
    std::string codec_name{"h264"};
    int device_id = -1;
    int width = 0;
    int height = 0;
    const bool loop_playback = parser.exist("loop");
    int expected_packets = 10;
    int timeout_seconds = 20;
    std::string resize_mode_name{"stretch"};
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_uri, std::cerr) ||
        !tooling::GetRequiredArgument(parser, "output", 1, "output", &output_uri_csv, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "codec", 2, std::string("h264"), &codec_name, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "device-id", 3, -1, &device_id, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "width", 4, 0, &width, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "height", 5, 0, &height, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "expected-packets", 6, 10, &expected_packets, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "timeout", 7, 20, &timeout_seconds, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "resize", 8, std::string("stretch"), &resize_mode_name, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }

    const auto output_uris = SplitOutputUris(output_uri_csv);
    if (output_uris.empty()) {
        std::cerr << "output URI/path is empty\n";
        return 1;
    }

    return Run(input_uri.c_str(),
               output_uris,
               ParseCodec(codec_name.c_str()),
               device_id,
               static_cast<std::uint32_t>(width),
               static_cast<std::uint32_t>(height),
               ParseResizeMode(resize_mode_name.c_str()),
               loop_playback,
               expected_packets,
               timeout_seconds);
}
