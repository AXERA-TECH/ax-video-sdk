#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ax_cmdline_utils.h"
#include "ax_mp4_demuxer.h"
#include "minimp4/minimp4.h"

namespace {

std::vector<std::uint8_t> ReadFileBytes(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(input.tellg());
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(size);
    if (size != 0) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!input) {
            return {};
        }
    }
    return bytes;
}

int ReadFromMemory(int64_t offset, void* buffer, size_t size, void* token) {
    if (offset < 0 || buffer == nullptr || token == nullptr) {
        return -1;
    }

    const auto* bytes = static_cast<const std::vector<std::uint8_t>*>(token);
    const auto begin = static_cast<std::size_t>(offset);
    if (begin > bytes->size() || size > (bytes->size() - begin)) {
        return -1;
    }

    std::memcpy(buffer, bytes->data() + begin, size);
    return 0;
}

const char* CodecName(axvsdk::codec::VideoCodecType codec) noexcept {
    using axvsdk::codec::VideoCodecType;
    switch (codec) {
    case VideoCodecType::kH264:
        return "h264";
    case VideoCodecType::kH265:
        return "h265";
    case VideoCodecType::kJpeg:
        return "jpeg";
    case VideoCodecType::kUnknown:
    default:
        return "unknown";
    }
}

void DumpMinimp4Tracks(const char* path) {
    auto file_bytes = ReadFileBytes(path);
    if (file_bytes.empty()) {
        std::cerr << "diagnostic: unable to read input bytes\n";
        return;
    }

    MP4D_demux_t demux{};
    if (MP4D_open(&demux, ReadFromMemory, &file_bytes, static_cast<int64_t>(file_bytes.size())) != 1) {
        std::cerr << "diagnostic: minimp4 open failed\n";
        return;
    }

    std::cerr << "diagnostic: track_count=" << demux.track_count << "\n";
    for (unsigned index = 0; index < demux.track_count; ++index) {
        const auto& track = demux.track[index];
        std::cerr << "diagnostic: track[" << index << "]"
                  << " object_type=0x" << std::hex << track.object_type_indication << std::dec
                  << " handler=0x" << std::hex << track.handler_type << std::dec
                  << " size=" << track.SampleDescription.video.width << "x"
                  << track.SampleDescription.video.height
                  << " dsi_bytes=" << track.dsi_bytes
                  << " timescale=" << track.timescale
                  << " samples=" << track.sample_count << "\n";
    }

    MP4D_close(&demux);
}

}  // namespace

int main(int argc, char* argv[]) {
    cmdline::parser parser;
    parser.set_program_name("ax_mp4_demux_probe");
    parser.add<std::string>("input", 'i', "input MP4 path", false, "");
    parser.add<int>("count", 'n', "number of packets to probe", false, 3);
    parser.add("loop", 0, "reset to the beginning when EOF is reached");

    const auto cli_result = axvsdk::tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != axvsdk::tooling::CliParseResult::kOk) {
        return axvsdk::tooling::CliParseExitCode(cli_result);
    }

    std::string input_path;
    int packet_count = 3;
    const bool loop_playback = parser.exist("loop");
    if (!axvsdk::tooling::GetRequiredArgument(parser, "input", 0, "input", &input_path, std::cerr)) {
        std::cerr << parser.usage();
        return EXIT_FAILURE;
    }
    if (!axvsdk::tooling::GetOptionalArgument(parser, "count", 1, 3, &packet_count, std::cerr)) {
        std::cerr << parser.usage();
        return EXIT_FAILURE;
    }
    if (packet_count <= 0) {
        std::cerr << "count must be > 0\n";
        return EXIT_FAILURE;
    }

    auto demuxer = axvsdk::codec::AxMp4Demuxer::Open(input_path);
    if (!demuxer) {
        std::cerr << "failed to open mp4: " << input_path << "\n";
        DumpMinimp4Tracks(input_path.c_str());
        return EXIT_FAILURE;
    }

    const auto& info = demuxer->video_info();
    std::cout << "codec=" << CodecName(info.codec)
              << " size=" << info.width << "x" << info.height
              << " fps=" << std::fixed << std::setprecision(3) << info.fps
              << " timescale=" << info.timescale
              << " samples=" << info.sample_count << "\n";

    int index = 0;
    int loop_count = 0;
    while (index < packet_count) {
        axvsdk::codec::EncodedPacket packet;
        if (!demuxer->ReadNextPacket(&packet)) {
            std::cout << "packet[" << index << "]=eof\n";
            if (!loop_playback) {
                break;
            }
            demuxer->Reset();
            ++loop_count;
            std::cout << "loop_reset=" << loop_count << "\n";
            continue;
        }

        const auto prefix_bytes = std::min<std::size_t>(8, packet.data.size());
        if (index < 3 || index >= packet_count - 3 || (loop_playback && index >= 178 && index <= 182)) {
            std::cout << "packet[" << index << "]"
                      << " pts=" << packet.pts
                      << " duration=" << packet.duration
                      << " bytes=" << packet.data.size()
                      << " key=" << (packet.key_frame ? 1 : 0)
                      << " prefix=";
            for (std::size_t byte_index = 0; byte_index < prefix_bytes; ++byte_index) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(packet.data[byte_index]);
            }
            std::cout << std::dec << "\n";
        }
        ++index;
    }

    return EXIT_SUCCESS;
}
