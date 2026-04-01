#include "ax_mp4_demuxer.h"
#include "ax_mp4_internal.h"

#define MINIMP4_IMPLEMENTATION
#include "minimp4/minimp4.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cstdio>

namespace axvsdk::codec {

namespace {

// Some AX VDEC paths are picky about the start code length for AnnexB streams.
// Prefer 4-byte start codes (0x00000001), which matches most raw H.264/H.265 bitstreams and MSP samples.
constexpr std::array<std::uint8_t, 4> kAnnexBStartCode{0x00, 0x00, 0x00, 0x01};

struct NalUnitView {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    std::uint8_t type{0};
};

std::vector<std::uint8_t> ReadFileBytes(const std::string& file_path) {
    std::ifstream input(file_path, std::ios::binary);
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

VideoCodecType ToCodecType(unsigned object_type_indication) noexcept {
    switch (object_type_indication) {
    case MP4_OBJECT_TYPE_AVC:
        return VideoCodecType::kH264;
    case MP4_OBJECT_TYPE_HEVC:
        return VideoCodecType::kH265;
    default:
        return VideoCodecType::kUnknown;
    }
}

std::uint32_t ReadBigEndian(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

std::uint8_t GetAvcNalType(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0) {
        return 0;
    }
    return static_cast<std::uint8_t>(data[0] & 0x1FU);
}

std::uint8_t GetHevcNalType(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 2) {
        return 0;
    }
    return static_cast<std::uint8_t>((data[0] >> 1U) & 0x3FU);
}

bool IsKeyFrame(VideoCodecType codec, std::uint8_t nal_type) noexcept {
    switch (codec) {
    case VideoCodecType::kH264:
        return nal_type == 5;
    case VideoCodecType::kH265:
        return nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_CRA_NUT;
    case VideoCodecType::kUnknown:
    case VideoCodecType::kJpeg:
    default:
        return false;
    }
}

bool ParseLengthPrefixedNalUnits(VideoCodecType codec,
                                 const std::uint8_t* sample,
                                 std::size_t sample_size,
                                 std::size_t nal_length_size,
                                 std::vector<NalUnitView>* nal_units,
                                 bool* key_frame) {
    if (sample == nullptr || sample_size == 0 || nal_units == nullptr || key_frame == nullptr) {
        return false;
    }

    if (nal_length_size == 0 || nal_length_size > 4) {
        return false;
    }

    nal_units->clear();
    *key_frame = false;

    std::size_t offset = 0;
    while (offset + nal_length_size <= sample_size) {
        const auto nal_size = static_cast<std::size_t>(ReadBigEndian(sample + offset, nal_length_size));
        offset += nal_length_size;

        if (nal_size == 0 || offset + nal_size > sample_size) {
            return false;
        }

        const auto* nal_data = sample + offset;
        const auto nal_type = codec == VideoCodecType::kH264
                                  ? GetAvcNalType(nal_data, nal_size)
                                  : GetHevcNalType(nal_data, nal_size);
        nal_units->push_back({nal_data, nal_size, nal_type});
        *key_frame = *key_frame || IsKeyFrame(codec, nal_type);

        offset += nal_size;
    }

    return !nal_units->empty() && offset == sample_size;
}

bool ParseLengthPrefixedNalUnitsAuto(VideoCodecType codec,
                                     const std::uint8_t* sample,
                                     std::size_t sample_size,
                                     std::size_t preferred_nal_length_size,
                                     std::vector<NalUnitView>* nal_units,
                                     bool* key_frame,
                                     std::size_t* resolved_nal_length_size) {
    if (resolved_nal_length_size == nullptr) {
        return false;
    }

    const std::array<std::size_t, 4> candidates{preferred_nal_length_size, 4U, 2U, 1U};
    for (const auto candidate : candidates) {
        if (candidate == 0 || candidate > 4) {
            continue;
        }
        if (ParseLengthPrefixedNalUnits(codec, sample, sample_size, candidate, nal_units, key_frame)) {
            *resolved_nal_length_size = candidate;
            return true;
        }
    }

    return false;
}

void AppendAnnexBUnit(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>* output) {
    output->insert(output->end(), kAnnexBStartCode.begin(), kAnnexBStartCode.end());
    output->insert(output->end(), data, data + size);
}

double EstimateFrameRate(const MP4D_demux_t& demux, unsigned track_index, unsigned sample_count) {
    if (sample_count == 0 || track_index >= demux.track_count) {
        return 0.0;
    }

    const auto& track = demux.track[track_index];
    if (track.timescale == 0) {
        return 0.0;
    }

    const auto window = std::min(sample_count, 30U);
    std::uint64_t duration_sum = 0;
    for (unsigned index = 0; index < window; ++index) {
        unsigned sample_bytes = 0;
        unsigned timestamp = 0;
        unsigned duration = 0;
        (void)MP4D_frame_offset(&demux, track_index, index, &sample_bytes, &timestamp, &duration);
        duration_sum += duration;
    }

    if (duration_sum == 0) {
        return 0.0;
    }

    return static_cast<double>(track.timescale) * static_cast<double>(window) /
           static_cast<double>(duration_sum);
}

bool CollectAvcDecoderConfig(const MP4D_demux_t& demux,
                             unsigned track_index,
                             std::size_t* nal_length_size,
                             std::vector<std::vector<std::uint8_t>>* config_units) {
    if (nal_length_size == nullptr || config_units == nullptr) {
        return false;
    }

    const auto& track = demux.track[track_index];
    if (track.dsi == nullptr || track.dsi_bytes < 5) {
        return false;
    }

    *nal_length_size = static_cast<std::size_t>((track.dsi[4] & 0x03U) + 1U);
    config_units->clear();

    for (int sps_index = 0;; ++sps_index) {
        int sps_bytes = 0;
        const auto* sps = static_cast<const std::uint8_t*>(MP4D_read_sps(&demux, track_index, sps_index, &sps_bytes));
        if (sps == nullptr || sps_bytes <= 0) {
            break;
        }
        config_units->emplace_back(sps, sps + sps_bytes);
    }

    for (int pps_index = 0;; ++pps_index) {
        int pps_bytes = 0;
        const auto* pps = static_cast<const std::uint8_t*>(MP4D_read_pps(&demux, track_index, pps_index, &pps_bytes));
        if (pps == nullptr || pps_bytes <= 0) {
            break;
        }
        config_units->emplace_back(pps, pps + pps_bytes);
    }

    return true;
}

bool CollectHevcDecoderConfig(const MP4D_demux_t& demux,
                              unsigned track_index,
                              std::size_t* nal_length_size,
                              std::vector<std::vector<std::uint8_t>>* config_units) {
    if (nal_length_size == nullptr || config_units == nullptr) {
        return false;
    }

    const auto& track = demux.track[track_index];
    if (track.dsi == nullptr || track.dsi_bytes < 23) {
        return false;
    }

    *nal_length_size = static_cast<std::size_t>((track.dsi[21] & 0x03U) + 1U);
    config_units->clear();

    std::size_t offset = 22;
    const auto num_of_arrays = static_cast<std::size_t>(track.dsi[offset++]);
    for (std::size_t array_index = 0; array_index < num_of_arrays; ++array_index) {
        if (offset + 3 > track.dsi_bytes) {
            return true;
        }

        offset += 1;  // completeness + nal unit type
        const auto num_nalus = static_cast<std::size_t>(ReadBigEndian(track.dsi + offset, 2));
        offset += 2;

        for (std::size_t nal_index = 0; nal_index < num_nalus; ++nal_index) {
            if (offset + 2 > track.dsi_bytes) {
                return true;
            }

            const auto nal_bytes = static_cast<std::size_t>(ReadBigEndian(track.dsi + offset, 2));
            offset += 2;
            if (offset + nal_bytes > track.dsi_bytes) {
                return true;
            }

            config_units->emplace_back(track.dsi + offset, track.dsi + offset + nal_bytes);
            offset += nal_bytes;
        }
    }

    return true;
}

}  // namespace

struct AxMp4Demuxer::Impl {
    std::vector<std::uint8_t> file_bytes;
    MP4D_demux_t demux{};
    Mp4VideoInfo video_info{};
    unsigned track_index{0};
    unsigned sample_index{0};
    std::size_t nal_length_size{0};
    bool decoder_config_sent{false};
    bool opened{false};
    std::vector<std::vector<std::uint8_t>> decoder_config_units;

