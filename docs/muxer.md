# Mux (`ax_muxer.h`)

头文件:

- `include/pipeline/ax_muxer.h`

`Muxer` 负责接收编码器输出的 `EncodedPacket`，并写入一个或多个目标:

- MP4 文件
- RTSP 推流

## 配置

`MuxerConfig`:

- `stream`: 输出码流信息(codec/宽高/fps)，通常由 encoder 配置决定
- `uris`: 输出目标列表，可同时包含 MP4 文件与 RTSP 地址

## 输入

```cpp
axvsdk::codec::EncodedPacket pkt{};
mux->SubmitPacket(std::move(pkt));  // pkt.data 位于 host 侧
```

## 说明

- “同一个 pipeline 既保存 MP4 又推 RTSP” 的典型做法是同一个 `Muxer` 打开多个 `uris`。
- RTSP 的 server/publisher 行为由内部实现选择:
  - 若端口已有 server，则复用并作为 publisher 发布
  - 否则启动 server

