#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "ax_cmdline_utils.h"
#include "codec/ax_jpeg_codec.h"
#include "common/ax_system.h"

namespace {

using namespace axvsdk;

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* bytes) {
    if (bytes == nullptr) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    bytes->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !bytes->empty();
}

std::uint64_t ImageChecksum(common::AxImage& image) {
    // 必须经 CopyToHost:AXCL 下解码出的帧在设备侧,plane_data() 的地址 CPU 解引用会段错误。
    std::vector<std::uint8_t> host(image.byte_size());
    if (host.empty() || !image.CopyToHost(host.data(), host.size())) {
        std::cerr << "ImageChecksum: CopyToHost failed (bytes=" << image.byte_size()
                  << " planes=" << image.plane_count() << " plane0=" << image.plane_size(0)
                  << " phy0=0x" << std::hex << image.physical_address(0) << std::dec
                  << " mem=" << static_cast<int>(image.memory_type()) << ")\n";
        return 0;
    }

    std::uint64_t sum = 0;
    const std::uint8_t* p = host.data();
    for (std::size_t plane = 0; plane < image.plane_count(); ++plane) {
        const auto size = image.plane_size(plane);
        const auto bytes = std::min<std::size_t>(size, 4096U);
        for (std::size_t i = 0; i < bytes; ++i) {
            sum += p[i];
        }
        p += size;
    }

    return sum;
}

int Run(const std::string& input_path, const std::string& output_dir) {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        std::cerr << "create_directories failed: " << ec.message() << "\n";
        return 2;
    }

    common::SystemOptions system_options{};
    system_options.enable_vdec = true;
    system_options.enable_venc = true;
    system_options.enable_ivps = true;
    if (!common::InitializeSystem(system_options)) {
        std::cerr << "InitializeSystem failed\n";
        return 3;
    }

    struct Guard {
        ~Guard() {
            common::ShutdownSystem();
        }
    } guard;

    std::vector<std::uint8_t> file_bytes;
    if (!ReadFile(input_path, &file_bytes) || file_bytes.empty()) {
        std::cerr << "ReadFile failed\n";
        return 4;
    }

    auto decoded_from_file = codec::DecodeJpegFile(input_path);
    auto decoded_from_memory = codec::DecodeJpegMemory(file_bytes.data(), file_bytes.size());
    if (!decoded_from_file || !decoded_from_memory) {
        std::cerr << "Decode from file or memory failed\n";
        return 5;
    }

    const auto file_checksum = ImageChecksum(*decoded_from_file);
    const auto memory_checksum = ImageChecksum(*decoded_from_memory);
    if (file_checksum == 0 || memory_checksum == 0 || file_checksum != memory_checksum) {
        std::cerr << "decoded checksum mismatch\n";
        return 6;
    }

    codec::JpegEncodeOptions encode_options{};
    encode_options.quality = 85;
    const auto encoded_memory = codec::EncodeJpegToMemory(*decoded_from_file, encode_options);
    if (encoded_memory.empty()) {
        std::cerr << "EncodeJpegToMemory failed\n";
        return 7;
    }

    const auto encoded_path = (std::filesystem::path(output_dir) / "encoded_from_nv12.jpg").string();
    if (!codec::EncodeJpegToFile(*decoded_from_file, encoded_path, encode_options)) {
        std::cerr << "EncodeJpegToFile failed\n";
        return 8;
    }

    const auto encoded_base64 = codec::EncodeJpegToBase64(*decoded_from_file, encode_options);
    if (encoded_base64.empty()) {
        std::cerr << "EncodeJpegToBase64 failed\n";
        return 9;
    }

    codec::JpegDecodeOptions bgr_decode_options{};
    bgr_decode_options.output_image.format = common::PixelFormat::kBgr24;
    auto decoded_from_base64_bgr = codec::DecodeJpegBase64(encoded_base64, bgr_decode_options);
    if (!decoded_from_base64_bgr) {
        std::cerr << "DecodeJpegBase64 failed\n";
        return 10;
    }

    const auto encoded_from_bgr = codec::EncodeJpegToMemory(*decoded_from_base64_bgr, encode_options);
    if (encoded_from_bgr.empty()) {
        std::cerr << "EncodeJpegToMemory from BGR failed\n";
        return 11;
    }

    auto roundtrip_decoded = codec::DecodeJpegMemory(encoded_from_bgr.data(), encoded_from_bgr.size());
    if (!roundtrip_decoded) {
        std::cerr << "roundtrip DecodeJpegMemory failed\n";
        return 12;
    }

    if (roundtrip_decoded->width() != decoded_from_file->width() ||
        roundtrip_decoded->height() != decoded_from_file->height()) {
        std::cerr << "roundtrip dimensions mismatch\n";
        return 13;
    }

    std::cout << "input_size=" << file_bytes.size() << "\n";
    std::cout << "decoded_width=" << decoded_from_file->width() << "\n";
    std::cout << "decoded_height=" << decoded_from_file->height() << "\n";
    std::cout << "decoded_file_checksum=" << file_checksum << "\n";
    std::cout << "decoded_memory_checksum=" << memory_checksum << "\n";
    std::cout << "encoded_memory_size=" << encoded_memory.size() << "\n";
    std::cout << "encoded_file_path=" << encoded_path << "\n";
    std::cout << "encoded_base64_size=" << encoded_base64.size() << "\n";
    std::cout << "base64_bgr_width=" << decoded_from_base64_bgr->width() << "\n";
    std::cout << "base64_bgr_height=" << decoded_from_base64_bgr->height() << "\n";
    std::cout << "base64_bgr_checksum=" << ImageChecksum(*decoded_from_base64_bgr) << "\n";
    std::cout << "encoded_from_bgr_size=" << encoded_from_bgr.size() << "\n";
    std::cout << "roundtrip_width=" << roundtrip_decoded->width() << "\n";
    std::cout << "roundtrip_height=" << roundtrip_decoded->height() << "\n";
    std::cout << "roundtrip_checksum=" << ImageChecksum(*roundtrip_decoded) << "\n";

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_jpeg_smoke");
    parser.add<std::string>("input", 'i', "input JPEG path", false, "");
    parser.add<std::string>("output", 'o', "output directory", false, "");

    const auto cli_result = tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != tooling::CliParseResult::kOk) {
        return tooling::CliParseExitCode(cli_result);
    }

    std::string input_path;
    std::string output_dir;
    if (!tooling::GetRequiredArgument(parser, "input", 0, "input", &input_path, std::cerr) ||
        !tooling::GetRequiredArgument(parser, "output", 1, "output", &output_dir, std::cerr)) {
        std::cerr << parser.usage();
        return 1;
    }

    return Run(input_path, output_dir);
}