    ~Impl() {
        if (opened) {
            MP4D_close(&demux);
        }
    }
};

std::unique_ptr<AxMp4Demuxer> AxMp4Demuxer::Open(const std::string& file_path) {
    auto impl = std::make_unique<Impl>();
    impl->file_bytes = ReadFileBytes(file_path);
    if (impl->file_bytes.empty()) {
        return nullptr;
    }

    if (MP4D_open(&impl->demux, ReadFromMemory, &impl->file_bytes,
                  static_cast<int64_t>(impl->file_bytes.size())) != 1) {
        return nullptr;
    }
    impl->opened = true;

    for (unsigned index = 0; index < impl->demux.track_count; ++index) {
        const auto& track = impl->demux.track[index];
        const auto codec = ToCodecType(track.object_type_indication);
        if (codec == VideoCodecType::kUnknown || track.SampleDescription.video.width == 0 ||
            track.SampleDescription.video.height == 0) {
            continue;
        }

        impl->track_index = index;
        impl->video_info.codec = codec;
        impl->video_info.width = static_cast<std::uint32_t>(track.SampleDescription.video.width);
        impl->video_info.height = static_cast<std::uint32_t>(track.SampleDescription.video.height);
        impl->video_info.timescale = static_cast<std::uint32_t>(track.timescale);
        impl->video_info.sample_count = static_cast<std::uint32_t>(track.sample_count);
        impl->video_info.fps = EstimateFrameRate(impl->demux, index, track.sample_count);

        const bool config_ok = codec == VideoCodecType::kH264
                                   ? CollectAvcDecoderConfig(impl->demux, index, &impl->nal_length_size,
                                                             &impl->decoder_config_units)
                                   : CollectHevcDecoderConfig(impl->demux, index, &impl->nal_length_size,
                                                              &impl->decoder_config_units);
        if (!config_ok || impl->nal_length_size == 0) {
            return nullptr;
        }

        return std::unique_ptr<AxMp4Demuxer>(new AxMp4Demuxer(std::move(impl)));
    }

    return nullptr;
}

AxMp4Demuxer::AxMp4Demuxer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AxMp4Demuxer::AxMp4Demuxer(AxMp4Demuxer&&) noexcept = default;

AxMp4Demuxer& AxMp4Demuxer::operator=(AxMp4Demuxer&&) noexcept = default;

AxMp4Demuxer::~AxMp4Demuxer() = default;

const Mp4VideoInfo& AxMp4Demuxer::video_info() const noexcept {
    return impl_->video_info;
}

bool AxMp4Demuxer::ReadNextPacket(EncodedPacket* packet) {
    if (packet == nullptr || impl_->sample_index >= impl_->video_info.sample_count) {
        return false;
    }

    unsigned frame_bytes = 0;
    unsigned dts = 0;
    unsigned duration = 0;
    const auto offset = static_cast<std::size_t>(
        MP4D_frame_offset(&impl_->demux, impl_->track_index, impl_->sample_index, &frame_bytes, &dts, &duration));

    if (offset > impl_->file_bytes.size() || frame_bytes > (impl_->file_bytes.size() - offset)) {
        return false;
    }

    const auto* sample = impl_->file_bytes.data() + offset;
    std::vector<NalUnitView> nal_units;
    bool key_frame = false;
    std::size_t resolved_nal_length_size = impl_->nal_length_size;
    if (!ParseLengthPrefixedNalUnitsAuto(impl_->video_info.codec, sample, frame_bytes, impl_->nal_length_size,
                                         &nal_units, &key_frame, &resolved_nal_length_size)) {
        return false;
    }
    if (resolved_nal_length_size != impl_->nal_length_size) {
        impl_->nal_length_size = resolved_nal_length_size;
    }

    packet->codec = impl_->video_info.codec;
    // minimp4 provides DTS in stts, but does not apply CTTS to timestamps.
    // For streams with B-frames, feeding DTS as PTS causes visible "jitter" (display-order issues) on VDEC.
    // Compute PTS = DTS + CTTS (track timescale units). The pipeline demuxer is responsible for any normalization.
    const auto* tr = impl_->demux.track + impl_->track_index;
    std::int64_t pts_ts = static_cast<std::int64_t>(dts);
#if MP4D_TIMESTAMPS_SUPPORTED
    if (tr != nullptr && tr->composition_offset != nullptr) {
        pts_ts += static_cast<std::int64_t>(tr->composition_offset[impl_->sample_index]);
    }
#endif
    if (pts_ts < 0) {
        pts_ts = 0;
    }
    const auto timescale = impl_->video_info.timescale;
    if (timescale == 0) {
        return false;
    }
    // Normalize to microseconds for downstream VDEC/VENC logic.
    packet->pts = (static_cast<std::uint64_t>(pts_ts) * 1000000ULL) / timescale;
    packet->duration = (static_cast<std::uint64_t>(duration) * 1000000ULL) / timescale;
    packet->key_frame = key_frame;
    packet->data.clear();

    if (!impl_->decoder_config_sent || key_frame) {
        for (const auto& unit : impl_->decoder_config_units) {
            AppendAnnexBUnit(unit.data(), unit.size(), &packet->data);
        }
        impl_->decoder_config_sent = true;
    }

    for (const auto& nal_unit : nal_units) {
        AppendAnnexBUnit(nal_unit.data, nal_unit.size, &packet->data);
    }

    ++impl_->sample_index;
    return true;
}

void AxMp4Demuxer::Reset() noexcept {
    impl_->sample_index = 0;
    impl_->decoder_config_sent = false;
}

}  // namespace axvsdk::codec

