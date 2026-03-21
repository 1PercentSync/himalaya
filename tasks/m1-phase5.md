# M1 阶段五：屏幕空间效果 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：RG Temporal 机制

- [x] create_managed_image 新增 temporal 参数 + 内部分配第二张 backing image
- [x] clear() swap current/history + resize 重建两张 + history 无效标记
- [x] get_history_image() 始终返回 valid + is_history_valid() 查询有效性
- [x] use_managed_image() 对 temporal image：current UNDEFINED + final_layout SHADER_READ_ONLY_OPTIMAL，history SHADER_READ_ONLY_OPTIMAL（首帧 UNDEFINED）

## Step 2：Per-frame Set 2

- [x] DescriptorManager：Set 2 pool 分配 2 份 set、get_set2(frame_index)
- [x] update_render_target 双份写入 + per-frame 重载
- [x] 所有现有 get_set2() 调用点适配 frame_index
- [x] 所有现有 update_render_target() 调用点确认走双份更新路径

## Step 3：Compute Pass Helpers

- [x] CommandBuffer push_storage_image（显式传 ResourceManager&）
- [x] CommandBuffer push_sampled_image（显式传 ResourceManager&）
- [x] DescriptorManager get_compute_set_layouts(set3_push_layout)

## Step 4：Phase 5 Config + GlobalUBO 扩展

- [x] RenderFeatures 新增 ao + contact_shadows + Application 初始化
- [x] FEATURE_AO + FEATURE_CONTACT_SHADOWS（bindings.glsl）+ feature_flags 填充逻辑
- [x] AOConfig + ContactShadowConfig 结构体 + Application 初始化
- [x] GlobalUBO 新增 inv_projection + prev_view_projection + bindings.glsl 同步 + Renderer 填充 + 帧末缓存 prev_view_projection
- [x] RenderInput + FrameContext 新增 ao_config + contact_shadow_config 指针

## Step 5：Depth/Normal Set 2 绑定 + AO 资源创建

- [x] Resolved depth 标记 temporal + FrameContext 新增 depth_prev
- [x] Set 2 binding 1（depth_resolved nearest）+ binding 2（normal_resolved nearest）写入双份 Set 2
- [x] Temporal binding 1（depth_resolved）每帧更新逻辑
- [x] 创建 managed images（ao_noisy RG8、ao_filtered RG8 temporal、contact_shadow_mask R8）+ FrameContext 新增 RGResourceId
- [x] Set 2 binding 3（ao_texture linear）+ binding 4（contact_shadow_mask linear）写入双份 Set 2
- [x] Temporal binding 3（ao_filtered）每帧更新逻辑

## Step 6：DebugUI 骨架

- [x] Features 面板新增 AO + Contact Shadows checkbox
- [x] AO 面板骨架（参数占位）
- [x] Contact Shadows 面板骨架（参数占位）

## Step 7：GTAO Pass（R8 diffuse AO）

- [x] gtao.comp：view-space position 重建 + normal 转换
- [x] gtao.comp：horizon search + cosine-weighted 积分 + per-pixel noise 旋转
- [x] gtao.comp：输出 RG8（R=AO, G=0.0）+ bias/intensity 应用
- [ ] GTAOPass 类（setup / record / destroy / rebuild_pipelines）+ Set 3 layout + pipeline
- [ ] GTAOPass::record()：push constants + push descriptors
- [ ] DebugUI AO 面板填充（radius、directions、steps per direction、bias、intensity）

## Step 8：AO Temporal Filter（R8 单通道）

- [ ] ao_temporal.comp：reprojection（inv_view_projection → world → prev_view_projection → prev UV）
- [ ] ao_temporal.comp：三层 rejection + R 通道处理 + G 通道 passthrough
- [ ] ao_temporal.comp：temporal blend + history 无效处理
- [ ] AOTemporalPass 类（setup / record / destroy / rebuild_pipelines）+ push descriptors

## Step 9：AO Forward 集成（近似 SO）

- [ ] forward.frag：screen_uv 计算 + rt_ao_texture 采样（R 通道）
- [ ] forward.frag：diffuse AO 乘法复合 + multi-bounce 色彩补偿
- [ ] forward.frag：Lagarde 近似 SO + IBL specular 调制
- [ ] forward.frag：FEATURE_AO 守护
- [ ] Renderer 编排：features.ao 条件调用 + FrameContext 条件资源声明

## Step 10：AO Debug Modes + Temporal UI

- [ ] Debug render mode 新增 DEBUG_MODE_AO_SSAO + DEBUG_MODE_AO 改为复合结果
- [ ] DebugUI AO 面板补充 temporal blend 滑条

## Step 11：Roughness Buffer

- [ ] R8 roughness managed image 创建 + on_sample_count_changed 适配
- [ ] depth_prepass.frag + depth_prepass_masked.frag：输出 roughness
- [ ] DepthPrePass：roughness 额外 color attachment + MSAA AVERAGE resolve
- [ ] FrameContext 新增 roughness RGResourceId

## Step 12：GTAO SO 升级（B1）

- [ ] GTAO Set 3 push descriptor 新增 roughness binding
- [ ] gtao.comp：读 roughness + 重建 view/reflection direction
- [ ] gtao.comp：per-direction specular cone vs horizon overlap → G 通道输出
- [ ] ao_temporal.comp：RG8 双通道适配（SO 通道同 blend factor、不做邻域 clamp）
- [ ] forward.frag：SO 改读 G 通道，删除 Lagarde 近似

## Step 13：Contact Shadows Compute

- [ ] contact_shadows.comp：screen-space ray march + UV 步进
- [ ] contact_shadows.comp：深度比较 + 深度自适应 thickness
- [ ] contact_shadows.comp：距离衰减 + push constants
- [ ] ContactShadowsPass 类（setup / record / destroy / rebuild_pipelines）+ push descriptors

## Step 14：Contact Shadows Forward 集成

- [ ] forward.frag：contact shadow 采样 + shadow attenuation 叠加
- [ ] FEATURE_CONTACT_SHADOWS 守护
- [ ] Renderer 编排：features.contact_shadows 条件调用
- [ ] DebugUI Contact Shadows 面板填充（step count、max distance、base thickness）
