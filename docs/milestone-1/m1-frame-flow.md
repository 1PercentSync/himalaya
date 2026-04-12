# Milestone 1：帧流程设计

> 本文档记录 M1 一帧从头到尾的 pass 执行顺序、每个 pass 的输入输出、数据流转关系。
> 这是 Render Graph 中具体有哪些节点和边的定义。

---

## MSAA 处理策略

Depth/Normal PrePass 在 MSAA 下输出多采样 buffer，但屏幕空间效果（AO、Contact Shadows）全部在 **resolve 后的单采样** buffer 上操作。

具体流程：Depth/Normal 先 resolve → 屏幕空间效果在单采样上操作 → Forward Pass（MSAA）→ 透明 Pass（MSAA）→ Color resolve → 后处理链。

**取舍：** Resolve 会丢失子像素信息（一个像素内多个 sample 记录的不同 depth/normal 被平均），但 AO 本身是低频近似效果，加上 temporal filtering 的模糊，精度损失在视觉上不可感知。反过来，直接在 MSAA buffer 上操作需要 sampler2DMS per-sample 处理，性能和实现复杂度显著增加。

---

## 完整帧流程

### 阶段一：CPU 准备

```
视锥剔除
  输入：渲染列表 + 相机
  输出：可见物体列表（不透明 + 透明分开）

透明物体排序
  输入：可见透明物体列表 + 相机位置
  输出：按距离排序的透明物体列表
```

### 阶段二：阴影

```
CSM Shadow Pass（单 RG pass，内部循环每 cascade）
  输入：场景物体（per-cascade 光空间 frustum 剔除）、光源方向
  输出：Shadow Map 2D Array（每 cascade 一个 layer，D32Sfloat Reverse-Z）
```

### 阶段三：深度与法线

```
Depth + Normal PrePass（MSAA）
  输入：可见不透明物体列表、相机、材质（法线贴图）
  输出：Depth Buffer（MSAA）、Normal Buffer（MSAA）

Depth Resolve
  输入：Depth Buffer（MSAA）
  输出：Depth Buffer（单采样）

Normal Resolve
  输入：Normal Buffer（MSAA）
  输出：Normal Buffer（单采样）
```

### 阶段四：屏幕空间效果

```
GTAO Pass (compute)
  输入：Depth Buffer（单采样）、Normal Buffer（单采样）
  输出：AO Texture（RGBA8：RGB=bent normal 噪声版、A=diffuse AO 噪声版）

Contact Shadows Pass (compute)
  输入：Depth Buffer（单采样）、光源方向
  输出：Contact Shadow Mask（R8）
  注：与 GTAO 无数据依赖，RG 不插入 barrier，GPU 可重叠执行

AO Spatial Blur Pass (compute)
  输入：AO Texture（噪声版）
  输出：AO Texture（空间滤波后，RGBA8）
  注：5×5 edge-aware bilateral blur，深度驱动边缘权重，bent normal + AO 一起降噪

AO Temporal Filter Pass (compute)
  输入：AO Texture（空间滤波后）、AO History（上一帧）、Depth Buffer、Depth History（上一帧）
  输出：AO Texture（RGBA8 时域滤波后）、AO History（更新）
```

### 阶段五：主光照

```
Forward Lighting Pass（MSAA）
  输入：可见不透明物体列表、相机、材质（PBR 参数 + 纹理）、
        Shadow Map Atlas、AO Texture、Contact Shadow Mask、
        Lightmap、Reflection Probes、IBL Cubemap、光源数组
  输出：HDR Color Buffer（MSAA）
```

### 阶段六：透明

```
HDR Color Buffer Copy
  输入：HDR Color Buffer（MSAA）
  输出：Refraction Source Texture（折射采样源）

Transparent Pass（MSAA）
  输入：排序后的透明物体列表、相机、材质、
        Refraction Source Texture、Depth Buffer（MSAA）、
        光源数组、Shadow Map Atlas、IBL Cubemap
  输出：HDR Color Buffer（透明物体叠加上去）
```

### 阶段七：Color Resolve + Skybox

```
MSAA Resolve
  输入：HDR Color Buffer（MSAA）
  输出：HDR Color Buffer（单采样）

Skybox Pass
  输入：Depth Buffer（单采样）、IBL 中间 Cubemap
  输出：HDR Color Buffer（天空背景填充无几何覆盖的像素）
  说明：GREATER_OR_EQUAL depth test + depth write OFF，仅写 depth == 0.0 的像素
```

### 阶段八：后处理

