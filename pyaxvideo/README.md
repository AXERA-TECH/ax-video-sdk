# pyaxvideo

ax-video-sdk 的 Python 绑定:在 AXERA 芯片上用 Python 跑**全硬件视频链路**——解码、编码、JPEG、crop / resize / 颜色空间转换全部走专用硬件,帧默认留在设备侧,给算法(NPU 推理)直接消费,只有真正需要时才拷回 numpy。

- **不挑 Python 版本**:纯 ctypes 绑定,一个 whl 覆盖 Python 3.8+(`py3-none-linux_*`)
- **平台**:`x86_64`(AXCL 算力卡)、`aarch64`(AXCL 卡 + AX650 板端双后端,自动或手动选择)
- **输入**:MP4 / MOV(含 iPhone 实拍 HEVC)/ RTSP;**输出**:MP4 文件 / RTSP

## 安装

whl 固定发布在 [`pyaxvideo-latest`](https://github.com/AXERA-TECH/ax-video-sdk/releases/tag/pyaxvideo-latest)(rolling,链接永久有效,内容随 main 更新):

```bash
# x86 主机 + AXCL 卡
pip install https://github.com/AXERA-TECH/ax-video-sdk/releases/download/pyaxvideo-latest/pyaxvideo-0.1.1-py3-none-linux_x86_64.whl
# aarch64(AXCL 卡 / AX650 板端)
pip install https://github.com/AXERA-TECH/ax-video-sdk/releases/download/pyaxvideo-latest/pyaxvideo-0.1.1-py3-none-linux_aarch64.whl
```

> 更新到最新构建:重跑上面的命令加 `--force-reinstall --no-deps`(版本号不自增,pip 不会自动认为有更新)。

板端无 pip 的裁剪系统:whl 就是 zip,`python3 -m zipfile -e xxx.whl site/` 解开后把
`site/pyaxvideo-*.data/purelib/pyaxvideo` 挪到 `site/` 下,`PYTHONPATH=site` 即可用(numpy 同理)。

## 快速开始

```python
import pyaxvideo as axv

axv.init(device=0)              # backend 自动探测;也可 backend="axcl"/"ax650",
                                # 或环境变量 PYAXVIDEO_BACKEND

with axv.VideoReader("cam.mp4") as r:          # mp4 / mov / rtsp://
    print(r.info)                              # ('h264', 1920, 1080, 30.0)
    for f in r:                                # f: DeviceFrame,设备侧零拷贝
        small = f.convert("rgb", 640, 384, mode="keep_aspect")   # IVPS 硬件
        img = small.to_numpy()                 # 需要时才下卡 → (384, 640, 3)
        jpg = f.crop(100, 100, 640, 360).to_jpeg(85)             # 硬件裁剪+JPEG
```

写出(编码走硬件,尺寸/格式不符自动 IVPS 转换):

```python
w = axv.VideoWriter("out.mp4", 1280, 720, codec="h264", fps=30)
# 或 "rtsp://0.0.0.0:8554/live"(本机起 RTSP server)/ rtsp://远端(推流,断链自动重连)
w.write(frame)          # DeviceFrame 或 numpy 数组都行
w.close()
```

## DeviceFrame:给硬件算法用的设备帧

```python
f.width, f.height, f.format      # 1920 1080 'nv12'
f.strides                        # 每 plane 字节跨度
f.phys_addr                      # 各 plane CMM 物理地址 → NPU 推理直连
f2 = f.clone()                   # 深拷贝到独立 CMM(要长期持有必须 clone)
```

解码 → IVPS 预处理 → NPU 推理可以全程零 host 拷贝:把 `convert()/crop()` 的输出帧
的 `phys_addr` 直接喂给推理引擎(AXCL `axclrtEngine` device buffer / 板端 `AX_ENGINE`
CMM 地址)。

**必读的生命周期规则**:`VideoReader` 给出的帧引用解码器缓冲池,攥着不放会把池子
耗尽、解码停摆——迭代中用完即弃,要跨帧持有先 `clone()`。`phys_addr` 随帧对象释放
而失效。

## 其他接口

```python
axv.decode_jpeg(jpg_bytes)       # 硬件 JPEG 解码 → DeviceFrame(NV12)
axv.frame_from_numpy(arr)        # numpy → 设备帧((h,w,3) BGR 或 NV12)
axv.backend()                    # "axcl" / "ax650"
```

`VideoReader(..., latest_only=True)` 实时算法只要最新帧;`realtime=True` 文件按源帧率
节奏送(模拟实时流);`loop=True` 循环播放。

## 资源占用

帧数据全程走 CMM/硬件模块,Python 侧只有句柄;单路 1080p30 解码 + IVPS 预处理的
CPU 占用与 C++ 直接调用 SDK 相当(百分之几)。`to_numpy()` 的 host 拷贝按需发生
(AXCL 下走 PCIe,1080p NV12 单帧约 3 MB)。

## 从源码构建 whl

```bash
cd pyaxvideo
./scripts/build_wheels.sh x86        # 需要本机装好 AXCL 用户态
# aarch64(交叉):
TOOLCHAIN_FILE=... MSP_DIR=... AXCL_ARM64_DIR=... ./scripts/build_wheels.sh aarch64
```

C ABI 层见 `include/capi/axvideo_capi.h`,其他语言绑定也从这层接。
