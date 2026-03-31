# 系统初始化 (`ax_system.h`)

头文件:

- `include/common/ax_system.h`

`axvsdk::common::InitializeSystem()` 用于初始化 SDK 的全局运行环境(底层 VDEC/VENC/IVPS/AXCL RT 等模块)。

## 关键类型

- `axvsdk::common::SystemOptions`
- `axvsdk::common::BackendType`
- `axvsdk::common::VdecModule`
- `axvsdk::common::VencType`

## 典型用法

```cpp
#include "common/ax_system.h"

axvsdk::common::SystemOptions opt{};
opt.device_id = -1;
opt.enable_vdec = true;
opt.enable_venc = true;
opt.enable_ivps = true;

if (!axvsdk::common::InitializeSystem(opt)) {
  // 失败通常意味着驱动/库版本不匹配或模块初始化参数不合法。
}
```

## 语义说明

- 必须初始化:
  - 成功初始化后才能创建 `VideoDecoder` / `VideoEncoder` / `Pipeline` / `ImageProcessor` / `AxDrawer` 等对象。
- 可重复调用:
  - `InitializeSystem()` 重复调用会复用既有状态，库不鼓励用不同参数重复初始化。
- 反初始化:
  - 进程结束前调用 `ShutdownSystem()` 释放资源。
- 后端选择:
  - `BackendType::kAuto`: 按构建目标自动选择。
  - `BackendType::kAxMsp`: 板端 MSP 后端。
  - `BackendType::kAxcl`: AXCL 算力卡后端。
- 设备选择:
  - MSP 板端一般保持 `device_id = -1`。
  - AXCL 建议显式指定 `device_id` 作为当前进程默认卡；未显式指定的模块会使用 `GetActiveDeviceId()`。
- 模块开关:
  - 只开启当前进程需要的模块可以减少启动耗时和资源占用(例如只做 JPEG 编解码可关闭 video vdec)。