namespace axvsdk::codec::internal {

namespace {

bool FindSecondLastStartCode(const std::vector<std::uint8_t>& data, std::size_t* second_last, std::size_t* last) {
    if (second_last == nullptr || last == nullptr) {
        return false;
    }
    *second_last = std::string::npos;
    *last = std::string::npos;

    const auto* p = data.data();
    const std::size_t n = data.size();
    if (p == nullptr || n < 3) {
        return false;
    }

    auto IsStartCodeAt = [&](std::size_t i) -> bool {
        if (i + 3 > n) return false;
        if (p[i] != 0x00 || p[i + 1] != 0x00) return false;
        if (p[i + 2] == 0x01) return true;
        if (i + 4 <= n && p[i + 2] == 0x00 && p[i + 3] == 0x01) return true;
        return false;
    };

    for (std::size_t i = 0; i + 3 <= n; ++i) {
        if (!IsStartCodeAt(i)) continue;
        *second_last = *last;
        *last = i;
        // Skip over the common 4-byte start code to reduce redundant matches.
        if (i + 4 <= n && p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x00 && p[i + 3] == 0x01) {
            i += 3;
        } else if (i + 3 <= n && p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x01) {
            i += 2;
        }
    }

    return *second_last != std::string::npos;
}

std::size_t FindFirstStartCode(const std::vector<std::uint8_t>& data) {
    const auto* p = data.data();
    const std::size_t n = data.size();
    if (p == nullptr || n < 3) {
        return std::string::npos;
    }
    for (std::size_t i = 0; i + 3 <= n; ++i) {
        if (p[i] != 0x00 || p[i + 1] != 0x00) continue;
        if (p[i + 2] == 0x01) return i;
        if (i + 4 <= n && p[i + 2] == 0x00 && p[i + 3] == 0x01) return i;
    }
    return std::string::npos;
}

int WriteToFile(int64_t offset, const void* buffer, size_t size, void* token) {
    if (offset < 0 || buffer == nullptr || token == nullptr) {
        return -1;
    }

    auto* file = static_cast<std::FILE*>(token);
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return -1;
    }

