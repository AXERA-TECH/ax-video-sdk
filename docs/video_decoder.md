# 视频解码 (`ax_video_decoder.h`)

头文件:

- `include/codec/ax_video_decoder.h`

`VideoDecoder` 负责 `EncodedPacket -> AxImage`，不负责 demux(文件/RTSP 拆包)。

## 配置

`VideoDecoderConfig`:

- `stream`: 输入码流信息(codec/宽高/fps)
- `output_image`: GetLatestFrame/callback 的默认输出图像描述
  - `format/width/height` 为空(0 或 Unknown)表示保持硬件原始输出(通常 NV12 + 原始分辨率)
- `device_id`: AXCL 下建议显式指定；板端通常保持 `-1`

## 输入

使用 `SubmitPacket()` 提交 demux 后的编码包:

```cpp
axvsdk::codec::EncodedPacket pkt{};
decoder->SubmitPacket(std::move(pkt));
```

提交 EOS:

- `SubmitEndOfStream()` 让内部线程完成 drain。

## 输出(显式取帧)

### 返回值版本

- 语义: “最新一帧”
- 仅在你真正取帧时才会做必要的拷贝/变换

```cpp
auto frame = decoder->GetLatestFrame();
```

### 出参版本

调用方复用输出缓冲:

```cpp
auto out = axvsdk::common::AxImage::Create(...);
decoder->GetLatestFrame(*out);
```

## 输出(回调)

回调与解码线程解耦，慢回调不会阻塞底层解码线程本身。

```cpp
decoder->SetFrameCallback([](axvsdk::common::AxImage::Ptr frame) {
  // AI/保存/处理...
});
```

`FrameCallbackMode`:

- `kLatest`: 回调线程只保留最新帧(慢回调时中间帧丢弃)
- `kQueue`: 内部有界队列按顺序投递(慢回调时丢弃最旧帧，避免阻塞解码)

## 平台差异与能力边界(摘要)

- 解码输出原生通常为 NV12。
- AX620E 系列的解码能力依赖具体芯片与驱动；例如部分平台可能只支持 H.264 解码。
- AXCL 设备侧图像默认在卡上；如需 host 访问需要显式拷贝。

