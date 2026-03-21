# Milestone 1：设计决策

> M1 各架构组件的设计选择与理由。接口定义见 `m1-interfaces.md`，长远架构目标见 `../project/architecture.md`，M1 功能范围见 `milestone-1.md`。

---

## 设计原则

既不过度设计（避免在项目初期花太多时间在框架上），也不欠缺考虑（避免以后吃亏需要推翻重来）。每个组件选择的实现级别都预留了向长远目标演进的通道。

---

<!-- 阶段一决策（Vulkan 1.4 核心特性、资源句柄、对象生命周期、帧同步、错误处理、ImGui 集成）已迁移至 archive/m1-phase1-decisions.md -->

---

<!-- Render Graph 核心设计（手动编排 + barrier 自动插入）、渐进式能力建设、帧间生命周期、Command Buffer 传递、import_image、Barrier 粒度/计算、Debug Utils、资源引用方式、READ_WRITE 语义、RG 接管范围、ImGui 作为 RG Pass、Swapchain Image 导入、RG 与 ResourceManager 关系、Barrier Aspect 推导、Framework 层 Vulkan 类型使用（阶段二）已迁移至 archive/m1-phase2-decisions.md -->

<!-- Render Graph 阶段三扩展（Managed 资源管理、Backing Image 即时查询、MSAA Resolve 策略、Depth Resolve 模式、Compute Pass 资源用途推导）已迁移至 archive/m1-phase3-decisions.md -->
<!-- Pass 类设计（层归属、FrameContext、Descriptor Set 绑定、具体类 vs 虚基类、方法职责、MSAA Pipeline 重建、Pipeline 创建与热重载、Attachment Format、显式 destroy）已迁移至 archive/m1-phase3-decisions.md -->

## Render Graph — 阶段五扩展

#### Temporal 资源管理（阶段五引入）

```cpp
// RGResourceDesc 中添加 is_temporal 标记
// 获取 temporal 资源的历史帧版本
ImageHandle get_history_image(RGResourceId id);
```

<!-- 帧间生命周期、Command Buffer 传递、import_image、Barrier 粒度/计算策略（阶段二）已迁移 -->

<!-- Compute Pass 资源用途推导（阶段三）已迁移至 archive/m1-phase3-decisions.md -->

---

<!-- 资源管理与描述符（阶段二）已迁移至 archive/m1-phase2-decisions.md -->
<!-- GlobalUBO 容量、Set 2 Layout（阶段三）已迁移至 archive/m1-phase3-decisions.md -->

---

<!-- 顶点格式、材质系统（含材质数据流、Per-draw 演进）、Shader 编译与热重载（阶段二）已迁移至 archive/m1-phase2-decisions.md -->

<!-- Shader 系统（公共文件组织、材质变体策略）（阶段三）已迁移至 archive/m1-phase3-decisions.md -->

---

## Temporal 数据管理

**选择：RG Managed Temporal（阶段五引入）**

`create_managed_image()` 新增 `temporal=true` 参数，RG 内部分配第二张 backing image，`clear()` 时自动 swap current/history，resize 时重建两张并标记 history 无效。

- `get_history_image()` 始终返回 valid RGResourceId（两张 backing image 总存在），`is_history_valid()` 查询 history 内容是否有效（首帧/resize 后无效，调用方据此设 blend_factor=0）
- `use_managed_image(handle, final_layout)` 由调用方显式指定 `final_layout`。Temporal current 传 `SHADER_READ_ONLY_OPTIMAL`（帧末 transition，确保 swap 后 history layout 正确）；非 temporal 传 `UNDEFINED`（不插入帧末 barrier）

**为什么始终返回 valid 而非首帧返回 invalid：** Compute pass 的 Set 3 是 push descriptor，不支持 `PARTIALLY_BOUND`，所有 binding 在 dispatch 时必须有效。始终返回 valid 使 pass 代码无分叉，所有 binding 无条件 push。首帧 history 内容是垃圾但 blend_factor=0 意味着 shader 忽略它。

**为什么 temporal current 需要 final_layout：** 非 temporal managed image 不做 final transition（每帧 UNDEFINED 覆写，不关心帧间 layout）。Temporal current 帧末处于 compute pass 留下的 GENERAL layout，下帧 swap 后变为 history，需要以 SHADER_READ_ONLY_OPTIMAL 导入。帧末强制 transition 确保 layout 匹配，不依赖"上帧最后一个 pass 是什么类型"的假设。`final_layout` 作为 `use_managed_image` 的显式参数而非 RG 内部推导，保持 RG 不区分 temporal/非 temporal 的统一性，与 `import_image` 接受 `final_layout` 参数的 API 风格一致。

**M2 复用：** SSR、SSGI 的 temporal pass 使用同一机制——`temporal=true` + `get_history_image()` + `is_history_valid()`，零额外基础设施。

---

<!-- ImGui 集成（阶段一）已迁移至 archive/m1-phase1-decisions.md -->
<!-- App 层设计（阶段三）已迁移至 archive/m1-phase3-decisions.md -->

<!-- 配置与调参系统、深度缓冲与精度（阶段二）已迁移至 archive/m1-phase2-decisions.md -->

