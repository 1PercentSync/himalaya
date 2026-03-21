# 当前阶段：M1 阶段五 — 屏幕空间效果

> 目标：在阶段四的阴影基础上，实现 AO（GTAO 算法）+ temporal filtering + Contact Shadows，为场景增加空间层次感和接地感。
> 同时搭建 temporal filtering 基础设施（RG temporal、per-frame Set 2、compute pass push descriptors），为 M2 的 SSR/SSGI 奠定基础。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

---

## 实现步骤

### 依赖关系

```
Step 1: Framework 基础设施
    ↓
Step 2: Phase 5 Config + 资源装配
    ↓
Step 3: GTAO Pass
    ↓
Step 4: AO Temporal Filter
    ↓
Step 5: AO Forward 集成
    ↓
Step 6: Contact Shadows
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | Framework 基础设施 | 现有渲染输出不变，无 validation 报错 |
| 2 | Phase 5 Config + 资源装配 | 无 validation 报错，DebugUI 面板可见，checkbox 可切换 |
| 3 | GTAO Pass | RenderDoc 检查 ao_noisy 纹理——角落/缝隙暗、开阔平面 ~1.0 |
| 4 | AO Temporal Filter | ao_filtered 比 ao_noisy 明显更平滑；相机移动时无可见 ghosting |
| 5 | AO Forward 集成 | 场景中角落/物体接缝处有自然暗部，AO toggle on/off 对比明显 |
| 6 | Contact Shadows | 物体接地处有细致接触阴影，Contact Shadows toggle on/off 对比明显 |

---

### Step 1：Framework 基础设施

纯机制变更，不改变现有渲染结果。

- [ ] RG temporal 机制：`create_managed_image(temporal=true)` + `get_history_image()`，内部 double buffer，`clear()` 时 swap，resize 时重建两张并标记 history 无效
- [ ] Per-frame-in-flight Set 2：DescriptorManager 分配 2 份 Set 2，`get_set2(frame_index)` 替代 `get_set2()`，所有 Set 2 update/bind 路径适配
- [ ] CommandBuffer push descriptor helpers：`push_storage_image(layout, set, binding, image)` + `push_sampled_image(layout, set, binding, image, sampler)`
- [ ] Compute pipeline 基础设施：`ComputePipelineDesc` + `create_compute_pipeline()`，pipeline layout 支持 Set 3 push descriptor layout

**验证**：现有渲染输出不变，无 validation 报错

#### 设计要点

RG temporal 见 `milestone-1/m1-design-decisions.md`「Temporal 基础设施」。Per-frame Set 2 见「Per-frame-in-flight Set 2」。Push descriptors 见「Compute Pass 绑定机制」。

关键设计：
- RG temporal 内部为 managed image 分配第二张 backing image（`history_backing`），`clear()` 时 swap current/history，resize 时重建两张并标记 history 无效
- Per-frame Set 2 与 Set 0 统一模式：2 份 descriptor set 对应 2 frames in flight，每帧绑定当前帧的 copy
- Push descriptor helpers 封装 `VkWriteDescriptorSet` 构造，Pass 层不接触 Vulkan 类型
- Compute pipeline 使用 4 组 set layout（Set 0-2 与 graphics 共享，Set 3 为 push descriptor），每个 compute pass 定义自己的 Set 3 layout

---

### Step 2：Phase 5 Config + 资源装配

声明配置结构、创建资源、连接 descriptor。

- [ ] RenderFeatures 扩展 `ao` + `contact_shadows`，feature_flags 扩展 `FEATURE_AO` + `FEATURE_CONTACT_SHADOWS`（bindings.glsl 同步）
- [ ] `AOConfig` + `ContactShadowConfig` 结构体（scene_data.h），Application 初始化默认值
- [ ] GlobalUBO 新增 `inv_projection` + `prev_view_projection`（bindings.glsl + scene_data.h 同步）
- [ ] Resolved depth 标记 temporal，FrameContext 新增 `depth_prev`
- [ ] DepthPrePass 新增 R8 roughness 输出（managed image + MSAA resolve），GTAO 需要 per-pixel roughness 计算 specular occlusion
- [ ] 创建 managed images：`ao_noisy`（RG8 普通）、`ao_filtered`（RG8 temporal）、`contact_shadow_mask`（R8 普通）；FrameContext 新增对应 RGResourceId
- [ ] Set 2 bindings 1-4 写入（depth_resolved nearest、normal_resolved nearest、ao_texture linear、contact_shadow_mask linear），per-frame Set 2 对 temporal binding 每帧更新
- [ ] DebugUI：Features 面板增加 AO + Contact Shadows checkbox，AO 面板骨架，Contact Shadows 面板骨架

**验证**：无 validation 报错，DebugUI 面板可见，checkbox 可切换

#### 设计要点

AO 命名规范见 `milestone-1/m1-design-decisions.md`「AO 命名」。AOConfig / ContactShadowConfig 见 `milestone-1/m1-interfaces.md`。

关键设计：
- Feature 层用 `ao`（RenderFeatures、feature_flags、DebugUI、config 结构体），Implementation 层用 `gtao`（shader 文件名、pass 类名）
- GTAO 输出 RG8：R=diffuse AO, G=specular occlusion（GTAO 直接计算标量 SO，利用 per-direction horizon angles + roughness buffer + reflection direction）
- Resolved depth 标记 temporal 后，RG 自动管理 double buffer，`get_history_image()` 返回上一帧深度，零额外 copy 开销
- DepthPrePass roughness 输出：DepthPrePass 已读材质数据（alpha test），roughness 从同一材质取出，shader 改动很小。M2 SSR 也需要 roughness buffer

---

### Step 3：GTAO Pass

- [ ] `gtao.comp`：GTAO 实现（view-space 位置重建 via inv_projection，horizon search N 方向 × M 步，cosine-weighted 解析积分，specular occlusion 直接计算（读 roughness buffer + 重建 reflection direction），interleaved gradient noise per-pixel 方向旋转），输出 RG8（R=AO, G=SO）
- [ ] `GTAOPass` 类（setup / record / destroy / rebuild_pipelines），push constants 传 AO 参数（radius、directions、steps_per_direction、bias、intensity），push descriptors 绑 ao_noisy storage output + roughness 输入 + depth/normal 输入（Set 2 已有）
- [ ] DebugUI AO 面板：radius、directions、steps per direction、bias、intensity 滑条

**验证**：RenderDoc 检查 ao_noisy 纹理——R 通道角落/缝隙暗、开阔平面 ~1.0；G 通道光滑表面在遮蔽区 SO 低、开阔区 SO 高

#### 设计要点

GTAO 算法选择见 `milestone-1/m1-design-decisions.md`「AO 算法选择」。Specular Occlusion 方案见「Specular Occlusion」。

关键设计：
- GTAO 在 view-space 工作：`inv_projection` 从 depth 重建 view-space position，normal buffer 的 world-space 法线通过 `mat3(view)` 转换为 view-space
- Specular Occlusion 在 GTAO shader 中直接计算（方案 B1）：读 roughness buffer 获取实际 roughness，重建 view direction + reflection direction，每个 horizon search 方向直接评估 specular cone 与 horizon 的重叠。精度高于 bent normal 中间表示（方案 A），且 AO 纹理仅 RG8（vs RGBA8）
- Poisson disk 或均匀角度分布的搜索方向，per-pixel interleaved gradient noise 旋转消除 banding
- Push constants 传 AO 参数（仅 gtao.comp 消费，不放 GlobalUBO）

---

### Step 4：AO Temporal Filter

- [ ] Renderer 每帧追踪并填充 `prev_view_projection`（上一帧末缓存当前帧 VP）
- [ ] `ao_temporal.comp`：reprojection（inv_view_projection → world → prev_view_projection → prev UV），三层 rejection（UV 有效性 + prev depth 深度一致性 + 邻域 clamp），temporal blend，RG8 双通道处理（AO 标量 clamp + blend，SO 标量同 blend factor 处理）
- [ ] `AOTemporalPass` 类（setup / record / destroy / rebuild_pipelines），push descriptors 绑 ao_noisy 输入 + ao_history 读 + ao_filtered 输出 + depth_prev 读，push constants 传 temporal 参数

**验证**：RenderDoc 对比 ao_noisy vs ao_filtered——filtered 明显更平滑；相机移动时无可见 ghosting

#### 设计要点

Temporal 基础设施见 `milestone-1/m1-design-decisions.md`「Temporal 基础设施」。Rejection 策略见「Temporal Rejection」。Prev depth 见「Prev Depth」。

关键设计：
- 三层 rejection：(1) UV 有效性（prev_uv 越界 → reject）(2) prev depth 一致性（当前世界坐标在上一帧的预期深度 vs 实际存储深度）(3) 邻域 clamp（AO 通道 3×3 min/max clamp）
- SO 通道不做邻域 clamp（方向量的 min/max 无物理意义），使用与 AO 相同的 blend factor
- Prev depth 来自 resolved depth 的 temporal history（`get_history_image(depth_handle)`），零额外 copy
- 首帧 / resize 后 / history 无效时 blend_factor = 0（纯用当前帧）

---

### Step 5：AO Forward 集成

- [ ] `forward.frag` 采样 `rt_ao_texture`（Set 2 binding 3），读取 R=AO, G=SO
- [ ] Diffuse AO：`ssao × material_ao` 乘法复合 + Jimenez 2016 multi-bounce 色彩补偿
- [ ] Specular Occlusion：直接读 G 通道作为 `spec_oc`，调制 IBL specular
- [ ] `FEATURE_AO` feature_flags 守护（禁用时 AO=1.0, SO=1.0）
- [ ] `screen_uv = gl_FragCoord.xy / global.screen_size`
- [ ] Debug render mode 新增 `DEBUG_MODE_AO_SSAO`（显示 GTAO 的 AO 通道），追加到 passthrough 模式末尾；现有 `DEBUG_MODE_AO` 改为显示 `ssao × material_ao` 复合结果
- [ ] DebugUI AO 面板补充 temporal blend 滑条

**验证**：场景中角落/物体接缝处有自然暗部，AO toggle on/off 对比明显，光滑金属在角落处 specular 适当衰减，AO debug mode 正确

#### 设计要点

AO 光照集成见 `milestone-1/m1-design-decisions.md`「AO 光照集成」。

关键设计：
- Diffuse AO 使用 multi-bounce 色彩补偿（Jimenez 2016）：浅色表面（高 albedo）AO 压暗减轻，避免白墙角落过度黑暗
- Specular Occlusion 直接从 AO 纹理 G 通道读取（GTAO 在 compute shader 中已用实际 roughness + reflection direction 计算），forward.frag 无需额外近似公式
- 仅调制间接光（IBL diffuse + IBL specular），直接光已有 shadow map + contact shadows 覆盖
- `DEBUG_MODE_AO`（原有 index 7）改为显示复合 AO（ssao × material_ao），新增 `DEBUG_MODE_AO_SSAO` 显示纯 GTAO 输出，方便分别调试

---

### Step 6：Contact Shadows

- [ ] `contact_shadows.comp`：screen-space ray march（世界空间起点 + 光方向 × 世界空间最大距离 → 投影到屏幕空间 → UV 步进 → 每步深度比较 + 深度自适应 thickness），距离衰减输出（首次命中 + 远端 smoothstep fade），push constants 传参数（光方向、max_distance、base_thickness、step_count），push descriptors 绑 depth 读 + contact_shadow_mask 写
- [ ] `ContactShadowsPass` 类（setup / record / destroy / rebuild_pipelines）
- [ ] `forward.frag` 采样 `rt_contact_shadow_mask`（Set 2 binding 4），乘到直接光 shadow attenuation 上，`FEATURE_CONTACT_SHADOWS` feature_flags 守护
- [ ] DebugUI Contact Shadows 面板：step count（8/16/24/32）、max distance 滑条、base thickness 滑条

**验证**：物体接地处有细致接触阴影，Contact Shadows toggle on/off 对比明显，远端边缘柔和无硬切线

#### 设计要点

Contact Shadows 设计见 `milestone-1/m1-design-decisions.md`「Contact Shadows」。

关键设计：
- Screen-space ray march：光方向投影到屏幕空间后在 UV 上均匀步进，每步线性插值深度（短 ray <0.5m 误差 <3%，可忽略）
- 搜索距离使用世界空间（物理意义明确），shader 内部投影到屏幕空间确定步进范围
- 深度自适应 thickness：`base_thickness × linear_depth`，远处更宽容（深度精度低 + 物体屏幕尺寸小）
- 距离衰减（方案 C）：首次命中 → 按 ray 上的位置 `smoothstep` 衰减。接触点全强度，搜索极限渐淡。物理合理（任何遮挡都完全遮光，衰减仅隐藏搜索距离边界 artifact）
- 无 temporal filter：确定性输出（无随机采样），步数不够则加步数
- Push constant 传光方向：shader 不假设光源类型，M2 多光源时每盏光独立 dispatch + 独立 R8 输出

---

## 阶段五帧流程

#### 阶段五结束状态帧流程

```
CSM Shadow Pass（单 RG pass，内部循环 cascade）
  输出: shadow_map (2D Array, D32Sfloat)
    ↓
