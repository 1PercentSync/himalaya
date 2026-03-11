# Milestone 1 阶段三：任务清单

> 实现步骤见 `docs/current-phase.md`，接口定义见 `docs/milestone-1/m1-interfaces.md`

---

## Step 1：Renderer 提取

- [ ] 创建 `RenderInput` 结构体（`app/include/himalaya/app/renderer.h`）
- [ ] 创建 `Renderer` 类骨架（init / render / destroy / on_swapchain_invalidated / on_swapchain_recreated）
- [ ] 将渲染资源所有权从 Application 迁移到 Renderer（pipeline、depth image、default textures、default sampler、shader compiler、material system、render graph、swapchain image handles、per-frame UBO/SSBO）
- [ ] 将渲染逻辑从 Application 迁移到 Renderer（RG 构建与执行、UBO/SSBO 填充、draw loop）
- [ ] Application 改为持有 Renderer 并在主循环中调用 `renderer_.render(cmd, input)`
- [ ] 两阶段 resize：`handle_resize()` 改为先调 `renderer_.on_swapchain_invalidated()`、再 `swapchain_.recreate()`、最后 `renderer_.on_swapchain_recreated()`
- [ ] 验证：编译通过，运行效果与阶段二一致，无 validation 报错
