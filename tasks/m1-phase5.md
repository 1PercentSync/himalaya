# M1 阶段五：屏幕空间效果 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：Framework 基础设施

- [ ] RG temporal 机制（double buffer、swap、resize 重建、history 无效标记）
- [ ] Per-frame-in-flight Set 2（DescriptorManager 2 份 Set 2、get_set2(frame_index)、所有 update/bind 路径适配）
- [ ] CommandBuffer push descriptor helpers（push_storage_image、push_sampled_image）
- [ ] Compute pipeline 基础设施（ComputePipelineDesc、create_compute_pipeline、Set 3 push descriptor layout）

## Step 2：Phase 5 Config + 资源装配

- [ ] RenderFeatures 扩展（ao、contact_shadows）+ feature_flags（FEATURE_AO、FEATURE_CONTACT_SHADOWS）+ bindings.glsl 同步
- [ ] AOConfig + ContactShadowConfig 结构体 + Application 初始化
- [ ] GlobalUBO 新增 inv_projection + prev_view_projection + bindings.glsl 同步
- [ ] Resolved depth 标记 temporal + FrameContext 新增 depth_prev
- [ ] DepthPrePass roughness 输出（R8 managed image + MSAA resolve + shader 修改）
- [ ] 创建 managed images（ao_noisy RG8、ao_filtered RG8 temporal、contact_shadow_mask R8）+ FrameContext 新增 RGResourceId
- [ ] Set 2 bindings 1-4 写入 + per-frame 更新逻辑
- [ ] DebugUI Features 面板（AO + Contact Shadows checkbox）+ AO/CS 面板骨架

## Step 3：GTAO Pass

- [ ] gtao.comp（horizon search + cosine-weighted 积分 + specular occlusion 直接计算 + per-pixel noise 旋转）
- [ ] GTAOPass 类（setup / record / destroy / rebuild_pipelines）
- [ ] DebugUI AO 面板参数滑条（radius、directions、steps per direction、bias、intensity）

## Step 4：AO Temporal Filter

- [ ] Renderer prev_view_projection 追踪
- [ ] ao_temporal.comp（reprojection + 三层 rejection + temporal blend + RG8 双通道处理）
- [ ] AOTemporalPass 类（setup / record / destroy / rebuild_pipelines）

## Step 5：AO Forward 集成

- [ ] forward.frag AO 采样（Set 2 binding 3）+ diffuse AO 乘法复合 + multi-bounce + specular occlusion
- [ ] forward.frag screen_uv 计算 + FEATURE_AO 守护
- [ ] Debug render mode 新增 DEBUG_MODE_AO_SSAO + 现有 DEBUG_MODE_AO 改为复合结果
- [ ] DebugUI AO 面板补充 temporal blend 滑条

## Step 6：Contact Shadows

- [ ] contact_shadows.comp（screen-space ray march + 深度自适应 thickness + 距离衰减）
- [ ] ContactShadowsPass 类（setup / record / destroy / rebuild_pipelines）
- [ ] forward.frag contact shadow 采样（Set 2 binding 4）+ shadow attenuation 叠加 + FEATURE_CONTACT_SHADOWS 守护
- [ ] DebugUI Contact Shadows 面板（step count、max distance、base thickness）
