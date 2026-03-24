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
- [x] GTAOPass 类（setup / record / destroy / rebuild_pipelines）+ Set 3 layout + pipeline
- [x] GTAOPass::record()：push constants + push descriptors
- [x] DebugUI AO 面板填充（radius、directions、steps per direction、bias、intensity）

## Step 8：AO Temporal Filter（R8 单通道）

- [x] ao_temporal.comp：reprojection（inv_view_projection → world → prev_view_projection → prev UV）
- [x] ao_temporal.comp：三层 rejection + R 通道处理 + G 通道 passthrough
- [x] ao_temporal.comp：temporal blend + history 无效处理
- [x] AOTemporalPass 类（setup / record / destroy / rebuild_pipelines）+ push descriptors

## Step 9：AO Forward 集成（近似 SO）

- [x] forward.frag：screen_uv 计算 + rt_ao_texture 采样（R 通道）
- [x] forward.frag：diffuse AO 乘法复合 + multi-bounce 色彩补偿
- [x] forward.frag：Lagarde 近似 SO + IBL specular 调制
- [x] forward.frag：FEATURE_AO 守护
- [x] Renderer 编排：features.ao 条件调用 + FrameContext 条件资源声明

## Step 10：AO Debug Modes + Temporal UI

- [x] Debug render mode 新增 DEBUG_MODE_AO_SSAO + DEBUG_MODE_AO 改为复合结果
- [x] DebugUI AO 面板补充 temporal blend 滑条

## Step 10a：GTAO 正确性修复

- [x] 重构 horizon search 流程：法线投影移到搜索循环之前（每 slice 先投影再搜索）
- [x] 修复 `n_proj_len` 缺失：slice 可见性乘以投影法线长度
- [x] Horizon 初始化改为切平面极限 `cos(γ ± π/2)`，替代固定 bias
- [x] Falloff 目标改为切平面极限（而非固定 bias），使衰减行为与视角无关
- [x] 添加 thickness heuristic：后续样本深度回退时衰减 horizon（EMA 或类似机制）
- [x] Push constants 新增 `frame_index`，IGN 噪声加入帧间变化

## Step 10b：GTAO 质量增强

- [x] 步进分布改为二次幂曲线（样本集中在像素附近）
- [x] 添加 R1 序列步进抖动（消除 banding）
- [x] Falloff 形状调整：flat inner + steep outer（XeGTAO 风格）
- [x] 新增 `ao_spatial.comp`：5×5 edge-aware bilateral blur
- [x] 新增 `AOSpatialPass` 类（setup / record / destroy / rebuild_pipelines）
- [x] Renderer 编排：GTAO → AO Spatial Blur → AO Temporal 管线串联
- [x] 新增 managed image `ao_blurred`（RG8）作为 spatial blur 输出

## Step 10c：HDR Sun 方向光 + 色温模式

- [x] `color_temperature_to_rgb()` 工具函数（framework 层 `color_utils.h` / `color_utils.cpp`）
- [x] IBL 类新增 `equirect_width()` / `equirect_height()` getter
- [x] `LightSourceMode` 枚举新增 `HdrSun`，DebugUI combo 适配四项（Scene / Fallback / HDR Sun / None）
- [x] `AppConfig` 新增 `hdr_sun_coords` 持久化（HDR 路径 → 像素坐标映射）+ 序列化/反序列化
- [x] Application HDR Sun 成员 + `update()` 中 equirectangular 坐标 → 方向转换 + IBL 旋转耦合
- [x] Fallback 模式新增色温支持（`fallback_light_color_temp_` + `color_temperature_to_rgb` 应用）
- [x] HDR Sun 模式新增色温支持（`hdr_sun_color_temp_` + `color_temperature_to_rgb` 应用）
- [x] DebugUI：HDR Sun 控件（Sun X/Y 输入、Intensity、Color Temp、Cast Shadows）+ Fallback 新增 Color Temp slider

## Step 11：Roughness Buffer

- [x] R8 roughness managed image 创建 + on_sample_count_changed 适配
- [x] depth_prepass.frag + depth_prepass_masked.frag：输出 roughness
- [x] DepthPrePass：roughness 额外 color attachment + MSAA AVERAGE resolve
- [x] FrameContext 新增 roughness + msaa_roughness RGResourceId

## Step 12：Bent Normal + GTSO

- [x] ao_noisy / ao_blurred / ao_filtered managed image 格式 RG8 → RGBA8 + Set 3 layout 更新
- [ ] gtao.comp：新增 bent normal 计算（Algorithm 2）+ 输出 RGBA8
- [ ] ao_spatial.comp：适配 RGBA8 四通道降噪
- [ ] ao_temporal.comp：适配 RGBA8（AO 邻域 clamp 改 A 通道，bent normal RGB blend 不 clamp）
- [ ] forward.frag：SO 改用 GTSO 解析公式（bent normal cone 交集），删除 Lagarde 近似

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
