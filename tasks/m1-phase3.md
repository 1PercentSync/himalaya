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

## Step 2：RG Managed 资源

- [ ] `RGImageDesc` 结构体（RGSizeMode Relative/Absolute、format、usage、sample_count、mip_levels）
- [ ] `RGManagedHandle` 类型 + `create_managed_image()` / `destroy_managed_image()` API
- [ ] `use_managed_image()` — 每帧调用，返回 `RGResourceId`（RG 内部推导 initial/final layout）
- [ ] `set_reference_resolution(VkExtent2D)` — Relative 模式的基准分辨率
- [ ] Resize 自动重建：`set_reference_resolution()` 被调用时，比较 desc 推导出的新旧尺寸，变化时销毁旧 backing image 并创建新的
- [ ] `update_managed_desc(handle, new_desc)` — 更新描述符（MSAA 切换用），desc 变化时重建 backing image
- [ ] 迁移现有 depth buffer 从手动管理到 managed 资源，删除 Renderer 中的手动 depth 创建/销毁代码
- [ ] 验证：现有渲染正常工作，depth buffer 由 RG managed 管理，resize 时自动重建
