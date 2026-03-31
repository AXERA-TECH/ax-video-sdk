# JPEG 编解码 (`ax_jpeg_codec.h`)

头文件:

- `include/codec/ax_jpeg_codec.h`

该模块以“函数 API”形式提供 JPEG decode/encode，内部尽量使用硬件能力并屏蔽平台差异。

## Decode

支持输入:

- 文件: `DecodeJpegFile(path)`
- 内存: `DecodeJpegMemory(ptr, size)`
- Base64: `DecodeJpegBase64(str)`

`JpegDecodeOptions::output_image`:

- 为空时默认输出 JPEG 原始尺寸的 NV12
- 也可指定输出为 RGB/BGR 或指定输出分辨率(内部做缩放/转换)

返回值:

- 返回新 `AxImage`(通常为板端 CMM 或 AXCL 设备侧图像)

## Encode

支持输出:

- 内存: `EncodeJpegToMemory(image)`
- 文件: `EncodeJpegToFile(image, path)`
- Base64: `EncodeJpegToBase64(image)`

注意:

- JPEG 编码输出始终位于 host 侧(可直接写文件/网络发送)。
- 输入图像不是硬件可直接编码格式时，库会先内部转换到合适的设备侧图像。