```
Height Fog Pass
  输入：HDR Color Buffer、Depth Buffer（单采样）、相机参数、雾参数
  输出：HDR Color Buffer（雾混合后）

Auto Exposure Pass
  输入：HDR Color Buffer
  输出：Exposure Value（1×1 buffer，和上一帧平滑插值）

Bloom Downsample Chain（逐级降采样）
  输入：HDR Color Buffer
  输出：Bloom Mip Chain

Bloom Upsample Chain（逐级升采样混合）
  输入：Bloom Mip Chain
  输出：Bloom Texture

Tonemapping Pass
  输入：HDR Color Buffer、Bloom Texture、Exposure Value
  输出：LDR Color Buffer

Vignette Pass
  输入：LDR Color Buffer
  输出：LDR Color Buffer（Vignette 后）

Color Grading Pass
  输入：LDR Color Buffer
  输出：Final Color Buffer
```

---

## PT 参考视图帧流程（独立渲染路径）

PT 参考视图激活时，光栅化管线全部跳过，走独立的精简帧流程。

```
PT Reference View Pass (RT Pipeline)
  输入：TLAS、Geometry Info SSBO、材质（PBR 参数 + 纹理）、
        光源数组、IBL Cubemap、Accumulation Buffer（上一帧累积）
  输出：Accumulation Buffer（本帧累积后）
  说明：raygen shader 从相机发射射线，NEE + MIS + Russian Roulette，
        采样结果 running average 累积。相机移动时重置 sample count

OIDN Denoise（CPU 中转）
  输入：Accumulation Buffer（readback 到 CPU）
  输出：Denoised HDR Buffer（upload 回 GPU）
  说明：不必每帧执行，可每隔 N 次采样触发一次

Tonemapping Pass
  输入：Denoised HDR Buffer（或未降噪的 Accumulation Buffer）
  输出：Swapchain Image
```

### PT 参考视图关键资源

| 资源 | 产生 | 消费 | 存活范围 |
|------|------|------|----------|
| TLAS | 场景加载时构建 | PT Reference View Pass | 跨帧持久 |
| Geometry Info SSBO | 场景加载时构建 | PT Reference View Pass | 跨帧持久 |
| Accumulation Buffer（RGBA32F） | PT Reference View Pass | OIDN / Tonemapping | 跨帧持久（相机移动时清零） |
| Denoised HDR Buffer | OIDN | Tonemapping | 帧内 |

## 烘焙模式帧流程（独立渲染路径）

烘焙模式激活时，光栅化和 PT 参考视图均跳过，GPU 全力用于烘焙。逐项顺序执行：先所有 lightmap（逐 instance），再所有 probe（逐 probe）。

### Lightmap 烘焙（每个 mesh instance）

```
--- 初始化（烘焙该 instance 的第一帧） ---

Position/Normal Map Rasterization Pass (Graphics Pipeline)
  输入：Mesh vertex/index buffer（含 lightmap UV in uv1）、instance transform
  输出：Position Map（RGBA32F，lightmap 分辨率）、Normal Map（RGBA32F，lightmap 分辨率）
  说明：vertex shader 将 uv1 映射到 NDC，fragment shader 输出世界空间 position + normal

--- 每帧累积 ---

Lightmap Baker Pass (RT Pipeline)
  输入：Position Map、Normal Map、
        TLAS、Geometry Info SSBO、材质、光源数组、IBL Cubemap、
        Lightmap Accumulation Buffer（上一帧累积）
  输出：Lightmap Accumulation Buffer（本帧 1 SPP 累积后）
  说明：baker raygen 按 texel 坐标读 position/normal map，发射射线，
        复用 closesthit/miss/anyhit（baker_mode=1 跳过 OIDN aux 写入），
        running average 累积到 RGBA32F buffer

ImGui Bake Preview
  输入：当前 Accumulation Buffer
  输出：ImGui image widget 显示 UV 空间累积画面 + 进度文字

--- 达到目标采样数后（GPU idle） ---

BakeDenoiser（同步）
  输入：Accumulation Buffer readback + albedo/normal 辅助通道（如有）
  输出：Denoised RGBA32F CPU 内存

BC6H 压缩（GPU compute）
  输入：Denoised RGBA32F → upload GPU image
  输出：BC6H image

KTX2 持久化
  输入：BC6H image readback
  输出：cache_root()/bake/ 下的 KTX2 文件

释放 accumulation + position/normal map → 下一个 instance
```

### Probe 烘焙（每个 probe）

