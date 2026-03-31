# AX Video SDK 文档

本文档面向使用 `axvsdk` C++ API 的开发者，目标是说明各模块的职责边界、关键接口、默认行为与平台差异。

重要约束:

- 在使用任何 codec / pipeline / 图像处理模块前，必须先调用 `axvsdk::common::InitializeSystem()`。
- SDK 尽量避免在“编解码主链路”上做不必要的拷贝: 图像拷贝与格式/尺寸变换通常只会发生在你显式取帧或注册回调时。

## 快速开始

```cpp
#include "common/ax_system.h"
#include "pipeline/ax_pipeline.h"

int main() {
  axvsdk::common::InitializeSystem();

  axvsdk::pipeline::PipelineConfig cfg{};
  cfg.input.uri = "input.mp4";
  cfg.outputs.push_back({.codec = axvsdk::codec::VideoCodecType::kH264,
                         .uris = {"/tmp/out.mp4"}});

  auto pipe = axvsdk::pipeline::CreatePipeline();
  pipe->Open(cfg);
  pipe->Start();

  // 取最新一帧(默认解码原图, 通常 NV12)。
  auto frame = pipe->GetLatestFrame();

  pipe->Stop();
  pipe->Close();

  axvsdk::common::ShutdownSystem();
  return 0;
}
```

## 模块文档

- 系统初始化: [system.md](system.md)
- 图像(内存/格式/生命周期): [image.md](image.md)
- 图像处理(crop/resize/csc): [image_processor.md](image_processor.md)
- 绘图/OSD: [drawer.md](drawer.md)
- Codec 公共类型: [codec_types.md](codec_types.md)
- 视频解码: [video_decoder.md](video_decoder.md)
- 视频编码: [video_encoder.md](video_encoder.md)
- JPEG 编解码: [jpeg_codec.md](jpeg_codec.md)
- Demux: [demuxer.md](demuxer.md)
- Mux: [muxer.md](muxer.md)
- Pipeline: [pipeline.md](pipeline.md)
- PipelineManager: [pipeline_manager.md](pipeline_manager.md)
- 平台差异与能力边界: [platforms.md](platforms.md)

