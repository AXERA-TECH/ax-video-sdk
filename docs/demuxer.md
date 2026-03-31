# Demux (`ax_demuxer.h`)

头文件:

- `include/pipeline/ax_demuxer.h`

`Demuxer` 负责从输入 URI 读取视频数据并输出 `EncodedPacket`，常用于:

- MP4 文件离线转码
- RTSP 拉流

## 输入类型识别

`DetectDemuxerInputType(uri, &type)` 会根据 URI 前缀/后缀做自动识别，通常不需要用户显式指定。

## 配置

`DemuxerConfig`:

- `uri`: MP4 文件路径或 `rtsp://...`
- `realtime_playback`:
  - MP4 输入时:
    - `true`: 按源 fps 节奏送包(模拟实时源/播放器语义)
    - `false`: 尽快读取(离线转码)
- `loop_playback`: 仅对可 reset 的文件型输入生效(MP4)

## 读取

```cpp
axvsdk::codec::EncodedPacket pkt{};
while (demux->ReadPacket(&pkt)) {
  // ...
}
```

控制:

- `Reset()`: 复位输入(例如 MP4 循环播放)
- `Interrupt()`: 中断阻塞读(例如退出时唤醒)

