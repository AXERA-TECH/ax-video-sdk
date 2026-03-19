#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "codec/ax_codec_types.h"

namespace axvsdk::codec {

class AxMp4Demuxer {
public:
    static std::unique_ptr<AxMp4Demuxer> Open(const std::string& file_path);

    AxMp4Demuxer(const AxMp4Demuxer&) = delete;
    AxMp4Demuxer& operator=(const AxMp4Demuxer&) = delete;
    AxMp4Demuxer(AxMp4Demuxer&&) noexcept;
    AxMp4Demuxer& operator=(AxMp4Demuxer&&) noexcept;
    ~AxMp4Demuxer();

    const Mp4VideoInfo& video_info() const noexcept;
    bool ReadNextPacket(EncodedPacket* packet);
    void Reset() noexcept;

private:
    struct Impl;

    explicit AxMp4Demuxer(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace axvsdk::codec