    return std::fwrite(buffer, 1, size, file) == size ? 0 : -1;
}

unsigned DurationTo90kHz(std::uint64_t duration_us, double frame_rate) noexcept {
    if (duration_us != 0) {
        // Use rounding (not floor) to avoid systematic drift for common frame durations like 33333us.
        // 33333us should map to 3000 ticks at 90kHz (30fps).
        const auto scaled = (duration_us * 90000ULL + 500000ULL) / 1000000ULL;
        return static_cast<unsigned>(std::max<std::uint64_t>(1ULL, scaled));
    }

    const double effective_fps = frame_rate > 0.0 ? frame_rate : 30.0;
    const auto scaled = static_cast<std::uint64_t>(std::llround(90000.0 / effective_fps));
    return static_cast<unsigned>(std::max<std::uint64_t>(1ULL, scaled));
}

}  // namespace

struct Mp4FileMuxer::Impl {
    std::FILE* file{nullptr};
    MP4E_mux_t* mux{nullptr};
    mp4_h26x_writer_t writer{};
    VideoStreamInfo stream{};
    bool writer_open{false};
};

std::unique_ptr<Mp4FileMuxer> Mp4FileMuxer::Open(const std::string& file_path, const VideoStreamInfo& stream) {
    if (file_path.empty() || (stream.codec != VideoCodecType::kH264 && stream.codec != VideoCodecType::kH265) ||
        stream.width == 0 || stream.height == 0) {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->file = std::fopen(file_path.c_str(), "wb");
    if (impl->file == nullptr) {
        return nullptr;
    }

    impl->mux = MP4E_open(1, 1, impl->file, WriteToFile);
    if (impl->mux == nullptr) {
        std::fclose(impl->file);
        return nullptr;
    }

    const int init_status =
        mp4_h26x_write_init(&impl->writer, impl->mux, static_cast<int>(stream.width), static_cast<int>(stream.height),
                            stream.codec == VideoCodecType::kH265 ? 1 : 0);
    if (init_status != MP4E_STATUS_OK) {
        (void)MP4E_close(impl->mux);
        std::fclose(impl->file);
        return nullptr;
    }

    impl->writer_open = true;
    impl->stream = stream;
    return std::unique_ptr<Mp4FileMuxer>(new Mp4FileMuxer(std::move(impl)));
}

Mp4FileMuxer::Mp4FileMuxer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Mp4FileMuxer::Mp4FileMuxer(Mp4FileMuxer&&) noexcept = default;

Mp4FileMuxer& Mp4FileMuxer::operator=(Mp4FileMuxer&&) noexcept = default;

Mp4FileMuxer::~Mp4FileMuxer() {
    Close();
}

bool Mp4FileMuxer::WritePacket(const EncodedPacket& packet) {
    if (!impl_ || impl_->mux == nullptr || packet.data.empty() || packet.data.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }

    const auto codec = packet.codec == VideoCodecType::kUnknown ? impl_->stream.codec : packet.codec;
    if (codec != impl_->stream.codec) {
        return false;
    }

    // We use MP4E_put_sample directly and must pass a *sample duration* in 90kHz timebase.
    const double fps = impl_->stream.frame_rate > 0.0 ? impl_->stream.frame_rate : 30.0;
    const auto fallback_dur_us_ll = std::llround(1000000.0 / fps);
    const std::uint64_t fallback_dur_us =
        static_cast<std::uint64_t>(fallback_dur_us_ll <= 0 ? 1LL : fallback_dur_us_ll);
    const std::uint64_t dur_us = packet.duration != 0 ? packet.duration : fallback_dur_us;
    const unsigned dur90 = DurationTo90kHz(dur_us, fps);

    // Encode path guarantee (per MSP semantics): AX_VENC_GetStream returns one pack per encoded frame.
    // Treat each packet as a complete access unit (AU) and mux one MP4 sample per WritePacket call.
    const auto first = FindFirstStartCode(packet.data);
    if (first == std::string::npos) {
        return false;
    }

    const std::uint8_t* cur = packet.data.data() + first;
    const std::uint8_t* eof = packet.data.data() + packet.data.size();
    bool is_key = packet.key_frame;
    std::vector<std::uint8_t> sample;
    sample.reserve(packet.data.size() + 64);

    for (;;) {
        int nal_size = 0;
        const std::uint8_t* nal = find_nal_unit(cur, static_cast<int>(eof - cur), &nal_size);
        if (nal_size <= 0 || nal == nullptr) {
            break;
        }

        if (impl_->stream.codec == VideoCodecType::kH265) {
            const int t = (nal[0] >> 1) & 0x3f;
            if (t == HEVC_NAL_VPS) {
                MP4E_set_vps(impl_->mux, impl_->writer.mux_track_id, nal, nal_size);
                cur = nal + nal_size;
                continue;
            }
            if (t == HEVC_NAL_SPS) {
                MP4E_set_sps(impl_->mux, impl_->writer.mux_track_id, nal, nal_size);
                cur = nal + nal_size;
                continue;
            }
            if (t == HEVC_NAL_PPS) {
                MP4E_set_pps(impl_->mux, impl_->writer.mux_track_id, nal, nal_size);
                cur = nal + nal_size;
                continue;
            }
            const bool intra = (t >= HEVC_NAL_BLA_W_LP && t <= HEVC_NAL_CRA_NUT);
            if (intra) is_key = true;
        } else {
            const int t = nal[0] & 31;
            if (t == 9) {  // AUD
                cur = nal + nal_size;
                continue;
            }
            if (t == 7) {
                MP4E_set_sps(impl_->mux, impl_->writer.mux_track_id, nal, nal_size);
                cur = nal + nal_size;
                continue;
            }
            if (t == 8) {
                MP4E_set_pps(impl_->mux, impl_->writer.mux_track_id, nal, nal_size);
                cur = nal + nal_size;
                continue;
            }
            if (t == 5) is_key = true;
        }

        // Append length-prefixed NAL to the sample buffer.
        const std::uint32_t sz = static_cast<std::uint32_t>(nal_size);
        sample.push_back(static_cast<std::uint8_t>(sz >> 24));
        sample.push_back(static_cast<std::uint8_t>(sz >> 16));
        sample.push_back(static_cast<std::uint8_t>(sz >> 8));
        sample.push_back(static_cast<std::uint8_t>(sz));
        sample.insert(sample.end(), nal, nal + nal_size);

        cur = nal + nal_size;
    }

    // Packet may contain only codec config (VPS/SPS/PPS/AUD). No video sample to write in that case.
    if (sample.empty()) {
        return true;
    }

    const int kind = is_key ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT;
    const int put = MP4E_put_sample(impl_->mux,
                                    impl_->writer.mux_track_id,
                                    sample.data(),
                                    static_cast<int>(sample.size()),
                                    static_cast<int>(std::min<unsigned>(dur90, INT_MAX)),
                                    kind);
    return put == MP4E_STATUS_OK;
}

void Mp4FileMuxer::Close() noexcept {
    if (!impl_) {
        return;
    }

    if (impl_->mux != nullptr) {
        (void)MP4E_close(impl_->mux);
        impl_->mux = nullptr;
    }
    if (impl_->writer_open) {
        mp4_h26x_write_close(&impl_->writer);
        impl_->writer_open = false;
    }
    if (impl_->file != nullptr) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

}  // namespace axvsdk::codec::internal
