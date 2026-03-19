#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "ax_cmdline_utils.h"
#include "codec/ax_jpeg_codec.h"
#include "common/ax_drawer.h"
#include "common/ax_image.h"
#include "common/ax_system.h"

namespace {

using namespace axvsdk;

bool FillNv12(common::AxImage* image, std::uint8_t y, std::uint8_t uv) {
    if (image == nullptr || image->format() != common::PixelFormat::kNv12) {
        return false;
    }

    auto* y_plane = image->mutable_plane_data(0);
    auto* uv_plane = image->mutable_plane_data(1);
    if (y_plane == nullptr || uv_plane == nullptr) {
        return false;
    }

    for (std::uint32_t row = 0; row < image->height(); ++row) {
        std::memset(y_plane + static_cast<std::size_t>(row) * image->stride(0), y, image->width());
    }
    for (std::uint32_t row = 0; row < image->height() / 2U; ++row) {
        std::memset(uv_plane + static_cast<std::size_t>(row) * image->stride(1), uv, image->width());
    }

    return image->FlushCache();
}

std::uint64_t SumNv12Region(const common::AxImage& image,
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

    std::uint64_t sum = 0;
    for (std::uint32_t row = 0; row < roi_height; ++row) {
        const auto* y_row = image.plane_data(0) + static_cast<std::size_t>(y + row) * image.stride(0) + x;
        for (std::uint32_t col = 0; col < roi_width; ++col) {
            sum += y_row[col];
        }
    }

    const auto uv_x = x & ~1U;
    const auto uv_y = (y / 2U);
    const auto uv_width = std::min((roi_width + 1U) & ~1U, image.width() - uv_x);
    const auto uv_height = (roi_height + 1U) / 2U;
    for (std::uint32_t row = 0; row < uv_height; ++row) {
        const auto* uv_row =
            image.plane_data(1) + static_cast<std::size_t>(uv_y + row) * image.stride(1) + uv_x;
        for (std::uint32_t col = 0; col < uv_width; ++col) {
            sum += uv_row[col];
        }
    }

    return sum;
}

int Run(const std::string& output_path) {
    common::SystemOptions system_options{};
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

    auto image = common::AxImage::Create(common::PixelFormat::kNv12, 640, 360);
    if (!image || !FillNv12(image.get(), 16, 128)) {
        std::cerr << "image init failed\n";
        return 3;
    }
    if (!image->InvalidateCache()) {
        std::cerr << "InvalidateCache failed before draw\n";
        return 3;
    }

    auto drawer = common::CreateDrawer();
    if (!drawer) {
        std::cerr << "CreateDrawer failed\n";
        return 4;
    }

    common::DrawFrame osd{};
    osd.hold_frames = 1;

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
        80,
        80,
        120,
        80,
        6,
        255,
        0xFF0000,
        false,
        false,
        0,
        0,
    });

    auto prepared = drawer->Prepare(osd);
    if (!prepared) {
        std::cerr << "drawer Prepare failed\n";
        return 5;
    }

    const auto bitmap_sum_before = SumNv12Region(*image, 40, 40, 16, 16);
    const auto rect_sum_before = SumNv12Region(*image, 80, 80, 120, 80);

    if (!drawer->Draw(*prepared, *image)) {
        std::cerr << "drawer Draw failed\n";
        return 6;
    }

    if (!image->InvalidateCache()) {
        std::cerr << "InvalidateCache failed after draw\n";
        return 7;
    }

    const auto bitmap_sum_after = SumNv12Region(*image, 40, 40, 16, 16);
    const auto rect_sum_after = SumNv12Region(*image, 80, 80, 120, 80);
    if (bitmap_sum_before == bitmap_sum_after && rect_sum_before == rect_sum_after) {
        std::cerr << "OSD verification failed"
                  << " bitmap_before=" << bitmap_sum_before
                  << " bitmap_after=" << bitmap_sum_after
                  << " rect_before=" << rect_sum_before
                  << " rect_after=" << rect_sum_after << "\n";
        return 7;
    }

    if (!output_path.empty() && !codec::EncodeJpegToFile(*image, output_path)) {
        std::cerr << "EncodeJpegToFile failed\n";
        return 8;
    }

    if (!output_path.empty()) {
        std::cout << "output_jpeg=" << output_path << "\n";
    }
    std::cout << "bitmap_sum_before=" << bitmap_sum_before << "\n";
    std::cout << "bitmap_sum_after=" << bitmap_sum_after << "\n";
    std::cout << "rect_sum_before=" << rect_sum_before << "\n";
    std::cout << "rect_sum_after=" << rect_sum_after << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_osd_draw_smoke");
    parser.add<std::string>("output", 'o', "output JPEG path", false, "");

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    return Run(parser.get<std::string>("output"));
}
