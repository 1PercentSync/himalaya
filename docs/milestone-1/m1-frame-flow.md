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