```
--- 每帧累积 ---

Probe Baker Pass (RT Pipeline)
  输入：Probe 世界空间位置、
        TLAS、Geometry Info SSBO、材质、光源数组、IBL Cubemap、
        Probe Accumulation Cubemap（上一帧累积，RGBA32F，6 face）
  输出：Probe Accumulation Cubemap（本帧 1 SPP 累积后）
  说明：raygen 从 probe 位置向 6 面方向发射射线（baker_mode=1）

ImGui Bake Preview
  输入：当前 Accumulation Cubemap（展示一个 face 或展开图）
  输出：ImGui image widget + 进度文字

--- 达到目标采样数后（GPU idle） ---

BakeDenoiser（同步，6 face 逐面降噪）
  输入：Accumulation Cubemap readback
  输出：Denoised RGBA32F CPU 内存

Prefilter Mip Chain（GPU compute，复用 prefilter 通用工具）
  输入：Denoised cubemap → upload GPU
  输出：Multi-mip prefiltered cubemap

BC6H 压缩（GPU compute，复用 BC6H 通用工具）
  输入：Prefiltered cubemap
  输出：BC6H cubemap

KTX2 持久化
  输入：BC6H cubemap readback
  输出：cache_root()/bake/ 下的 KTX2 文件

释放 accumulation cubemap → 下一个 probe
```

### 烘焙关键资源

| 资源 | 产生 | 消费 | 存活范围 |
|------|------|------|----------|
| Position Map（RGBA32F） | Pos/Normal Rasterization | Lightmap Baker Pass | 当前 instance 烘焙期间 |
| Normal Map（RGBA32F） | Pos/Normal Rasterization | Lightmap Baker Pass | 当前 instance 烘焙期间 |
| Lightmap Accumulation（RGBA32F） | Lightmap Baker Pass | BakeDenoiser readback | 当前 instance 烘焙期间 |
| Probe Accumulation Cubemap（RGBA32F） | Probe Baker Pass | BakeDenoiser readback | 当前 probe 烘焙期间 |
| TLAS + Geometry Info SSBO | 场景加载时构建 | 所有 Baker Pass | 跨烘焙持久 |

---

## Pass 粒度说明

所有效果保持独立 pass，实现清晰优先。以下效果有合并到其他 pass 的可能但 M1 不做：

| 效果 | 可合并目标 | M1 保持独立的理由 |
|------|-----------|------------------|
| Contact Shadows | Forward Lighting Pass shader 内部 | 光照 shader 不额外增加复杂度 |
| Height Fog | Forward Lighting Pass shader 内部 | 与光照 shader 解耦，可独立开关 |
| Vignette | Tonemapping Pass shader 内部 | 模块化更清晰，可独立开关 |
| Color Grading | Tonemapping Pass shader 内部 | 同上 |

---

## 关键资源生命周期

| 资源 | 产生 | 消费 | 存活范围 |
|------|------|------|----------|
| Shadow Map 2D Array | CSM Shadow Pass | Forward Pass、Transparent Pass | 帧内 |
| Depth Buffer（MSAA） | Depth PrePass | Depth Resolve、Forward Pass、Transparent Pass | 帧内 |
| Depth Buffer（单采样） | Depth Resolve | GTAO、Contact Shadows、Height Fog | 帧内 + 帧间（temporal，AO Temporal rejection 用） |
| Depth History（单采样） | 上一帧 Depth Resolve（RG temporal swap） | AO Temporal Filter（prev depth rejection） | 帧间（temporal） |
| Normal Buffer（单采样） | Normal Resolve | GTAO | 帧内 |
| Roughness Buffer（R8） | Depth PrePass | M2 SSR（原为 B1 SO 设计，SO 改为 GTSO 后 M1 无消费方） | 帧内 |
| AO Texture（RGBA8 噪声版） | GTAO Pass | AO Spatial Blur | 帧内 |
| AO Texture（RGBA8 空间滤波后） | AO Spatial Blur | AO Temporal Filter | 帧内 |
| AO Texture（RGBA8 时域滤波后） | AO Temporal Filter | Forward Pass（读 bent normal + AO，计算 GTSO） | 帧内 |
| AO History（RGBA8） | AO Temporal Filter | 下一帧 AO Temporal Filter | 帧间（temporal） |
| Contact Shadow Mask（R8） | Contact Shadows Pass | Forward Pass | 帧内 |
| HDR Color Buffer（MSAA） | Forward Pass | Transparent Pass、MSAA Resolve | 帧内 |
| Refraction Source | HDR Copy | Transparent Pass | 帧内 |
| HDR Color Buffer（单采样） | MSAA Resolve | Skybox Pass、后处理链 | 帧内 |
| Exposure Value | Auto Exposure | Tonemapping、下一帧 Auto Exposure | 帧间（temporal） |
| Bloom Mip Chain | Bloom Downsample | Bloom Upsample | 帧内 |
| Bloom Texture | Bloom Upsample | Tonemapping | 帧内 |