<!-- 阶段三决策已迁移至 archive/m1-phase3-decisions.md -->
<!-- 阶段四决策（Pass 运行时开关、CSM 阴影系统、Instancing、运行时加载、缓存基础设施、纹理压缩、KTX2、多级上传、IBL 缓存）已迁移至 archive/m1-phase4-decisions.md -->

---

## 总结

<!-- 阶段一条目已迁移至 archive/m1-phase1-decisions.md -->
<!-- 阶段二条目已迁移至 archive/m1-phase2-decisions.md -->
<!-- 阶段三条目已迁移至 archive/m1-phase3-decisions.md -->
<!-- 阶段四条目已迁移至 archive/m1-phase4-decisions.md -->

| 组件 | M1 实现级别 | 核心理由 |
|------|------------|----------|
| Temporal 数据 | RG managed temporal（阶段五引入：`temporal=true` + `get_history_image()`（始终 valid）+ `is_history_valid()`，内部 double buffer + swap，current 帧末 final_layout = SHADER_READ_ONLY_OPTIMAL） | 一次建设，AO/SSR/SSGI 复用 |
| AO 算法 | GTAO（直接实现，跳过 Crytek SSAO），全分辨率 | 差距仅 ~40 行 shader，避免 M2 替换丢弃品 |
| AO 命名 | Feature 层 `ao`（config、flag、UI），Implementation 层 `gtao`（shader、pass 类） | 特性与算法解耦，UE5 同模式 |
| AO 集成 | Diffuse indirect：`ssao × material_ao` + multi-bounce 色彩补偿（Jimenez 2016）。Specular indirect：仅由 SO 控制（Lagarde 近似 → B1），material AO 不参与 specular（AO 是方向无关标量，specular 是方向相关的，标量 AO 乘 specular 会错误压暗反射方向朝向开阔区域的表面） | Diffuse 侧物理合理（标量 AO 近似半球可见性），specular 侧由方向性 SO 控制更正确 |
| Specular Occlusion | GTAO 直接计算标量 SO（方案 B1：读 roughness buffer + 重建 R，per-direction 评估 specular cone 与 horizon 重叠）。分阶段实施：先用 Lagarde 近似公式验证 AO 管线（Step 9），再升级到 B1（Step 12） | 精度最高（vs bent normal 有损中间表示），AO 纹理仅 RG8（vs RGBA8）。分阶段降低调试难度，近似基线可对比验证 B1 正确性 |
| AO Temporal | RG temporal double buffer + 三层 rejection（UV 有效性 + prev depth + 邻域 clamp），无 blur pass | GTAO 噪声低，temporal 即可得到干净结果 |
| Prev depth | Resolved depth 标记 temporal → RG 管理 double buffer → `get_history_image()` 返回上一帧深度 | 零 copy 开销，M2 SSR/SSGI 复用 |
| Temporal 基础设施 | RG managed temporal（`temporal=true` + `get_history_image()`（始终 valid）+ `is_history_valid()`），`use_managed_image(handle, final_layout)` 调用方显式传 final_layout（temporal current 传 SHADER_READ_ONLY_OPTIMAL，非 temporal 传 UNDEFINED），per-frame Set 2（2 份对应 2 frames in flight，temporal binding 每帧更新当前帧 copy） | RG 管理 double buffer 避免各 pass 重复代码，始终返回 valid 避免 push descriptor 分叉，final_layout 显式传参保持 RG 不区分 temporal/非 temporal，per-frame Set 2 解决 temporal binding 帧间竞争 |
| Compute pass 绑定 | Set 3 push descriptors（`push_storage_image` / `push_sampled_image` CommandBuffer helpers，显式传 ResourceManager& 保持 CommandBuffer 纯 wrapper）。`DescriptorManager::get_compute_set_layouts(set3_push)` 返回 `{set0, set1, set2, set3}` 供 per-frame compute pipeline 创建 | Temporal swap 天然解决，无 descriptor set 生命周期管理。`get_compute_set_layouts` 避免 10+ compute pass 手动拼 Set 0-2 layout，架构演进时单点修改 |
| AO 参数传递 | Compute pass push constants（不放 GlobalUBO） | 仅 compute pass 消费，不膨胀全局 UBO |
| Roughness buffer | DepthPrePass 新增 R8 输出（managed image + MSAA AVERAGE resolve） | GTAO SO 计算需要实际 roughness，M2 SSR 复用 |
| Contact Shadows | Screen-space ray march + 世界空间搜索距离 + 深度自适应 thickness + 距离衰减（首次命中 + 远端 smoothstep fade）+ 无 temporal + push constant 传光方向 | Screen-space 业界标准，深度自适应远近兼顾，距离衰减物理合理（遮挡是二值的，fade 仅隐藏搜索边界 artifact） |
| Contact Shadows 多光源 | M1 单方向光单 dispatch 单 R8，shader 通过 push constant 接受光方向参数（不假设光源类型）。M2 多光源方案待定 | M1 接口通用，M2 方案需结合 forward 架构约束再定（per-light 独立 R8 在 forward renderer 下 Set 2 binding 不够用且性能代价高） |
