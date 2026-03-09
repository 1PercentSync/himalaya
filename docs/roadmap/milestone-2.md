# Milestone 2：画质全面提升

> **目标**：在 M1 基础上吃掉所有低垂的果实，达到比较好的视觉效果。

---

## 预期效果

完成后画面将具有：动态天空昼夜变化（Bruneton 大气散射）、物理正确的大气散射与雾效（aerial perspective 替换高度雾）、距离相关的软阴影（PCSS）、屏幕空间精确反射（SSR）和屏幕空间间接光照（SSGI）、GTAO、POM 增强的表面深度感、完整的后处理链（DOF、Motion Blur、God Rays、Lens Flare、Film Grain、Chromatic Aberration）、SDK 上采样支持（FSR + DLSS）、多套 Lightmap blend 支持昼夜变化。

写实度相比 M1 会有**质变级跳跃**。

---

## 实现内容

### 极低工作量（改几行到十几行代码）

| 项目 | 说明 |
|------|------|
| Burley Diffuse | 替换 Lambert，粗糙表面掠射角观感更好 |
| Film Grain | 增加画面质感，掩盖 banding |
| Chromatic Aberration | 增加镜头感 |
| PCSS | 在 PCF 基础上加 Blocker Search，半影随遮挡距离变化（替换 PCF，Contact Shadows 保留）|
| GTAO | 替换 SSAO 的遮挡计算公式，temporal filtering 复用 |

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
