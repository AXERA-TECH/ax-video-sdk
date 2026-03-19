#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ax_cmdline_utils.h"
#include "common/ax_image.h"
#include "common/ax_system.h"
#include "pipeline/ax_pipeline_manager.h"

namespace {

using namespace axvsdk;

struct ChannelCounters {
    std::atomic<int> packets{0};
    std::atomic<int> key_packets{0};
    std::atomic<int> callback_frames{0};
    std::atomic<int> poll_frames{0};
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

enum class FrameCopyMode {
    kNone = 0,
    kCallback,
    kPollReturn,
    kPollPrealloc,
};

FrameCopyMode ParseFrameCopyMode(const char* value) {
    if (value == nullptr) {
        return FrameCopyMode::kNone;
    }

    const std::string text(value);
    if (text == "callback") {
        return FrameCopyMode::kCallback;
    }
    if (text == "poll_return" || text == "poll-return") {
        return FrameCopyMode::kPollReturn;
    }
    if (text == "poll_prealloc" || text == "poll-prealloc") {
        return FrameCopyMode::kPollPrealloc;
    }
    return FrameCopyMode::kNone;
}

std::string MakeName(int index) {
    std::ostringstream output;
    output << "ch" << std::setw(2) << std::setfill('0') << index;
    return output.str();
}

pipeline::PipelineConfig MakeConfig(const std::string& input_path,
                                    int device_id,
                                    codec::VideoCodecType codec,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    bool loop_playback,
                                    FrameCopyMode frame_copy_mode,
                                    ChannelCounters* counters) {
    pipeline::PipelineConfig config{};
    config.device_id = device_id;
    config.input.uri = input_path;
    config.input.realtime_playback = false;
    config.input.loop_playback = loop_playback;
    if (frame_copy_mode != FrameCopyMode::kNone) {
        config.frame_output.output_image.format = common::PixelFormat::kNv12;
    }

    pipeline::PipelineOutputConfig output{};
    output.codec = codec;
    output.width = width;
    output.height = height;
    output.packet_callback = [counters](codec::EncodedPacket packet) {
        if (counters != nullptr) {
            counters->packets.fetch_add(1, std::memory_order_relaxed);
            if (packet.key_frame) {
                counters->key_packets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    config.outputs.push_back(std::move(output));
    return config;
}

int Run(const char* input_path,
        int channel_count,
        int start_device_id,
        int device_count,
        codec::VideoCodecType output_codec,
        std::uint32_t output_width,
        std::uint32_t output_height,
        bool loop_playback,
        FrameCopyMode frame_copy_mode,
        int frame_copy_interval_ms,
        int expected_packets_per_channel,
        int timeout_seconds) {
    if (channel_count <= 0) {
        std::cerr << "invalid channel_count\n";
        return 2;
    }
    if (device_count <= 0) {
        std::cerr << "invalid device_count\n";
        return 2;
    }

    int initialized_device_count = 0;
    for (int device_offset = 0; device_offset < device_count; ++device_offset) {
        common::SystemOptions system_options{};
        system_options.device_id = start_device_id + device_offset;
        system_options.enable_vdec = true;
        system_options.enable_venc = true;
        system_options.enable_ivps = (output_width != 0U && output_height != 0U);
        if (!common::InitializeSystem(system_options)) {
            std::cerr << "InitializeSystem failed at device_index=" << (start_device_id + device_offset) << "\n";
            while (initialized_device_count-- > 0) {
                common::ShutdownSystem();
            }
            return 3;
        }
        ++initialized_device_count;
    }

    struct Guard {
        int count{0};
        ~Guard() {
            for (int index = 0; index < count; ++index) {
                common::ShutdownSystem();
            }
        }
    } guard{initialized_device_count};

    auto manager = pipeline::CreatePipelineManager();
    if (!manager) {
        std::cerr << "CreatePipelineManager failed\n";
        return 4;
    }

    std::vector<std::unique_ptr<ChannelCounters>> counters;
    counters.reserve(static_cast<std::size_t>(channel_count));
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(channel_count));
    std::vector<std::thread> copy_threads;
    std::atomic<bool> stop_copy_threads{false};

    for (int index = 0; index < channel_count; ++index) {
        auto channel_counters = std::make_unique<ChannelCounters>();
        const auto name = MakeName(index);
        const int device_id = start_device_id + (index % device_count);
        const auto config = MakeConfig(
            input_path,
            device_id,
            output_codec,
            output_width,
            output_height,
            loop_playback,
            frame_copy_mode,
            channel_counters.get());
        if (!manager->AddPipeline(name, config)) {
            manager->Clear();
            std::cerr << "AddPipeline failed at " << name << "\n";
            return 5;
        }
        counters.push_back(std::move(channel_counters));
        names.push_back(name);
    }

    if (!manager->StartAll()) {
        manager->Clear();
        std::cerr << "StartAll failed\n";
        return 6;
    }

    if (frame_copy_mode == FrameCopyMode::kCallback) {
        for (int index = 0; index < channel_count; ++index) {
            auto* pipeline = manager->GetPipeline(names[static_cast<std::size_t>(index)]);
            if (pipeline == nullptr) {
                continue;
            }
            auto* channel_counters = counters[static_cast<std::size_t>(index)].get();
            pipeline->SetFrameCallback([channel_counters](common::AxImage::Ptr frame) {
                if (channel_counters != nullptr && frame) {
                    channel_counters->callback_frames.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    } else if (frame_copy_mode == FrameCopyMode::kPollReturn || frame_copy_mode == FrameCopyMode::kPollPrealloc) {
        copy_threads.reserve(static_cast<std::size_t>(channel_count));
        for (int index = 0; index < channel_count; ++index) {
            auto* pipeline = manager->GetPipeline(names[static_cast<std::size_t>(index)]);
            auto* channel_counters = counters[static_cast<std::size_t>(index)].get();
            if (pipeline == nullptr || channel_counters == nullptr) {
                continue;
            }

            copy_threads.emplace_back([pipeline, channel_counters, frame_copy_mode, frame_copy_interval_ms, &stop_copy_threads] {
                common::AxImage::Ptr latest;
                common::AxImage::Ptr prealloc;
                while (!stop_copy_threads.load(std::memory_order_relaxed)) {
                    bool ok = false;
                    if (frame_copy_mode == FrameCopyMode::kPollReturn) {
                        latest = pipeline->GetLatestFrame();
                        ok = static_cast<bool>(latest);
                    } else {
                        if (!prealloc) {
                            latest = pipeline->GetLatestFrame();
                            if (latest) {
                                prealloc = common::AxImage::Create(latest->descriptor());
                                ok = prealloc && pipeline->GetLatestFrame(*prealloc);
                            }
                        } else {
                            ok = pipeline->GetLatestFrame(*prealloc);
                        }
                    }

                    if (ok) {
                        channel_counters->poll_frames.fetch_add(1, std::memory_order_relaxed);
                    }

                    if (frame_copy_interval_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(frame_copy_interval_ms));
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
    }

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        int ready_channels = 0;
        for (const auto& name : names) {
            pipeline::ManagedPipelineInfo info{};
            if (!manager->GetPipelineInfo(name, &info)) {
                continue;
            }

            const auto encoded = info.stats.output_stats.empty() ? 0ULL : info.stats.output_stats.front().encoded_packets;
            if (static_cast<int>(encoded) >= expected_packets_per_channel) {
                ++ready_channels;
            }
        }

        if (ready_channels == channel_count) {
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    std::uint64_t total_decoded_frames = 0;
    std::uint64_t total_encoded_packets = 0;
    std::uint64_t total_dropped_frames = 0;
    std::uint64_t total_submit_failures = 0;
    int successful_channels = 0;

    for (int index = 0; index < channel_count; ++index) {
        pipeline::ManagedPipelineInfo info{};
        const bool ok = manager->GetPipelineInfo(names[static_cast<std::size_t>(index)], &info);
        if (!ok) {
            continue;
        }

        const auto encoded = info.stats.output_stats.empty() ? 0ULL : info.stats.output_stats.front().encoded_packets;
        const auto dropped = info.stats.output_stats.empty() ? 0ULL : info.stats.output_stats.front().dropped_frames;

        total_decoded_frames += info.stats.decoded_frames;
        total_encoded_packets += encoded;
        total_dropped_frames += dropped;
        total_submit_failures += info.stats.branch_submit_failures;
        if (info.stats.decoded_frames > 0 && encoded > 0) {
            ++successful_channels;
        }

        std::cout << names[static_cast<std::size_t>(index)]
                  << "_decoded_frames=" << info.stats.decoded_frames << "\n";
        std::cout << names[static_cast<std::size_t>(index)]
                  << "_encoded_packets=" << encoded << "\n";
        std::cout << names[static_cast<std::size_t>(index)]
                  << "_dropped_frames=" << dropped << "\n";
        std::cout << names[static_cast<std::size_t>(index)]
                  << "_callback_packets=" << counters[static_cast<std::size_t>(index)]->packets.load(std::memory_order_relaxed)
                  << "\n";
        std::cout << names[static_cast<std::size_t>(index)]
                  << "_key_packets=" << counters[static_cast<std::size_t>(index)]->key_packets.load(std::memory_order_relaxed)
                  << "\n";
    }

    stop_copy_threads.store(true, std::memory_order_relaxed);
    for (auto& thread : copy_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    manager->StopAll();
    manager->Clear();

    const double elapsed_seconds = elapsed_ms > 0 ? static_cast<double>(elapsed_ms) / 1000.0 : 0.0;
    const double aggregate_fps =
        elapsed_seconds > 0.0 ? static_cast<double>(total_encoded_packets) / elapsed_seconds : 0.0;
    std::uint64_t total_callback_frames = 0;
    std::uint64_t total_poll_frames = 0;

    std::cout << "channel_count=" << channel_count << "\n";
    std::cout << "start_device_id=" << start_device_id << "\n";
    std::cout << "device_count=" << device_count << "\n";
    std::cout << "successful_channels=" << successful_channels << "\n";
    std::cout << "elapsed_ms=" << elapsed_ms << "\n";
    std::cout << "total_decoded_frames=" << total_decoded_frames << "\n";
    std::cout << "total_encoded_packets=" << total_encoded_packets << "\n";
    std::cout << "total_dropped_frames=" << total_dropped_frames << "\n";
    std::cout << "total_submit_failures=" << total_submit_failures << "\n";
    std::cout << "aggregate_encoded_fps=" << aggregate_fps << "\n";

    for (int index = 0; index < channel_count; ++index) {
        total_callback_frames += counters[static_cast<std::size_t>(index)]->callback_frames.load(std::memory_order_relaxed);
        total_poll_frames += counters[static_cast<std::size_t>(index)]->poll_frames.load(std::memory_order_relaxed);
    }
    std::cout << "frame_copy_mode=" << static_cast<int>(frame_copy_mode) << "\n";
    std::cout << "total_callback_frames=" << total_callback_frames << "\n";
    std::cout << "total_poll_frames=" << total_poll_frames << "\n";

    if (successful_channels != channel_count) {
        return 7;
    }
    if (expected_packets_per_channel > 0 &&
        total_encoded_packets < static_cast<std::uint64_t>(expected_packets_per_channel) *
                                    static_cast<std::uint64_t>(channel_count)) {
        return 8;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_pipeline_stress_smoke");
    parser.add<std::string>("input", 'i', "input MP4 path", false, "");
    parser.add<int>("channels", 'c', "channel count", false, 16);
    parser.add<int>("device-id", 'd', "start device index", false, 0);
    parser.add<int>("device-count", 0, "number of devices used for round-robin assignment", false, 1);
    parser.add<std::string>("codec", 'x', "output codec: h264|h265", false, "h264");
    parser.add<int>("width", 'w', "output width", false, 0);
    parser.add<int>("height", 0, "output height", false, 0);
    parser.add("loop", 0, "loop MP4/file input");
    parser.add<std::string>("frame-copy-mode", 'm', "none|callback|poll_return|poll_prealloc", false, "none");
    parser.add<int>("frame-copy-interval", 0, "frame copy interval in ms; 0 means busy loop", false, 33);
    parser.add<int>("expected-packets", 'n', "expected packets per channel", false, 10);
    parser.add<int>("timeout", 't', "timeout seconds", false, 30);

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_path;
    int channel_count = 16;
    int start_device_id = 0;
    int device_count = 1;
    std::string codec_name{"h264"};
    int output_width = 0;
    int output_height = 0;
    const bool loop_playback = parser.exist("loop");
    std::string frame_copy_mode_name{"none"};
    int frame_copy_interval_ms = 33;
    int expected_packets_per_channel = 10;
    int timeout_seconds = 30;
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_path, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "channels", 1, 16, &channel_count, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "device-id", 2, 0, &start_device_id, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "device-count", 3, 1, &device_count, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "codec", 4, std::string("h264"), &codec_name, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "width", 5, 0, &output_width, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "height", 6, 0, &output_height, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "frame-copy-mode", 7, std::string("none"), &frame_copy_mode_name, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "frame-copy-interval", 8, 33, &frame_copy_interval_ms, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "expected-packets", 9, 10, &expected_packets_per_channel, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "timeout", 10, 30, &timeout_seconds, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }

    return Run(input_path.c_str(),
               channel_count,
               start_device_id,
               device_count,
               ParseCodec(codec_name.c_str()),
               static_cast<std::uint32_t>(output_width),
               static_cast<std::uint32_t>(output_height),
               loop_playback,
               ParseFrameCopyMode(frame_copy_mode_name.c_str()),
               frame_copy_interval_ms,
               expected_packets_per_channel,
               timeout_seconds);
}
