# 图像类型 (`ax_image.h`)

头文件:

- `include/common/ax_image.h`

`axvsdk::common::AxImage` 是 SDK 内部编解码/图像处理/OSD 的统一图像载体，承载:

- 像素格式(`PixelFormat`)
- 宽高与 stride(`ImageDescriptor`)
- 底层内存来源(`MemoryType`)
- 物理/虚拟地址与(可选) block id

## 像素格式

`PixelFormat` 当前公开支持:

- `kNv12`: 硬件编解码原生输入/输出主格式(2 plane)。
- `kRgb24` / `kBgr24`: 常用于图像处理、frame output、JPEG 输入输出等(1 plane)。

## 内存类型

`MemoryType`:

- `kAuto`: 由库按平台策略选择，当前通常等价于 `kCmm`。
- `kCmm`: AX CMM 分配，可用于硬件模块直连，通常是默认首选。
- `kPool`: 来自 AX pool(常用于解码输出等硬件帧)。
- `kExternal`: 外部内存，库不拥有底层内存生命周期。

`CacheMode`:

- `kNonCached`: 适合大多数硬件直连场景。
- `kCached`: 适合 CPU 频繁访问；需要调用方配合 `FlushCache()/InvalidateCache()`。

## 创建与包装外部内存

### 创建新图像(库分配)

```cpp
using axvsdk::common::AxImage;
using axvsdk::common::PixelFormat;

auto img = AxImage::Create(PixelFormat::kNv12, 1920, 1080);
```

或用 `ImageDescriptor` 显式指定 stride(未填 stride 时由库推导对齐):

```cpp
axvsdk::common::ImageDescriptor d{};
d.format = PixelFormat::kBgr24;
d.width = 640;
d.height = 640;
// d.strides[0] = 0;  // 0 表示自动推导

auto img = AxImage::Create(d);
```

### 包装外部内存(库不释放)

```cpp
axvsdk::common::ImageDescriptor d{};
d.format = PixelFormat::kNv12;
d.width = 1920;
d.height = 1080;

std::array<axvsdk::common::ExternalImagePlane, axvsdk::common::kMaxImagePlanes> planes{};
planes[0].virtual_address = y_ptr;
planes[1].virtual_address = uv_ptr;

auto holder = std::shared_ptr<void>(ptr_owner, [](void*) { /* free/close */ });
auto img = AxImage::WrapExternal(d, planes, holder);
```

外部 plane 可选提供 `physical_address`/`block_id`，库在某些平台可走更快的硬件 copy。

## 访问地址与 cache

- `virtual_address(plane)`: 当前 plane 的 CPU 可访问地址。
- `physical_address(plane)`: 硬件侧物理地址(用于硬件模块直连)。

注意:

- AXCL 设备侧图像的 `virtual_address()` 不一定能被 host 直接访问。
  - 若需要 host 读写，应显式拷贝到 host 可访问内存(具体通过上层模块或 copy 逻辑完成)。
- cached 内存需要配合:
  - CPU 写完后送硬件: `FlushCache()`
  - 硬件写完后 CPU 读: `InvalidateCache()`

