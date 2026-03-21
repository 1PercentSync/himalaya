# 当前阶段：M1 阶段五 — 屏幕空间效果

> 目标：在阶段四的阴影基础上，实现 AO（GTAO 算法）+ temporal filtering + Contact Shadows，为场景增加空间层次感和接地感。
> 同时搭建 temporal filtering 基础设施（RG temporal、per-frame Set 2、compute pass push descriptors），为 M2 的 SSR/SSGI 奠定基础。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions-core.md`。

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

- `create_managed_image` 新增 `temporal` 参数，内部分配第二张 backing image（`history_backing`）
- `clear()` 时 swap current/history backing，resize 时重建两张并标记 history 无效
- `get_history_image(handle)` 始终返回 valid RGResourceId + `is_history_valid(handle)` 查询有效性
- `use_managed_image(handle, final_layout)` 新增 `final_layout` 参数（无默认值）：调用方显式传入。所有现有调用点适配传 `UNDEFINED`。Temporal image：current 以 UNDEFINED 导入 + `final_layout = SHADER_READ_ONLY_OPTIMAL`，history 以 SHADER_READ_ONLY_OPTIMAL 导入（首帧 UNDEFINED）

**验证**：现有渲染输出不变，无 validation 报错

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「Temporal 数据管理」。

---

### Step 2：Per-frame Set 2

DescriptorManager 变更 + 所有调用点适配。

- DescriptorManager：Set 2 pool 分配 2 份 set，`get_set2()` → `get_set2(uint32_t frame_index)`
- `update_render_target(binding, image, sampler)` 保留（内部写两份），新增 `update_render_target(frame_index, binding, image, sampler)`（写指定帧）
- 所有现有 `get_set2()` 调用点（Renderer bind 路径）适配 `get_set2(frame_index)`
- 所有现有 `update_render_target()` 调用点（init / resize / MSAA 切换）确认走双份更新路径

**验证**：现有渲染输出不变，无 validation 报错

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「Per-frame Set 2」。

---

### Step 3：Compute Pass Helpers

CommandBuffer + DescriptorManager 小幅扩展。

- CommandBuffer 新增 `push_storage_image(ResourceManager&, VkPipelineLayout, set, binding, ImageHandle)`
- CommandBuffer 新增 `push_sampled_image(ResourceManager&, VkPipelineLayout, set, binding, ImageHandle, SamplerHandle)`
- DescriptorManager 新增 `get_compute_set_layouts(VkDescriptorSetLayout set3_push)` 返回 `{set0, set1, set2, set3}`

**验证**：编译通过，无功能变化（无调用方）

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「Compute Pass Helpers」。

---

### Step 4：Phase 5 Config + GlobalUBO 扩展

数据结构声明，无渲染效果。

- RenderFeatures 新增 `ao` + `contact_shadows` 字段，Application 初始化
- `FEATURE_AO` + `FEATURE_CONTACT_SHADOWS` 常量（bindings.glsl），GlobalUBO `feature_flags` 填充逻辑扩展
- `AOConfig` + `ContactShadowConfig` 结构体（scene_data.h），Application 初始化默认值
- GlobalUBO 新增 `inv_projection` + `prev_view_projection`（scene_data.h + bindings.glsl 同步），Renderer 填充逻辑 + 帧末缓存当前帧 VP（首帧 prev = current）
- RenderInput + FrameContext 新增 `ao_config` + `contact_shadow_config` 指针

**验证**：编译通过，无 validation 报错，现有渲染不变

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「AO 命名约定」「GlobalUBO — 阶段五扩展」。AOConfig / ContactShadowConfig 见 `milestone-1/m1-interfaces.md`。

---

### Step 5：Depth/Normal Set 2 绑定 + AO 资源创建

连接已有资源到 Set 2，创建 phase 5 新资源。

- Resolved depth 标记 temporal（`create_managed_image` 加 `temporal=true`），FrameContext 新增 `depth_prev`
- Set 2 binding 1（depth_resolved, nearest sampler）+ binding 2（normal_resolved, nearest sampler）写入双份 Set 2
- Temporal binding 1（depth_resolved）每帧更新当前帧 Set 2 copy 的逻辑
- 创建 managed images：`ao_noisy`（RG8）、`ao_filtered`（RG8 temporal）、`contact_shadow_mask`（R8），FrameContext 新增对应 RGResourceId
- Set 2 binding 3（ao_texture, linear sampler）+ binding 4（contact_shadow_mask, linear sampler）写入双份 Set 2
- Temporal binding 3（ao_filtered）每帧更新当前帧 Set 2 copy 的逻辑

**验证**：无 validation 报错，RenderDoc 可见新资源（内容为空/黑色）

#### 设计要点

关键设计：
- GTAO 输出 RG8：R=diffuse AO, G=specular occlusion。Step 7 先只写 R 通道（G=0），Step 12 升级 B1 后写 RG 两通道
- ao_noisy 和 ao_filtered 从一开始就创建为 RG8，Step 12 升级时不需要改资源格式
- Resolved depth 标记 temporal 后，RG 自动管理 double buffer，`get_history_image()` 返回上一帧深度，零额外 copy 开销
- 新增 managed image usage flags：ao_noisy `Storage | Sampled`（GTAO storage write + AO Temporal sampled read），ao_filtered `Storage | Sampled`（AO Temporal storage write + Set 2 sampled），contact_shadow_mask `Storage | Sampled`（Contact Shadows storage write + Set 2 sampled）

---

### Step 6：DebugUI 骨架

纯 UI 搭建，无渲染效果。

- Features 面板新增 AO + Contact Shadows checkbox
- AO 面板骨架（参数滑条占位，Step 7/10 填充）
- Contact Shadows 面板骨架（参数占位，Step 14 填充）

**验证**：面板可见，checkbox 可切换，无渲染效果

---

### Step 7：GTAO Pass（R8 diffuse AO）

首个 per-frame compute pass。输出仅 R 通道（G 写 0）。

- `gtao.comp`：inv_projection 重建 view-space position，view-space normal 转换（`mat3(view) * world_normal`）
- `gtao.comp`：horizon search（N 方向 × M 步），cosine-weighted 解析积分，interleaved gradient noise per-pixel 方向旋转
- `gtao.comp`：输出 RG8（R=AO, G=0.0），bias + intensity 应用
- `GTAOPass` 类（setup / record / destroy / rebuild_pipelines），Set 3 push descriptor layout + pipeline 创建（通过 `get_compute_set_layouts`）
- `GTAOPass::record()`：push constants 传 AO 参数，push descriptors 绑 ao_noisy storage output
- DebugUI AO 面板填充：radius、directions、steps per direction、bias、intensity 滑条

**验证**：RenderDoc 检查 ao_noisy — R 通道角落/缝隙暗、开阔平面 ~1.0；G 通道全 0

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「AO 算法选择」「GlobalUBO — 阶段五扩展」。

实现细节：
- GTAO 在 view-space 工作：`inv_projection` 从 depth 重建 view-space position，normal buffer 的 world-space 法线通过 `mat3(view)` 转换为 view-space
- 均匀角度分布的搜索方向，per-pixel interleaved gradient noise 旋转消除 banding
- SO 不在此步实现——先验证 diffuse AO 正确性，Step 12 升级到 B1

---

### Step 8：AO Temporal Filter（R8 单通道）

- `ao_temporal.comp`：reprojection（inv_view_projection → world → prev_view_projection → prev UV）
- `ao_temporal.comp`：三层 rejection（UV 有效性 + prev depth 深度一致性 + 邻域 clamp），R 通道处理，G 通道 passthrough（直接写 0.0）
- `ao_temporal.comp`：temporal blend + 首帧/resize/history 无效时 blend_factor=0
- `AOTemporalPass` 类（setup / record / destroy / rebuild_pipelines），push descriptors 绑 ao_noisy + ao_history + ao_filtered + depth_prev

**验证**：ao_filtered 比 ao_noisy 明显更平滑，相机移动时无可见 ghosting

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「AO Temporal Filter」「Temporal 数据管理」。

实现细节：
- G 通道当前为 0，passthrough 处理（Step 12 升级 B1 后 SO 通道不做邻域 clamp，使用与 AO 相同的 blend factor）

---

### Step 9：AO Forward 集成（近似 SO）

AO 管线首次产生可见画面效果。使用 Lagarde 近似公式作为 SO 中间验证基线。

- `forward.frag`：`screen_uv = gl_FragCoord.xy / global.screen_size`，采样 `rt_ao_texture`（Set 2 binding 3），读 R 通道
- Diffuse AO：`ssao × material_ao` + Jimenez 2016 multi-bounce 色彩补偿
- Specular Occlusion：Lagarde 近似公式（`saturate(pow(NdotV + ao, exp) - 1 + ao)`，使用材质 roughness），调制 IBL specular
- `FEATURE_AO` feature_flags 守护（禁用时 AO=1.0, SO=1.0）
- Renderer 编排：features.ao 控制 GTAOPass + AOTemporalPass record()，FrameContext 条件资源声明

**验证**：角落/物体接缝处有自然暗部，AO toggle on/off 对比明显，光滑金属角落 specular 适当衰减

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「AO 光照集成」「Specular Occlusion」。

实现细节：
- 此步使用 Lagarde 近似公式（`saturate(pow(NdotV + ao, exp) - 1 + ao)`）作为 SO 中间验证基线，不需要 roughness buffer
- Step 12 升级到 B1 后，近似公式删除，改为直接读 AO 纹理 G 通道

---

### Step 10：AO Debug Modes + Temporal UI

调试工具完善。

- Debug render mode 新增 `DEBUG_MODE_AO_SSAO`（显示 GTAO R 通道原始输出），追加到 passthrough 末尾
- 现有 `DEBUG_MODE_AO` 改为显示 `ssao × material_ao` 复合结果
- DebugUI AO 面板补充 temporal blend 滑条

**验证**：debug modes 显示正确，temporal blend 滑条影响时域平滑程度

---

### Step 11：Roughness Buffer

DepthPrePass 扩展，独立于 GTAO 升级可验证。

- R8 roughness managed image 创建（Relative 1.0x，sample_count 跟随 MSAA），on_sample_count_changed 适配
- `depth_prepass.frag`：输出 roughness（`material.roughness_factor * texture(metallic_roughness_tex).g`）
- `depth_prepass_masked.frag`：同上
- DepthPrePass：roughness 作为额外 color attachment + MSAA AVERAGE resolve
- FrameContext 新增 `roughness` RGResourceId

**验证**：RenderDoc 检查 roughness 纹理 — 金属面 ~0.3-0.5、粗糙面 ~0.7-1.0，值与材质一致

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「Roughness Buffer」。

---

### Step 12：GTAO Specular Occlusion 升级（B1）

从近似公式升级到精确计算。

- GTAO Set 3 push descriptor 新增 roughness sampled image binding
- `gtao.comp`：读 roughness buffer，重建 view direction + reflection direction
- `gtao.comp`：per-direction specular cone vs horizon overlap 评估，输出 G 通道（SO 标量）
- `ao_temporal.comp`：适配 RG8 双通道（SO 通道：相同 blend factor，不做邻域 clamp）
- `forward.frag`：SO 改为读 G 通道，删除 Lagarde 近似公式

**验证**：对比 Step 9 基线 — 光滑表面在部分遮蔽区域 SO 更精确（反射方向朝向开阔时 SO 更高），整体视觉差异细微但可在 debug mode 中对比

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「Specular Occlusion」。

---

### Step 13：Contact Shadows Compute

- `contact_shadows.comp`：世界空间起点 + 光方向 × 最大距离 → 投影到屏幕空间 → UV 步进
- `contact_shadows.comp`：每步深度比较 + 深度自适应 thickness（`base_thickness × linear_depth`）
- `contact_shadows.comp`：距离衰减（首次命中 + smoothstep fade），push constants 传参数
- `ContactShadowsPass` 类（setup / record / destroy / rebuild_pipelines），push descriptors 绑 depth 读 + contact_shadow_mask 写

**验证**：RenderDoc 检查 contact_shadow_mask — 物体接地处有阴影遮罩，远端衰减渐变

#### 设计要点

见 `milestone-1/m1-phase5-decisions.md`「Contact Shadows」。

---

### Step 14：Contact Shadows Forward 集成

- `forward.frag`：采样 `rt_contact_shadow_mask`（Set 2 binding 4），乘到直接光 shadow attenuation
- `FEATURE_CONTACT_SHADOWS` feature_flags 守护
- Renderer 编排：features.contact_shadows 控制 ContactShadowsPass record()
- DebugUI Contact Shadows 面板填充：step count（8/16/24/32）、max distance、base thickness 滑条

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
Contact Shadows (compute)                          ← GTAO 与 Contact Shadows 无数据依赖，
  读: depth (push descriptor), light direction (push constant)    RG 不插入 barrier，GPU 可重叠执行
  写: contact_shadow_mask (R8, push descriptor storage image)
    ↓
AO Temporal (compute)
  读: ao_noisy, ao_history, depth, depth_prev (push descriptors)
  写: ao_filtered (push descriptor storage image)
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

> 设计决策详见 `milestone-1/m1-phase5-decisions.md`。以下为实现级别的补充备注。

| 主题 | 说明 |
|------|------|
| GTAO 输出 | RG8_UNORM：R=diffuse AO, G=specular occlusion。Step 7 先只写 R（G=0），Step 12 升级 B1 后写 RG |
| Compute workgroup size | 所有 compute pass（GTAO、AO Temporal、Contact Shadows）统一 8×8（每像素独立计算，无 shared memory 需求） |
| AO 资源 usage | ao_noisy / ao_filtered / contact_shadow_mask: `Storage \| Sampled`（compute storage write + sampled read）。roughness: `ColorAttachment \| Sampled`（DepthPrePass color attachment + GTAO push descriptor sampled） |
