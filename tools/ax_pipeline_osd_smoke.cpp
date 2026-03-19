#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "ax_cmdline_utils.h"
#include "common/ax_system.h"
#include "pipeline/ax_pipeline.h"

namespace {

using namespace axvsdk;

std::uint64_t SumNv12Region(common::AxImage& image,
                            std::uint32_t x,
                            std::uint32_t y,
                            std::uint32_t width,
                            std::uint32_t height) {
    if (image.format() != common::PixelFormat::kNv12 || x >= image.width() || y >= image.height() || width == 0 ||
        height == 0) {
        return 0;
    }

    const auto roi_width = std::min(width, image.width() - x);
    const auto roi_height = std::min(height, image.height() - y);
    if (roi_width == 0 || roi_height == 0) {
        return 0;
    }

    std::uint64_t sum = 0;
    if (!image.InvalidateCache()) {
        return 0;
    }

    const auto* y_plane = image.plane_data(0);
    const auto* uv_plane = image.plane_data(1);
    if (y_plane == nullptr || uv_plane == nullptr) {
        return 0;
    }

    for (std::uint32_t row = 0; row < roi_height; ++row) {
        const auto* y_row = y_plane + static_cast<std::size_t>(y + row) * image.stride(0) + x;
        for (std::uint32_t col = 0; col < roi_width; ++col) {
            sum += y_row[col];
        }
    }

    const auto uv_x = x & ~1U;
    const auto uv_y = y / 2U;
    const auto uv_width = std::min((roi_width + 1U) & ~1U, image.width() - uv_x);
    const auto uv_height = (roi_height + 1U) / 2U;
    for (std::uint32_t row = 0; row < uv_height; ++row) {
        const auto* uv_row = uv_plane + static_cast<std::size_t>(uv_y + row) * image.stride(1) + uv_x;
        for (std::uint32_t col = 0; col < uv_width; ++col) {
            sum += uv_row[col];
        }
    }

    return sum;
}

int Run(const std::string& input_path, const std::string& output_jpeg, int timeout_seconds) {
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

    auto pipeline = pipeline::CreatePipeline();
    if (!pipeline) {
        std::cerr << "CreatePipeline failed\n";
        return 3;
    }

    std::atomic<int> packet_count{0};

    pipeline::PipelineConfig config{};
    config.input.uri = input_path;
    config.input.realtime_playback = false;
    config.input.loop_playback = false;

    pipeline::PipelineOutputConfig output{};
    output.codec = codec::VideoCodecType::kH264;
    output.packet_callback = [&](codec::EncodedPacket) {
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

    auto before_frame = common::AxImage::Ptr{};
    const auto wait_begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - wait_begin < std::chrono::seconds(timeout_seconds)) {
        before_frame = pipeline->GetLatestFrame();
        if (before_frame) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!before_frame) {
        pipeline->Stop();
        pipeline->Close();
        std::cerr << "failed to get initial frame\n";
        return 6;
    }

    const auto before_bitmap_sum = SumNv12Region(*before_frame, 40, 40, 16, 16);
    const auto before_rect_sum = SumNv12Region(*before_frame, 40, 40, 120, 120);
    if (before_bitmap_sum == 0 || before_rect_sum == 0) {
        pipeline->Stop();
        pipeline->Close();
        std::cerr << "failed to sample before frame\n";
        return 7;
    }

    common::DrawFrame osd{};
    osd.hold_frames = 0;
    common::DrawBitmap bitmap{};
    bitmap.format = common::DrawBitmapFormat::kRgb888;
    bitmap.alpha = 255;
    bitmap.width = 16;
    bitmap.height = 16;
    bitmap.dst_x = 40;
    bitmap.dst_y = 40;
    bitmap.data.resize(static_cast<std::size_t>(bitmap.width) * bitmap.height * 3U, 0);
    for (std::size_t index = 0; index < bitmap.data.size(); index += 3U) {
        bitmap.data[index] = 0;
        bitmap.data[index + 1U] = 255;
        bitmap.data[index + 2U] = 0;
    }
    osd.bitmaps.push_back(std::move(bitmap));
    osd.rects.push_back(common::DrawRect{
        40,
        40,
        120,
        120,
        4,
        255,
        0x00FF00,
        true,
        false,
        0,
        0,
    });
    osd.lines.push_back(common::DrawLine{
        {
            {220, 80},
            {340, 160},
            {420, 120},
        },
        8,
        255,
        0xFF0000,
    });
    osd.mosaics.push_back(common::DrawMosaic{
        520,
        120,
        160,
        120,
        common::DrawMosaicBlockSize::k16,
    });

    if (!pipeline->SetOsd(osd)) {
        pipeline->Stop();
        pipeline->Close();
        std::cerr << "SetOsd failed\n";
        return 8;
    }

    common::AxImage::Ptr after_frame;
    std::uint64_t after_bitmap_sum = 0;
    std::uint64_t after_rect_sum = 0;
    const auto osd_wait_begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - osd_wait_begin < std::chrono::seconds(timeout_seconds)) {
        after_frame = pipeline->GetLatestFrame();
        if (after_frame) {
            after_bitmap_sum = SumNv12Region(*after_frame, 40, 40, 16, 16);
            after_rect_sum = SumNv12Region(*after_frame, 40, 40, 120, 120);
            if (after_bitmap_sum != before_bitmap_sum || after_rect_sum != before_rect_sum) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    pipeline->Stop();
    pipeline->Close();

    if (!after_frame) {
        std::cerr << "failed to get OSD frame\n";
        return 9;
    }

    if (after_bitmap_sum == before_bitmap_sum && after_rect_sum == before_rect_sum) {
        std::cerr << "OSD verification failed"
                  << " before_bitmap_sum=" << before_bitmap_sum
                  << " after_bitmap_sum=" << after_bitmap_sum
                  << " before_rect_sum=" << before_rect_sum
                  << " after_rect_sum=" << after_rect_sum << "\n";
        return 10;
    }

    (void)output_jpeg;
    std::cout << "before_bitmap_sum=" << before_bitmap_sum << "\n";
    std::cout << "after_bitmap_sum=" << after_bitmap_sum << "\n";
    std::cout << "before_rect_sum=" << before_rect_sum << "\n";
    std::cout << "after_rect_sum=" << after_rect_sum << "\n";
    std::cout << "encoded_packets=" << packet_count.load(std::memory_order_relaxed) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_pipeline_osd_smoke");
    parser.add<std::string>("input", 'i', "input URI/path", false, "");
    parser.add<std::string>("output", 'o', "output JPEG path", false, "");
    parser.add<int>("timeout", 't', "timeout seconds", false, 20);

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_path;
    std::string output_jpeg;
    int timeout_seconds = parser.get<int>("timeout");
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_path, std::cerr) ||
        !tooling::GetOptionalArgument(parser, "output", 1, std::string(""), &output_jpeg, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }

    return Run(input_path, output_jpeg, timeout_seconds);
}