DepthPrePass (MSAA)
  输出: msaa_depth, msaa_normal → resolve: depth, normal
  新增输出: roughness (R8, resolve if MSAA)
    ↓
GTAO Pass (compute)
  读: depth (Set 2 binding 1), normal (Set 2 binding 2), roughness (push descriptor)
  写: ao_noisy (RG8, push descriptor storage image)
    ↓
AO Temporal (compute)
  读: ao_noisy, ao_history, depth, depth_prev (push descriptors)
  写: ao_filtered (push descriptor storage image)
    ↓
Contact Shadows (compute)
  读: depth (Set 2 binding 1), light direction (push constant)
  写: contact_shadow_mask (R8, push descriptor storage image)
    ↓
ForwardPass (MSAA, depth EQUAL write OFF)
  读: shadow_map (Set 2 binding 5/6), ao_filtered (Set 2 binding 3),
       contact_shadow_mask (Set 2 binding 4)
  输出: msaa_color → resolve: hdr_color
    ↓
SkyboxPass → TonemappingPass → ImGuiPass → Present
```

---

## 阶段五文件清单

### 新增文件

```
passes/
├── include/himalaya/passes/
│   ├── gtao_pass.h              # [Step 3] GTAO compute pass
│   ├── ao_temporal_pass.h       # [Step 4] AO temporal filter compute pass
│   └── contact_shadows_pass.h   # [Step 6] Contact shadows compute pass
└── src/
    ├── gtao_pass.cpp            # [Step 3]
    ├── ao_temporal_pass.cpp     # [Step 4]
    └── contact_shadows_pass.cpp # [Step 6]
