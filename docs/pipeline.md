# Pipeline (`ax_pipeline.h`)

头文件:

- `include/pipeline/ax_pipeline.h`

`Pipeline` 是面向应用的“端到端”链路:

`demux -> decoder -> (optional osd/frame_output) -> N x (encoder -> mux)`

## 配置结构

`PipelineConfig`:

- `device_id`: AXCL 下建议显式指定；一个 pipeline 只绑定一张卡
- `input`: `DemuxerConfig`(URI 自动识别 MP4/RTSP)
- `outputs`: N 路输出，每路一个 `PipelineOutputConfig`
- `frame_output`: 控制 `GetLatestFrame` / frame callback 的输出格式/尺寸/缩放策略

`PipelineOutputConfig` 常用字段:

- `codec`: H264/H265
- `width/height/frame_rate/bitrate_kbps/gop`: 传 0 表示走默认策略(跟随输入或自动估算)
- `overflow_policy`: 输入队列满时的丢帧策略(默认丢最老)
- `resize`: 输出尺寸与输入不一致时的缩放策略
- `uris`: 可同时输出到多个目标(MP4 + RTSP)
- `packet_callback`: 额外编码包回调(默认 nullptr)

## 取帧与回调

- `GetLatestFrame()` / `GetLatestFrame(out)`:
  - 语义为“最新一帧”
  - 只有在你取帧时才会做必要的拷贝/缩放/格式转换
- `SetFrameCallback(cb)`:
  - 只有注册回调时，pipeline 才会为回调准备额外输出图像

## OSD

- `SetOsd(DrawFrame)`:
  - 异步设置，不阻塞 demux/dec/enc 主链路
  - 建议一次调用把需要同时绘制的 OSD 都放在同一个 `DrawFrame` 中
- `hold_frames` 控制该次 OSD 生效帧数

## 统计

`GetStats()` 返回:

- `decoded_frames`
- `branch_submit_failures`
- 每路输出 `VideoEncoderStats`

