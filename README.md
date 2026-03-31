# AX Video SDK

这是一个面向 AXERA 芯片的视频处理库，目标是对 AX 的视频编解码能力 做一层更清晰、边界更稳定、接口更易用的封装。

当前项目重点面向以下芯片：

- `AX650`
- `AX620E`
  - `AX630C`
  - `AX620Q`
  - `AX620QP`
- `AXCL`

其中：

- `AX650` 和 `AX620E(630C/620Q/620QP)` 和 `AXCL` 分开实现、分开维护
- 公共对外头文件尽量不直接暴露 MSP 头文件
- 最终交付以单个 SDK 动态库为主

## 主要能力

当前已经实现或正在完善的能力包括：

- 系统初始化与反初始化
- 自定义图像类 `AxImage`
- 视频解码
- 视频编码
- JPEG 编解码
- 图像处理
  - copy
  - crop
  - resize
  - cropresize
  - csc
- Pipeline
  - demux
  - mux
  - frame output
  - OSD

## 设计目标

这个项目的设计目标是：

- 接口尽量简单
- AX650 与 AX620E 系列差异隔离清楚
- 性能优先，尽量使用 AX 硬件能力和 CMM 内存
- 对用户暴露稳定、干净的 SDK 接口

## 目录说明

- [include](include)
  对外头文件
- [src](src)
  核心实现
- [tools](tools)
  内部测试工具、smoke 工具
- [third-party](third-party)
  第三方依赖
- [toolchains](toolchains)
  CMake toolchain 文件
- [docs](docs)
  API 文档
- [design](design)
  设计文档、测试记录

## 文档

API 文档入口:

- [docs/README.md](docs/README.md)

## 本地构建

### 1. 准备 MSP / AXCL SDK 压缩包

默认构建脚本会从仓库根目录下的 `.ci/downloads/` 查找对应平台所需的 SDK 压缩包：

- `msp_50_3.10.2.zip`
- `msp_20e_3.0.0.zip`
- `axcl_linux_3.10.2.zip`

也可以通过环境变量覆盖：

```bash
export AXSDK_MSP_ZIP_PATH=/path/to/msp_20e_3.0.0.zip
```

AXCL 也支持直接使用本机已安装目录：

```bash
export AXSDK_AXCL_DIR=/usr
```

### 2. 准备交叉编译器

构建脚本默认会从 `.ci/toolchains/.../bin` 查找编译器。

也可以通过环境变量覆盖：

```bash
export AXSDK_TOOLCHAIN_BIN=/path/to/toolchain/bin
```

脚本会在构建前先执行一次：

```bash
<compiler> -v
```

用来确认编译器可用。

### 3. 运行构建脚本

仓库根目录提供了六个脚本：

- [build_ax650.sh](/home/axera/ax_video_sdk/build_ax650.sh)
- [build_ax630c.sh](/home/axera/ax_video_sdk/build_ax630c.sh)
- [build_ax620q.sh](/home/axera/ax_video_sdk/build_ax620q.sh)
- [build_ax620qp.sh](/home/axera/ax_video_sdk/build_ax620qp.sh)
- [build_axcl_x86.sh](/home/axera/ax_video_sdk/build_axcl_x86.sh)
- [build_axcl_aarch64.sh](/home/axera/ax_video_sdk/build_axcl_aarch64.sh)

示例：

```bash
./build_ax650.sh
./build_ax630c.sh
./build_ax620q.sh
./build_ax620qp.sh
./build_axcl_x86.sh
./build_axcl_aarch64.sh
```

### 4. 构建产物

每个脚本都会：

- 生成独立 build 目录
- 编译 `libax_video_sdk.so`
- 复制对外头文件 `include/`
- 打包成可分发产物

产物默认输出到：

- `artifacts/ax650/ax_video_sdk_ax650.tar.gz`
- `artifacts/ax630c/ax_video_sdk_ax630c.tar.gz`
- `artifacts/ax620q/ax_video_sdk_ax620q.tar.gz`
- `artifacts/ax620qp/ax_video_sdk_ax620qp.tar.gz`
- `artifacts/axcl-x86_64/ax_video_sdk_axcl-x86_64.tar.gz`
- `artifacts/axcl-aarch64/ax_video_sdk_axcl-aarch64.tar.gz`

压缩包中包含：

- `include/`
- `lib/libax_video_sdk.so`
- `BUILD_INFO.txt`

## CI

仓库已经补了 GitHub Actions：

- [.github/workflows/build-sdk.yml](/home/axera/ax_video_sdk/.github/workflows/build-sdk.yml)

CI 会自动：

- 下载 MSP 压缩包
- 下载 AXCL SDK 压缩包
- 下载对应交叉编译器
- 解压到工作目录下 `.ci/`
- 调用六个 `build_*.sh`
- 上传每个平台的 SDK 打包产物

为了避免重复下载，CI 对以下目录做了缓存：

- `.ci/downloads`
- `.ci/toolchains`
- `.ci/msp`
- `.ci/axcl`

## 当前状态

### AX620E

- H.264 decode 可用
- H.264 / H.265 encode 可用
- OSD 可用
- 图像处理基础链路可用
- frame output 可用
- 单进程双 pipeline 1080p 编解码短压测通过

已知边界：

- `AX630C / AX620Q / AX620QP` 当前都按 `H.264 decode only` 处理，不支持 H.265 decode
- `AX620Q / AX620QP` 的解码能力依赖驱动；若板子上没有 `/soc/ko/ax_vdec.ko`，则用户需要先自行安装对应驱动，否则解码不可用
- 跨进程双路测试目前不应作为有效容量测试
- AX620E 系列不按高并发多路设计，现实目标以 `1~2 路 1080p` 为主

### AXCL

- AXCL 后端当前支持单进程多卡，`Pipeline` 级别固定单卡
- 视频编解码、JPEG 编解码、图像处理、frame output 已打通
- AXCL 下 `GetLatestFrame` / frame callback 返回的默认是设备侧图像；如需访问 host 内存，需要显式拷贝到 host

已知边界：

- AXCL 当前的 OSD 仅支持 `rect`
- `line / polygon / mosaic / bitmap` 这些绘图类型在 AXCL 上暂未开放，原因是运行时稳定性不足，当前版本会直接拒绝
- AXCL 下应避免并行运行多个测试进程，否则容易受到 PCIe/runtime 干扰

## 说明

这个项目当前仍处于快速迭代阶段，接口和实现还会继续收敛。现阶段优先级是：

- 边界清晰
- AX650 / AX620E 系列差异隔离
- 编解码、图像处理、OSD、Pipeline 这些核心能力先稳定下来
