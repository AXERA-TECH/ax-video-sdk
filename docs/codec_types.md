# Codec 公共类型 (`ax_codec_types.h`)

头文件:

- `include/codec/ax_codec_types.h`

## VideoCodecType

`VideoCodecType`:

- `kH264`
- `kH265`
- `kJpeg`

## EncodedPacket

`EncodedPacket` 表示一包 host 侧编码数据:

- `codec`: 编码类型
- `data`: 字节数组(通常为 AnnexB)
- `pts` / `duration`: 时间戳信息(单位与上层一致，通常为 us 或 timescale 对齐后的值)
- `key_frame`: 是否关键帧

解码器与 demuxer 之间、编码器与 muxer 之间都使用该结构传递数据。

## VideoStreamInfo / Mp4VideoInfo

- `VideoStreamInfo`: 通用视频流元信息(codec/宽高/fps)
- `Mp4VideoInfo`: MP4 输入的补充信息(timescale/sample_count 等)

