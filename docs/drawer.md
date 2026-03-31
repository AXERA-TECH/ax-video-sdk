# 绘图/OSD (`ax_drawer.h`)

头文件:

- `include/common/ax_drawer.h`

`AxDrawer` 用于对 `AxImage` 绘制常见 OSD 元素，并可结合 `Pipeline::SetOsd()` 进行异步叠加。

## 关键类型

- `DrawFrame`: 一次提交的一组 OSD 命令(建议一次把需要同时绘制的内容都放进去)。
  - `hold_frames`: OSD 保留帧数
    - `1`: 只作用于接下来的一帧
    - `0`: 持续生效，直到被覆盖或 `ClearOsd()`
- `DrawLine` / `DrawPolygon` / `DrawRect` / `DrawMosaic` / `DrawBitmap`

颜色统一按 `0xRRGGBB`，并支持 `alpha`。

## 典型用法

```cpp
auto drawer = axvsdk::common::CreateDrawer();

axvsdk::common::DrawFrame f{};
f.hold_frames = 1;
f.rects.push_back({.x = 10, .y = 10, .width = 200, .height = 120, .color = 0x00FF00});

drawer->Draw(f, *image);
```

结合 pipeline 异步 OSD:

```cpp
pipe->SetOsd(f);  // 不阻塞 demux/dec/enc 主链路
```

## PreparedDrawCommands

`Prepare()` 可将 `DrawFrame` 编译为内部结构(例如 bitmap 预处理)，用于复用:

```cpp
auto prepared = drawer->Prepare(f);
prepared->Apply(*image);
```

## 平台差异

- 具体可绘制类型由后端实现决定。
- 当前版本在 AXCL 后端下通常只保证 `rect` 可用，其它类型可能被拒绝或不稳定。

