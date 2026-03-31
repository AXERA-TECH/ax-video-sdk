# 图像处理 (`ax_image_processor.h`)

头文件:

- `include/common/ax_image_processor.h`

`ImageProcessor` 提供统一的 crop/resize/csc 等图像处理能力，并尽量使用硬件 IVPS 加速。

## 关键类型

- `ImageProcessRequest`
- `CropRect`
- `ResizeOptions`
  - `ResizeMode::kStretch`: 拉伸到目标尺寸
  - `ResizeMode::kKeepAspectRatio`: 保持宽高比(留边填充)
  - `ResizeAlign`: 留边对齐方式

## Process 两种形态

### 1) 返回值版本(库分配输出)

适合更重视易用性、输出不需要复用的场景:

```cpp
auto proc = axvsdk::common::CreateImageProcessor();

axvsdk::common::ImageProcessRequest req{};
req.output_image.format = axvsdk::common::PixelFormat::kBgr24;
req.output_image.width = 640;
req.output_image.height = 640;
req.resize.mode = axvsdk::common::ResizeMode::kKeepAspectRatio;

auto out = proc->Process(*in, req);
```

### 2) 出参版本(调用方复用输出缓冲)

适合强调性能、减少反复申请释放的场景:

```cpp
auto dst = axvsdk::common::AxImage::Create(axvsdk::common::PixelFormat::kBgr24, 640, 640);
bool ok = proc->Process(*in, req, *dst);
```

## 背景色

`ResizeOptions::background_color` 按 `0xRRGGBB` 传入:

- RGB/BGR 输出: 直接使用该颜色
- NV12 输出: 底层会转换成对应 YUV 背景色

