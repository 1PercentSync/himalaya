# Milestone 2：画质全面提升 + 实时路径追踪

> **目标**：在 M1 基础上吃掉所有低垂的果实，达到比较好的视觉效果；同时引入实时路径追踪模式和混合 RT 效果。

---

## 预期效果

**光栅化模式**完成后画面将具有：动态天空昼夜变化（Bruneton 大气散射）、物理正确的大气散射与雾效（aerial perspective 替换高度雾）、距离相关的软阴影（PCSS，已在 M1 完成）、屏幕空间精确反射（SSR）和屏幕空间间接光照（SSGI）、POM 增强的表面深度感、完整的后处理链（DOF、Motion Blur、God Rays、Lens Flare、Film Grain、Chromatic Aberration）、SDK 上采样支持（FSR + DLSS）、多套 Lightmap blend 支持昼夜变化。

**PT 模式**提供实时路径追踪渲染（ReSTIR DI + SHaRC + NRD），画面水平 ≈ DOOM: The Dark Ages 路径追踪模式。混合 RT 效果（RT Reflections、RT Shadows）弥补光栅化模式的最大视觉短板。

写实度相比 M1 会有**质变级跳跃**。

---

## 实现内容

### 极低工作量（改几行到十几行代码）

| 项目 | 说明 |
|------|------|
| Burley Diffuse | 替换 Lambert，粗糙表面掠射角观感更好 |
| Film Grain | 增加画面质感，掩盖 banding |
| Chromatic Aberration | 增加镜头感 |
| ~~PCSS~~ | ~~在 PCF 基础上加 Blocker Search，半影随遮挡距离变化~~ — **已提前到 M1 阶段四 Step 7** |
| 多方向光 CSM | M1 限制 1 盏方向光，M2 扩展为 N 盏（每盏独立 shadow map + cascade 数据，GlobalUBO per-light cascade_view_proj）|
| Cascade blend 评估 | M1 Step 7 PCSS 实现后，M2 实测 lerp vs dithering + FSR/DLSS 的观感和性能，决定是否从 lerp 切换为 dithering。Lerp 在 PCSS 下 blend region 采样成本翻倍；M1 的 `blend_cascade_shadow()` 已隔离 blend 策略，切换成本低 |
| ~~GTAO~~ | ~~替换 SSAO 的遮挡计算公式~~ — **已提前到 M1 阶段五直接实现** |

### 低工作量

| 项目 | 说明 |
|------|------|
| SSR | 屏幕空间 Ray March，光滑地面/金属表面反射质变。为 SSGI 铺路（共享大量代码）|
| Tiled Forward | 2D 屏幕 Tile 光源裁剪（Compute Pass），从暴力 Forward 升级 |
| Screen-Space God Rays | 后处理 pass 径向模糊，三四十行 shader，体积渲染前的占位方案 |
| FSR SDK 接入 | 统一接口层 + FSR SDK，native AA + 上采样。静态场景仅需相机 jitter |

### 中等工作量

| 项目 | 说明 |
|------|------|
| SSGI | 与 SSR 共享 Ray March 基础代码，叠加在 Lightmap 上补充屏幕空间间接光 |
| Bruneton 大气散射 | 替换静态 Cubemap 天空，同时获得 aerial perspective（替换高度雾）|
| 多套 Lightmap blend | 在单套 Lightmap 基础上扩展，支持昼夜变化的间接光照插值 |
| POM | 砖墙、石板路等表面深度感显著提升 |
| Gaussian DOF | 按深度做模糊的基础景深效果 |
| Camera Motion Blur | 基于相机运动的模糊（静态场景 + 相机移动只需相机 reprojection） |
| DLSS SDK 接入 | M2 已有 FSR 和统一接口层，再接 DLSS 就是多一个后端，N 卡首选。静态场景仅需 camera motion vectors |
| Lens Flare | 基于 Bloom 降采样数据，复用 Bloom 中间结果，低成本画面点缀 |
| 2D 云层 | 噪声纹理铺在天空球上，配合 Bruneton 天空给出不错的室外画面 |

---

## 实时路径追踪（PT 模式）

基于 M1 建立的 RT 基础设施（加速结构、RT Pipeline），引入完整的实时路径追踪渲染模式。

| 项目 | 说明 |
|------|------|
| ReSTIR DI | 直接光照 reservoir 时空重采样，百万级光源高效采样 |
| SHaRC | Spatial Hash Radiance Cache，间接光路径提前终止查缓存 |
| NRD | GPU 实时降噪（ReBLUR 间接光 + ReLAX ReSTIR 信号 + SIGMA 阴影） |

---

## 混合 RT 效果

光栅化模式下的 RT 增强，仅选择光栅化有明显视觉瑕疵的效果：

| 项目 | 替换目标 | 说明 |
|------|---------|------|
| RT Reflections | SSR + Reflection Probes | SSR 屏幕边缘反射消失、Probes 低频近似失真，光栅化最大视觉短板 |
| RT Shadows | CSM / PCSS | Shadow map 分辨率锯齿、cascade 边界过渡、远处物体阴影消失 |
