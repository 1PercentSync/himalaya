# 当前阶段：M1 阶段五 — 屏幕空间效果

> 目标：在阶段四的阴影基础上，实现 AO（GTAO 算法）+ temporal filtering + Contact Shadows，为场景增加空间层次感和接地感。
> 同时搭建 temporal filtering 基础设施（RG temporal、per-frame Set 2、compute pass push descriptors），为 M2 的 SSR/SSGI 奠定基础。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

---

## 实现步骤

### 依赖关系

```
Steps 1-3: 基础设施（独立，无渲染变化）
    ↓
Steps 4-6: Config + 资源 + DebugUI 骨架
    ↓
Step 7: GTAO（R8 diffuse AO）
    ↓
Step 8: AO Temporal（R8）
    ↓
Step 9: AO Forward 集成（近似 SO）
    ↓
Step 10: AO Debug Modes + Temporal UI
    ↓ (完整 AO 管线，近似 SO 基线)
Step 11: Roughness Buffer
    ↓
Step 12: GTAO SO 升级（B1）
    ↓
Step 13: Contact Shadows Compute
    ↓
Step 14: Contact Shadows Forward 集成
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | RG Temporal 机制 | 现有渲染输出不变，无 validation 报错 |
| 2 | Per-frame Set 2 | 现有渲染输出不变，无 validation 报错 |
| 3 | Compute Pass Helpers | 编译通过，现有渲染不变 |
| 4 | Phase 5 Config + GlobalUBO | 编译通过，现有渲染不变 |
| 5 | Depth/Normal Set 2 绑定 + AO 资源 | 无 validation 报错，RenderDoc 可见新资源 |
| 6 | DebugUI 骨架 | 面板可见，checkbox 可切换 |
| 7 | GTAO Pass（R8） | RenderDoc: ao_noisy R 通道角落暗、开阔 ~1.0 |
| 8 | AO Temporal Filter（R8） | ao_filtered 比 ao_noisy 更平滑，无 ghosting |
| 9 | AO Forward 集成（近似 SO） | 角落暗部可见，AO toggle 对比明显 |
| 10 | AO Debug Modes + Temporal UI | debug modes 正确，temporal blend 滑条生效 |
| 11 | Roughness Buffer | RenderDoc: roughness 值与材质一致 |
| 12 | GTAO SO 升级（B1） | 对比 Step 9 基线，SO 更精确 |
| 13 | Contact Shadows Compute | RenderDoc: contact_shadow_mask 接地处有遮罩 |
| 14 | Contact Shadows Forward 集成 | 接触阴影可见，toggle 对比明显 |

---

### Step 1：RG Temporal 机制

纯 RG 内部变更，不影响现有渲染。

- [ ] `create_managed_image` 新增 `temporal` 参数，内部分配第二张 backing image（`history_backing`）
- [ ] `clear()` 时 swap current/history backing，resize 时重建两张并标记 history 无效
- [ ] `get_history_image(handle)` 返回 history 的 RGResourceId（首帧/resize 后返回 invalid）
- [ ] `use_managed_image()` 对 temporal image：current 以 UNDEFINED 导入，history 以 SHADER_READ_ONLY_OPTIMAL 导入

**验证**：现有渲染输出不变，无 validation 报错

#### 设计要点

RG temporal 见 `milestone-1/m1-design-decisions.md`「Temporal 基础设施」。

关键设计：
- RG temporal 内部为 managed image 分配第二张 backing image（`history_backing`），`clear()` 时 swap current/history，resize 时重建两张并标记 history 无效
- `use_managed_image()` 返回当前帧写入目标（UNDEFINED），`get_history_image()` 返回上一帧保留内容（SHADER_READ_ONLY_OPTIMAL）

---

### Step 2：Per-frame Set 2

DescriptorManager 变更 + 所有调用点适配。

- [ ] DescriptorManager：Set 2 pool 分配 2 份 set，`get_set2()` → `get_set2(uint32_t frame_index)`
- [ ] `update_render_target(binding, image, sampler)` 保留（内部写两份），新增 `update_render_target(frame_index, binding, image, sampler)`（写指定帧）
- [ ] 所有现有 `get_set2()` 调用点（Renderer bind 路径）适配 `get_set2(frame_index)`
- [ ] 所有现有 `update_render_target()` 调用点（init / resize / MSAA 切换）确认走双份更新路径

**验证**：现有渲染输出不变，无 validation 报错

#### 设计要点

Per-frame Set 2 见 `milestone-1/m1-design-decisions.md`「Per-frame-in-flight Set 2」。

关键设计：
- Per-frame Set 2 与 Set 0 统一模式：2 份 descriptor set 对应 2 frames in flight，每帧绑定当前帧的 copy
- Temporal binding 每帧更新当前帧 copy；非 temporal binding 在 init/resize/MSAA 切换时写入两份 copy
- 解决 temporal 资源（depth_resolved、ao_filtered）backing image 帧间 swap 导致的并发冲突：单份 Set 2 在 2 frames in flight 下 update 违反 Vulkan spec

---

### Step 3：Compute Pass Helpers

CommandBuffer + DescriptorManager 小幅扩展。

- [ ] CommandBuffer 新增 `push_storage_image(ResourceManager&, VkPipelineLayout, set, binding, ImageHandle)`
- [ ] CommandBuffer 新增 `push_sampled_image(ResourceManager&, VkPipelineLayout, set, binding, ImageHandle, SamplerHandle)`
- [ ] DescriptorManager 新增 `get_compute_set_layouts(VkDescriptorSetLayout set3_push)` 返回 `{set0, set1, set2, set3}`

**验证**：编译通过，无功能变化（无调用方）

#### 设计要点

Push descriptors 见 `milestone-1/m1-design-decisions.md`「Compute Pass 绑定机制」。

关键设计：
- Push descriptor helpers 显式传 `ResourceManager&` 用于 `ImageHandle → VkImageView` 解析，保持 CommandBuffer 作为纯 `VkCommandBuffer` wrapper
- `get_compute_set_layouts` 封装 Set 0-2 全局 layout + Set 3 push descriptor layout，避免 10+ per-frame compute pass 手动拼 layout，架构演进时单点修改

---

### Step 4：Phase 5 Config + GlobalUBO 扩展

数据结构声明，无渲染效果。

- [ ] RenderFeatures 新增 `ao` + `contact_shadows` 字段，Application 初始化
- [ ] `FEATURE_AO` + `FEATURE_CONTACT_SHADOWS` 常量（bindings.glsl），GlobalUBO `feature_flags` 填充逻辑扩展
- [ ] `AOConfig` + `ContactShadowConfig` 结构体（scene_data.h），Application 初始化默认值
- [ ] GlobalUBO 新增 `inv_projection` + `prev_view_projection`（scene_data.h + bindings.glsl 同步），Renderer 填充逻辑
- [ ] RenderInput + FrameContext 新增 `ao_config` + `contact_shadow_config` 指针

**验证**：编译通过，无 validation 报错，现有渲染不变

#### 设计要点

AO 命名规范见 `milestone-1/m1-design-decisions.md`「AO 命名」。AOConfig / ContactShadowConfig 见 `milestone-1/m1-interfaces.md`。

关键设计：
- Feature 层用 `ao`（RenderFeatures、feature_flags、DebugUI、config 结构体），Implementation 层用 `gtao`（shader 文件名、pass 类名）
- AO 参数通过 push constants 传递（仅 compute pass 消费，不膨胀 GlobalUBO），`inv_projection` 和 `prev_view_projection` 放 GlobalUBO（多 pass 共享）

---

### Step 5：Depth/Normal Set 2 绑定 + AO 资源创建

连接已有资源到 Set 2，创建 phase 5 新资源。

- [ ] Resolved depth 标记 temporal（`create_managed_image` 加 `temporal=true`），FrameContext 新增 `depth_prev`
- [ ] Set 2 binding 1（depth_resolved, nearest sampler）+ binding 2（normal_resolved, nearest sampler）写入双份 Set 2
- [ ] Temporal binding 1（depth_resolved）每帧更新当前帧 Set 2 copy 的逻辑
- [ ] 创建 managed images：`ao_noisy`（RG8）、`ao_filtered`（RG8 temporal）、`contact_shadow_mask`（R8），FrameContext 新增对应 RGResourceId
- [ ] Set 2 binding 3（ao_texture, linear sampler）+ binding 4（contact_shadow_mask, linear sampler）写入双份 Set 2
- [ ] Temporal binding 3（ao_filtered）每帧更新当前帧 Set 2 copy 的逻辑

**验证**：无 validation 报错，RenderDoc 可见新资源（内容为空/黑色）

#### 设计要点

关键设计：
- GTAO 输出 RG8：R=diffuse AO, G=specular occlusion。Step 7 先只写 R 通道（G=0），Step 12 升级 B1 后写 RG 两通道
- ao_noisy 和 ao_filtered 从一开始就创建为 RG8，Step 12 升级时不需要改资源格式
- Resolved depth 标记 temporal 后，RG 自动管理 double buffer，`get_history_image()` 返回上一帧深度，零额外 copy 开销

---

### Step 6：DebugUI 骨架

纯 UI 搭建，无渲染效果。

- [ ] Features 面板新增 AO + Contact Shadows checkbox
- [ ] AO 面板骨架（参数滑条占位，Step 7/10 填充）
- [ ] Contact Shadows 面板骨架（参数占位，Step 14 填充）

**验证**：面板可见，checkbox 可切换，无渲染效果

---

### Step 7：GTAO Pass（R8 diffuse AO）

首个 per-frame compute pass。输出仅 R 通道（G 写 0）。

- [ ] `gtao.comp`：inv_projection 重建 view-space position，view-space normal 转换（`mat3(view) * world_normal`）
- [ ] `gtao.comp`：horizon search（N 方向 × M 步），cosine-weighted 解析积分，interleaved gradient noise per-pixel 方向旋转
- [ ] `gtao.comp`：输出 RG8（R=AO, G=0.0），bias + intensity 应用
- [ ] `GTAOPass` 类（setup / record / destroy / rebuild_pipelines），Set 3 push descriptor layout + pipeline 创建（通过 `get_compute_set_layouts`）
- [ ] `GTAOPass::record()`：push constants 传 AO 参数，push descriptors 绑 ao_noisy storage output
- [ ] DebugUI AO 面板填充：radius、directions、steps per direction、bias、intensity 滑条

**验证**：RenderDoc 检查 ao_noisy — R 通道角落/缝隙暗、开阔平面 ~1.0；G 通道全 0

#### 设计要点

GTAO 算法选择见 `milestone-1/m1-design-decisions.md`「AO 算法选择」。

关键设计：
- GTAO 在 view-space 工作：`inv_projection` 从 depth 重建 view-space position，normal buffer 的 world-space 法线通过 `mat3(view)` 转换为 view-space
- Poisson disk 或均匀角度分布的搜索方向，per-pixel interleaved gradient noise 旋转消除 banding
- Push constants 传 AO 参数（仅 gtao.comp 消费，不放 GlobalUBO）
- SO 不在此步实现——先验证 diffuse AO 正确性，Step 12 升级到 B1

---

### Step 8：AO Temporal Filter（R8 单通道）

- [ ] Renderer 每帧追踪 `prev_view_projection`（帧末缓存当前帧 VP）
- [ ] `ao_temporal.comp`：reprojection（inv_view_projection → world → prev_view_projection → prev UV）
- [ ] `ao_temporal.comp`：三层 rejection（UV 有效性 + prev depth 深度一致性 + 邻域 clamp），R 通道处理，G 通道 passthrough（直接写 0.0）
- [ ] `ao_temporal.comp`：temporal blend + 首帧/resize/history 无效时 blend_factor=0
- [ ] `AOTemporalPass` 类（setup / record / destroy / rebuild_pipelines），push descriptors 绑 ao_noisy + ao_history + ao_filtered + depth_prev

**验证**：ao_filtered 比 ao_noisy 明显更平滑，相机移动时无可见 ghosting

#### 设计要点

Temporal 基础设施见 `milestone-1/m1-design-decisions.md`「Temporal 基础设施」。Rejection 策略见「Temporal Rejection」。Prev depth 见「Prev Depth」。

关键设计：
- 三层 rejection：(1) UV 有效性（prev_uv 越界 → reject）(2) prev depth 一致性（当前世界坐标在上一帧的预期深度 vs 实际存储深度）(3) 邻域 clamp（AO 通道 3×3 min/max clamp）
- G 通道当前为 0，passthrough 处理（Step 12 升级 B1 后 SO 通道不做邻域 clamp，使用与 AO 相同的 blend factor）
- Prev depth 来自 resolved depth 的 temporal history（`get_history_image(depth_handle)`），零额外 copy
- 首帧 / resize 后 / history 无效时 blend_factor = 0（纯用当前帧）

---

### Step 9：AO Forward 集成（近似 SO）

AO 管线首次产生可见画面效果。使用 Lagarde 近似公式作为 SO 中间验证基线。

- [ ] `forward.frag`：`screen_uv = gl_FragCoord.xy / global.screen_size`，采样 `rt_ao_texture`（Set 2 binding 3），读 R 通道
- [ ] Diffuse AO：`ssao × material_ao` + Jimenez 2016 multi-bounce 色彩补偿
- [ ] Specular Occlusion：Lagarde 近似公式（`saturate(pow(NdotV + ao, exp) - 1 + ao)`，使用材质 roughness），调制 IBL specular
- [ ] `FEATURE_AO` feature_flags 守护（禁用时 AO=1.0, SO=1.0）
- [ ] Renderer 编排：features.ao 控制 GTAOPass + AOTemporalPass record()，FrameContext 条件资源声明

**验证**：角落/物体接缝处有自然暗部，AO toggle on/off 对比明显，光滑金属角落 specular 适当衰减

#### 设计要点

AO 光照集成见 `milestone-1/m1-design-decisions.md`「AO 光照集成」。Specular Occlusion 分阶段实施见「Specular Occlusion」。

关键设计：
- Diffuse AO 使用 multi-bounce 色彩补偿（Jimenez 2016）：浅色表面（高 albedo）AO 压暗减轻，避免白墙角落过度黑暗
- Specular Occlusion 此步使用 Lagarde 近似公式（业界广泛使用：Unity HDRP、UE 等），基于材质 roughness + NdotV + AO 标量推导 SO，不需要 roughness buffer
- 仅调制间接光（IBL diffuse + IBL specular），直接光已有 shadow map + contact shadows 覆盖
- Step 12 升级到 B1 后，近似公式删除，改为直接读 AO 纹理 G 通道

---

### Step 10：AO Debug Modes + Temporal UI

调试工具完善。

- [ ] Debug render mode 新增 `DEBUG_MODE_AO_SSAO`（显示 GTAO R 通道原始输出），追加到 passthrough 末尾
- [ ] 现有 `DEBUG_MODE_AO` 改为显示 `ssao × material_ao` 复合结果
- [ ] DebugUI AO 面板补充 temporal blend 滑条

**验证**：debug modes 显示正确，temporal blend 滑条影响时域平滑程度

---

### Step 11：Roughness Buffer

DepthPrePass 扩展，独立于 GTAO 升级可验证。

- [ ] R8 roughness managed image 创建（Relative 1.0x，sample_count 跟随 MSAA），on_sample_count_changed 适配
- [ ] `depth_prepass.frag`：输出 roughness（`material.roughness_factor * texture(metallic_roughness_tex).g`）
- [ ] `depth_prepass_masked.frag`：同上
- [ ] DepthPrePass：roughness 作为额外 color attachment + MSAA AVERAGE resolve
- [ ] FrameContext 新增 `roughness` RGResourceId

**验证**：RenderDoc 检查 roughness 纹理 — 金属面 ~0.3-0.5、粗糙面 ~0.7-1.0，值与材质一致

#### 设计要点

关键设计：
- DepthPrePass 已读材质数据（alpha test），roughness 从同一材质取出，shader 改动很小
- MSAA 时 AVERAGE resolve（与 normal 一致）。Roughness 是标量 [0,1]，多个子采样平均物理合理
- M2 SSR 也需要 roughness buffer，此处同时为 SSR 铺路

---

### Step 12：GTAO Specular Occlusion 升级（B1）

从近似公式升级到精确计算。

- [ ] GTAO Set 3 push descriptor 新增 roughness sampled image binding
- [ ] `gtao.comp`：读 roughness buffer，重建 view direction + reflection direction
- [ ] `gtao.comp`：per-direction specular cone vs horizon overlap 评估，输出 G 通道（SO 标量）
- [ ] `ao_temporal.comp`：适配 RG8 双通道（SO 通道：相同 blend factor，不做邻域 clamp）
- [ ] `forward.frag`：SO 改为读 G 通道，删除 Lagarde 近似公式

**验证**：对比 Step 9 基线 — 光滑表面在部分遮蔽区域 SO 更精确（反射方向朝向开阔时 SO 更高），整体视觉差异细微但可在 debug mode 中对比

#### 设计要点

Specular Occlusion 方案见 `milestone-1/m1-design-decisions.md`「Specular Occlusion」。

关键设计：
- 方案 B1：GTAO 读 roughness buffer 获取实际 roughness，重建 view direction + reflection direction，每个 horizon search 方向直接评估 specular cone 与 horizon 的重叠。精度高于 bent normal 中间表示（方案 A），且 AO 纹理仅 RG8（vs RGBA8）
- SO 通道不做邻域 clamp（标量 SO 的 min/max 在物理上合理但视觉增益不明显），使用与 AO 相同的 blend factor
- 分阶段实施：Step 9 用 Lagarde 近似验证 AO 管线正确性，本步升级到 B1 后可直接对比精度差异

---

### Step 13：Contact Shadows Compute

- [ ] `contact_shadows.comp`：世界空间起点 + 光方向 × 最大距离 → 投影到屏幕空间 → UV 步进
- [ ] `contact_shadows.comp`：每步深度比较 + 深度自适应 thickness（`base_thickness × linear_depth`）
- [ ] `contact_shadows.comp`：距离衰减（首次命中 + smoothstep fade），push constants 传参数
- [ ] `ContactShadowsPass` 类（setup / record / destroy / rebuild_pipelines），push descriptors 绑 depth 读 + contact_shadow_mask 写

**验证**：RenderDoc 检查 contact_shadow_mask — 物体接地处有阴影遮罩，远端衰减渐变

#### 设计要点

Contact Shadows 设计见 `milestone-1/m1-design-decisions.md`「Contact Shadows」。

关键设计：
- Screen-space ray march：光方向投影到屏幕空间后在 UV 上均匀步进，每步线性插值深度（短 ray <0.5m 误差 <3%，可忽略）
- 搜索距离使用世界空间（物理意义明确），shader 内部投影到屏幕空间确定步进范围
- 深度自适应 thickness：`base_thickness × linear_depth`，远处更宽容（深度精度低 + 物体屏幕尺寸小）
- 距离衰减（方案 C）：首次命中 → 按 ray 上的位置 `smoothstep` 衰减。接触点全强度，搜索极限渐淡
- 无 temporal filter：确定性输出（无随机采样），步数不够则加步数
- Push constant 传光方向：shader 不假设光源类型。M1 单方向光单 dispatch，M2 多光源方案待定

---

### Step 14：Contact Shadows Forward 集成

- [ ] `forward.frag`：采样 `rt_contact_shadow_mask`（Set 2 binding 4），乘到直接光 shadow attenuation
- [ ] `FEATURE_CONTACT_SHADOWS` feature_flags 守护
- [ ] Renderer 编排：features.contact_shadows 控制 ContactShadowsPass record()
- [ ] DebugUI Contact Shadows 面板填充：step count（8/16/24/32）、max distance、base thickness 滑条

**验证**：物体接地处有细致接触阴影，toggle on/off 对比明显，远端边缘柔和无硬切线

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
  读: depth (push descriptor), light direction (push constant)
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
│   ├── gtao_pass.h              # [Step 7] GTAO compute pass
│   ├── ao_temporal_pass.h       # [Step 8] AO temporal filter compute pass
│   └── contact_shadows_pass.h   # [Step 13] Contact shadows compute pass
└── src/
    ├── gtao_pass.cpp            # [Step 7]
    ├── ao_temporal_pass.cpp     # [Step 8]
    └── contact_shadows_pass.cpp # [Step 13]
shaders/
├── gtao.comp                   # [Step 7] GTAO (horizon search), [Step 12] + specular occlusion
├── ao_temporal.comp            # [Step 8] AO temporal filter, [Step 12] RG8 双通道适配
└── contact_shadows.comp        # [Step 13] Screen-space contact shadows
```

