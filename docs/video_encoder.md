# 视频编码 (`ax_video_encoder.h`)

头文件:

- `include/codec/ax_video_encoder.h`

`VideoEncoder` 负责 `AxImage -> EncodedPacket`。

## 配置

`VideoEncoderConfig`:

- `codec`: `kH264` / `kH265`
- `width/height`: 输出分辨率
- `frame_rate`: 0 表示内部估算
- `bitrate_kbps`: 0 表示内部估算
- `gop`: 0 表示内部估算
- `input_queue_depth`: 0 表示内部默认
- `overflow_policy`:
  - `kDropOldest`: 队列满丢最老，优先保留最新画面(默认，适合实时)
  - `kDropNewest`: 丢最新
  - `kBlock`: 阻塞提交线程
- `resize`: 输入与目标尺寸不一致时的预处理策略(编码硬件最终输入为 NV12)

## 输入

`SubmitFrame(AxImage::Ptr)`:

- 允许输入 NV12/RGB/BGR 等
- 若不是目标 NV12 尺寸，内部会转换/缩放，并尽量复用中间 buffer
- host 普通内存输入时，库会在内部拷贝到设备侧/硬件可用内存后再编码

## 输出

### 显式取包

```cpp
axvsdk::codec::EncodedPacket pkt{};
if (encoder->GetLatestPacket(&pkt)) {
  // 写文件/推流...
}
```

### 回调取包

```cpp
encoder->SetPacketCallback([](axvsdk::codec::EncodedPacket pkt) {
  // pkt.data 位于 host 侧
});
```

## 统计

`GetStats()` 返回提交/丢帧/输出包/当前队列深度等信息，便于压测与调参。