shaders/
├── gtao.comp                   # [Step 3] GTAO (horizon search + specular occlusion)
├── ao_temporal.comp            # [Step 4] AO temporal filter (reprojection + rejection + blend)
└── contact_shadows.comp        # [Step 6] Screen-space contact shadows
```

### 修改文件

```
rhi/
├── include/himalaya/rhi/
│   ├── commands.h              # [Step 1] push descriptor helpers
│   └── pipeline.h              # [Step 1] ComputePipelineDesc + create_compute_pipeline
├── src/
│   ├── commands.cpp            # [Step 1]
│   └── pipeline.cpp            # [Step 1]
framework/
├── include/himalaya/framework/
│   ├── render_graph.h          # [Step 1] temporal API
│   ├── scene_data.h            # [Step 2] AOConfig, ContactShadowConfig, RenderFeatures, GlobalUBO, feature_flags
│   └── frame_context.h         # [Step 2] 新增 RGResourceId
├── src/
│   └── render_graph.cpp        # [Step 1] temporal 实现
passes/
├── include/himalaya/passes/
│   └── depth_prepass.h         # [Step 2] roughness 输出
├── src/
│   └── depth_prepass.cpp       # [Step 2] roughness attachment + shader 修改
app/
├── include/himalaya/app/
│   ├── renderer.h              # [Step 2-5] RenderInput 扩展
│   └── debug_ui.h              # [Step 2] DebugUIContext 扩展
├── src/
│   ├── renderer.cpp            # [Step 2-6]
│   ├── application.cpp         # [Step 2] AOConfig + ContactShadowConfig 初始化 + RenderFeatures
│   └── debug_ui.cpp            # [Step 2-6] AO 面板 + Contact Shadows 面板
rhi/
├── include/himalaya/rhi/
│   └── descriptors.h           # [Step 1] get_set2(frame_index)
├── src/
│   └── descriptors.cpp         # [Step 1] per-frame Set 2
shaders/
├── common/bindings.glsl        # [Step 2] FEATURE_AO, FEATURE_CONTACT_SHADOWS, Set 2 binding 1-4, GlobalUBO 新增字段
├── forward.frag                # [Step 5-6] AO 采样 + contact shadow 采样
└── depth_prepass.frag          # [Step 2] roughness 输出
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| AO 命名 | Feature 层用 `ao`（RenderFeatures、feature_flags、DebugUI、config），Implementation 层用 `gtao`（shader、pass 类） |
| GTAO 算法 | 直接实现 GTAO（跳过 Crytek SSAO），结构化搜索 + 解析积分，原始噪声低于随机采样方案 |
| GTAO 输出 | RG8_UNORM：R=diffuse AO, G=specular occlusion（直接计算，非 bent normal 中间表示） |
| Specular Occlusion | 方案 B1：GTAO 读 roughness buffer + 重建 reflection direction，per-direction 直接评估 specular cone 与 horizon 重叠。精度最高且 AO 纹理仅 2 通道 |
| AO 集成 | Diffuse: `ssao × material_ao` + multi-bounce 色彩补偿 (Jimenez 2016)。Specular: GTAO 输出的 SO 标量直接调制 IBL specular。仅间接光 |
| Multi-bounce | `max(vec3(ao), ((ao * a + b) * ao + c) * ao)` 其中 a/b/c 由 albedo 决定。浅色表面压暗减轻 |
| 无 blur pass | GTAO + temporal 即可得到干净结果，不需要额外空间模糊 |
| RG temporal | RG managed image 标记 `temporal=true` → 内部 double buffer，`clear()` 自动 swap，resize 重建两张。API：`get_history_image()` 返回上一帧 RGResourceId |
| Per-frame Set 2 | Set 2 分配 2 份（对应 2 frames in flight），temporal binding 每帧更新当前帧的 copy，解决帧间竞争 |
| Compute pass 绑定 | Set 3 push descriptors。每个 compute pass 定义自己的 Set 3 layout + pipeline layout。Set 0/1/2 与 graphics 共享 |
| AO 参数传递 | Push constants（仅 compute pass 消费，不膨胀 GlobalUBO） |
| Prev depth | Resolved depth 标记 temporal → RG 自动管理 double buffer → `get_history_image()` 返回上一帧深度，零 copy 开销 |
| Temporal rejection | 三层：(1) UV 有效性 (2) prev depth 深度一致性 (3) 邻域 clamp（仅 AO 通道，SO 不 clamp） |
| Contact Shadows | Screen-space march + 世界空间距离 + 深度自适应 thickness + 距离衰减（方案 C：首次命中 + smoothstep fade）+ 无 temporal + push constant 传光方向 |
| Contact Shadows 多光源 | M1 一盏方向光。Push constant 传光方向使 shader 不假设光源类型，M2 per-light dispatch + per-light R8 输出 |
| AO 分辨率 | 全分辨率。GTAO 采样效率高（4方向×4步=16 fetch），M1 无 TAA 半分辨率瑕疵更明显 |
| Roughness buffer | DepthPrePass 新增 R8 输出。GTAO 计算 SO 用，M2 SSR 可复用 |
| GlobalUBO 增长 | 720 → ~848 bytes（+inv_projection 64 + prev_view_projection 64），仍远小于 16KB |
| DepthPrePass roughness | 已读材质数据（alpha test），roughness 从同一材质取出。MSAA 时 AVERAGE resolve（与 normal 一致） |