### 修改文件

```
rhi/
├── include/himalaya/rhi/
│   ├── commands.h              # [Step 3] push descriptor helpers
│   └── descriptors.h           # [Step 2] get_set2(frame_index), get_compute_set_layouts
├── src/
│   ├── commands.cpp            # [Step 3]
│   └── descriptors.cpp         # [Step 2-3]
framework/
├── include/himalaya/framework/
│   ├── render_graph.h          # [Step 1] temporal API
│   ├── scene_data.h            # [Step 4] AOConfig, ContactShadowConfig, RenderFeatures, GlobalUBO
│   └── frame_context.h         # [Step 4-5] 新增 RGResourceId + config 指针
├── src/
│   └── render_graph.cpp        # [Step 1] temporal 实现
passes/
├── include/himalaya/passes/
│   └── depth_prepass.h         # [Step 11] roughness 输出
├── src/
│   └── depth_prepass.cpp       # [Step 11] roughness attachment + shader 修改
app/
├── include/himalaya/app/
│   ├── renderer.h              # [Step 4-14] RenderInput 扩展
│   └── debug_ui.h              # [Step 6] DebugUIContext 扩展
├── src/
│   ├── renderer.cpp            # [Step 2-14]
│   ├── application.cpp         # [Step 4] config 初始化 + RenderFeatures
│   └── debug_ui.cpp            # [Step 6-14] 面板逐步填充
shaders/
├── common/bindings.glsl        # [Step 4] FEATURE_AO, FEATURE_CONTACT_SHADOWS, GlobalUBO 新增字段
│                               # [Step 5] Set 2 binding 1-4
├── forward.frag                # [Step 9] AO 采样 + 近似 SO, [Step 12] B1 SO, [Step 14] contact shadow
├── depth_prepass.frag          # [Step 11] roughness 输出
└── depth_prepass_masked.frag   # [Step 11] roughness 输出
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| AO 命名 | Feature 层用 `ao`（RenderFeatures、feature_flags、DebugUI、config），Implementation 层用 `gtao`（shader、pass 类） |
| GTAO 算法 | 直接实现 GTAO（跳过 Crytek SSAO），结构化搜索 + 解析积分，原始噪声低于随机采样方案 |
| GTAO 输出 | RG8_UNORM：R=diffuse AO, G=specular occlusion。Step 7 先只写 R（G=0），Step 12 升级 B1 后写 RG |
| Specular Occlusion | 最终方案 B1：GTAO 读 roughness buffer + 重建 reflection direction，per-direction 评估 specular cone 与 horizon 重叠。分阶段实施：Step 9 先用 Lagarde 近似作为中间验证基线，Step 12 升级到 B1 |
| AO 集成 | Diffuse: `ssao × material_ao` + multi-bounce 色彩补偿 (Jimenez 2016)。Specular: Step 9 用 Lagarde 近似，Step 12 改为读 G 通道。仅间接光 |
| Multi-bounce | `max(vec3(ao), ((ao * a + b) * ao + c) * ao)` 其中 a/b/c 由 albedo 决定。浅色表面压暗减轻 |
| 无 blur pass | GTAO + temporal 即可得到干净结果，不需要额外空间模糊 |
| RG temporal | RG managed image 标记 `temporal=true` → 内部 double buffer，`clear()` 自动 swap，resize 重建两张。API：`get_history_image()` 返回上一帧 RGResourceId |
| Per-frame Set 2 | Set 2 分配 2 份（对应 2 frames in flight），temporal binding 每帧更新当前帧的 copy，解决帧间竞争（单份 Set 2 在 2 frames in flight 下 update 违反 Vulkan spec） |
| Compute pass 绑定 | Set 3 push descriptors（helpers 显式传 ResourceManager& 保持 CommandBuffer 纯 wrapper）。`get_compute_set_layouts(set3_push)` 封装 Set 0-2 全局 layout + Set 3，避免 10+ compute pass 手动拼 layout |
| AO 参数传递 | Push constants（仅 compute pass 消费，不膨胀 GlobalUBO） |
| Prev depth | Resolved depth 标记 temporal → RG 自动管理 double buffer → `get_history_image()` 返回上一帧深度，零 copy 开销 |
| Temporal rejection | 三层：(1) UV 有效性 (2) prev depth 深度一致性 (3) 邻域 clamp（仅 AO 通道，SO 不 clamp） |
| Contact Shadows | Screen-space march + 世界空间距离 + 深度自适应 thickness + 距离衰减（方案 C：首次命中 + smoothstep fade）+ 无 temporal + push constant 传光方向 |
| Contact Shadows 多光源 | M1 单方向光单 dispatch 单 R8。Push constant 传光方向使 shader 不假设光源类型。M2 多光源方案待定 |
| AO 分辨率 | 全分辨率。GTAO 采样效率高（4方向×4步=16 fetch），M1 无 TAA 半分辨率瑕疵更明显 |
| Roughness buffer | DepthPrePass 新增 R8 输出（Step 11）。GTAO 计算 SO 用（Step 12），M2 SSR 可复用 |
| GlobalUBO 增长 | 720 → ~848 bytes（+inv_projection 64 + prev_view_projection 64），仍远小于 16KB |
| DepthPrePass roughness | 已读材质数据（alpha test），roughness 从同一材质取出。MSAA 时 AVERAGE resolve（与 normal 一致） |
