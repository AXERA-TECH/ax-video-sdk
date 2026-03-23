#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "ax_cmdline_utils.h"
#include "ax_mp4_decode_util.h"
#include "codec/ax_video_decoder.h"
#include "ax_image_internal.h"
#include "common/ax_system.h"

namespace {

using namespace axvsdk;

int Run(const char* input_path, int expected_frames, int timeout_seconds, bool dump_meta) {
    common::SystemOptions system_options{};
    system_options.enable_vdec = true;
    system_options.enable_venc = false;
    system_options.enable_ivps = false;
    if (!common::InitializeSystem(system_options)) {
        std::cerr << "InitializeSystem failed\n";
        return 2;
    }

    struct Guard {
        ~Guard() {
            common::ShutdownSystem();
        }
    } guard;

    auto decoder = codec::CreateVideoDecoder();
    if (!decoder) {
        std::cerr << "CreateVideoDecoder failed\n";
        return 3;
    }

    auto demuxer = tooling::OpenDemuxer(input_path, false);
    if (!demuxer) {
        std::cerr << "OpenDemuxer failed\n";
        return 4;
    }

    const auto config = tooling::MakeDecoderConfigFromStreamInfo(demuxer->GetVideoStreamInfo());
    if (!decoder->Open(config)) {
        std::cerr << "decoder Open failed\n";
        return 5;
    }

    std::atomic<int> callback_count{0};
    std::atomic<int> polling_hits{0};
    std::atomic<std::uint32_t> width{0};
    std::atomic<std::uint32_t> height{0};

    decoder->SetFrameCallback([&](common::AxImage::Ptr frame) {
        if (!frame) {
            return;
        }
        if (dump_meta) {
            const auto& fi = common::internal::AxImageAccess::GetAxFrameInfo(*frame);
            const auto index = callback_count.load(std::memory_order_relaxed);
            if (index < 64) {
                std::cerr << "frame[" << index << "]"
                          << " pts=" << static_cast<unsigned long long>(fi.stVFrame.u64PTS)
                          << " time_ref=" << fi.stVFrame.u32TimeRef
                          << " seq=" << static_cast<unsigned long long>(fi.stVFrame.u64SeqNum)
                          << " blk=" << fi.stVFrame.u32BlkId[0] << "\n";
            }
        }
        width.store(frame->width(), std::memory_order_relaxed);
        height.store(frame->height(), std::memory_order_relaxed);
        callback_count.fetch_add(1, std::memory_order_relaxed);
    }, codec::FrameCallbackMode::kQueue);

    if (!decoder->Start()) {
        std::cerr << "decoder Start failed\n";
        decoder->Close();
        return 6;
    }

    std::atomic<bool> stop_feed{false};
    std::thread feed_thread([&] {
        (void)tooling::FeedDecoderFromDemuxer(*demuxer, *decoder, &stop_feed);
    });

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        auto latest = decoder->GetLatestFrame();
        if (latest) {
            polling_hits.fetch_add(1, std::memory_order_relaxed);
            width.store(latest->width(), std::memory_order_relaxed);
            height.store(latest->height(), std::memory_order_relaxed);
        }

        if (callback_count.load(std::memory_order_relaxed) >= expected_frames) {
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_feed.store(true, std::memory_order_relaxed);
    decoder->Stop();
    if (feed_thread.joinable()) {
        feed_thread.join();
    }
    decoder->Close();

    const auto cb = callback_count.load(std::memory_order_relaxed);
    const auto poll = polling_hits.load(std::memory_order_relaxed);
    const auto w = width.load(std::memory_order_relaxed);
    const auto h = height.load(std::memory_order_relaxed);

    std::cout << "callback_frames=" << cb << "\n";
    std::cout << "polling_hits=" << poll << "\n";
    std::cout << "last_width=" << w << "\n";
    std::cout << "last_height=" << h << "\n";

    if (cb <= 0 || poll <= 0 || w == 0 || h == 0) {
        return 7;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_decoder_smoke");
    parser.add<std::string>("input", 'i', "input MP4 path", false, "");
    parser.add<int>("expected-frames", 'n', "expected decoded frames", false, 10);
    parser.add<int>("timeout", 't', "timeout seconds", false, 15);
    parser.add("dump-meta", 0, "dump first 64 decoded frame metadata (PTS/SeqNum/TimeRef) to stderr");

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_path;
    int expected_frames = 10;
    int timeout_seconds = 15;
    bool dump_meta = false;
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_path, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "expected-frames", 1, 10, &expected_frames, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "timeout", 2, 15, &timeout_seconds, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }
    dump_meta = parser.exist("dump-meta");

    return Run(input_path.c_str(), expected_frames, timeout_seconds, dump_meta);
}
