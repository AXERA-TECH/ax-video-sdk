# PipelineManager (`ax_pipeline_manager.h`)

头文件:

- `include/pipeline/ax_pipeline_manager.h`

`PipelineManager` 用于在单进程内管理多个 `Pipeline` 的生命周期:

- 按 name 增删 pipeline
- 单独启动/停止某一路
- 启动/停止全部
- 查询运行状态与统计

## 典型用法

```cpp
auto mgr = axvsdk::pipeline::CreatePipelineManager();
mgr->AddPipeline("p0", cfg0);
mgr->AddPipeline("p1", cfg1);
mgr->StartAll();

// ...

mgr->StopAll();
mgr->Clear();
```

## 查询

- `ListPipelines()` 返回 `ManagedPipelineInfo` 列表(name/running/stats)。

