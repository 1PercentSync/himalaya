# 远期 / 可选实现目标

> 本文档记录不排入 M1–M3 的技术项，按性质分类。
> 每一项都有明确的触发条件或前置依赖，不建议无条件实现。

---

## 画质提升

| 项目 | 触发条件 / 备注 |
|------|----------------|
| 体积云 | 替换 2D 云层。整个技术栈中复杂度最高的单一模块（Ray March + 3D 噪声 + 时域 reprojection + 半分辨率渲染 + 大气散射光照交互）。2D 云层配合 Bruneton 天空已足够 |
| Bokeh DOF | 替换 Gaussian DOF。半分辨率 gather-based Bokeh，画质提升明显但 DOF 在游戏中争议大。如演示场景不以浅景深为卖点可停在 Gaussian |
| Screen-Space SSS | 替换 Pre-Integrated SSS。质变级的真实皮肤感，但需处理轮廓渗色（深度/法线不连续性检测）|
| SDF 阴影混合 | **条件性**：仅当 GI 引入 SDF 基础设施时。CSM 近中距离 + SDF 远距离混合 |
| Irradiance Probes + SDF GI | **条件性**：依赖 SDF 基础设施。在 Lightmap + SSGI 基础上边际收益集中在"动态变化 + 屏幕外"交集 |
| HDR 输出 | HDR10 / scRGB 输出，需要两套 tonemapping 参数 + UI 亮度处理 + HDR 校准界面 |
| 广色域内部渲染 | ACEScg / Rec.2020 工作空间，需输入贴图色域转换。当前 sRGB 够用，架构预留 |

---

## 场景系统

| 项目 | 触发条件 / 备注 |
|------|----------------|
| 水体 | 静态水体渲染。大部分需求被已有基础设施覆盖（SSR→反射、屏幕空间折射→折射、PBR Fresnel→菲涅尔）。额外工作：深度着色、法线贴图、法线计算投影焦散（可预烘焙）、岸线泡沫 |
| 地形 | Heightmap（Clipmap LOD、Splatmap 材质混合）+ Mesh 补全（洞穴、悬崖）。仅大面积自然地形场景需要 |
| CPU 粒子 → GPU 粒子 | CPU 粒子：基础特效能力。GPU 粒子（Compute Shader）：百万级粒子 + 深度碰撞。与 Froxel 体积雾配合着色 |

---

## 透明 / 材质

| 项目 | 触发条件 / 备注 |
|------|----------------|
| WBOIT | 简单排序出现实际排序问题时切换。一个 pass 完成、不需排序、性能固定且低 |
| Pre-Integrated SSS | 角色渲染需要时。成本极低（LUT 替代 Lambert），Forward+ 天然适配 |

---

## 性能 / 基础设施

| 项目 | 触发条件 / 备注 |
|------|----------------|
| 异步资源加载框架 | 所有 Streaming 的前置依赖。后台线程加载 + Vulkan Transfer Queue 异步上传 |
| 纹理 Mip Streaming | 纹理总量超出显存预算时。按屏幕占比/距离动态加载 mip 级别 |
| Lightmap Streaming / 压缩 | 多套 Lightmap 数据量大时。优先尝试 BC6H 压缩 + 区域分块加载 |
| Virtual Texturing | 不实现，但 Lightmap 采样封装为统一接口预留替换可能 |
| XeSS SDK | 跨平台但生态不如 FSR，远期可选 |

---

## 神经网络渲染与算法演进

| 项目 | 说明 |
|------|------|
| NRC（Neural Radiance Cache） | 神经网络替代 SHaRC/ReSTIR GI 缓存层，精度更高，依赖 Tensor Core |
| DLSS Ray Reconstruction | 神经网络替代 NRD 传统降噪，单 pass transformer 推理，NVIDIA 专有 |
| Cooperative Vectors | DXR 1.2 / SM 6.9 标准化 shader 内神经网络推理，使 NRC、神经材质等跨厂商可用 |
| ReSTIR PT | GRIS 框架下对完整光传输路径做重采样，统一替代 ReSTIR DI + ReSTIR GI |

---

## 明确不做（记录）

| 项目 | 排除理由 |
|------|----------|
| GPU-Driven Rendering | 复杂度弥散到整个架构，Vulkan 多线程 Command Buffer + 几千到上万 drawcall 不是真正瓶颈 |
| Mesh Shader | 移动端不支持，无 GPU-Driven 时收益有限 |
| HLOD | 离线处理工具链开发成本高，场景规模不大时不需要 |
| Nanite-style 虚拟几何 | 个人项目从零实现不现实 |
| Virtual Shadow Maps | 实现极复杂（虚拟纹理系统 + 页面管理 + GPU-driven culling），CSM 足以满足需求 |
| Visibility Buffer | 公开完整实现和资料相对少，不符合"资料丰富、AI 辅助可靠"的约束 |

---

## 混合管线方向（已部分排入 M1–M3）

| 项目 | 说明 | 状态 |
|------|------|------|
| RT 烘焙管线 | GPU PT 烘焙器（Lightmap + Reflection Probes） | **M1 排入** |
| PT 参考视图 | Accumulation debug 渲染 + OIDN viewport denoising | **M1 排入** |
| RT Reflections | 替换 SSR + Reflection Probes | **M2 排入** |
| RT Shadows | 替换 CSM / PCSS | **M2 排入** |
| 实时 PT | ReSTIR DI + SHaRC + NRD | **M2 排入** |
| ReSTIR GI | 替换 SHaRC | **M3 排入** |
| RTAO | 替换 GTAO，接口一致（输入场景 → 输出单通道遮挡纹理） | 未排入（GTAO 观感尚可） |

---

## 角色相关（需要角色时才考虑）

| 项目 | 说明 |
|------|------|
| Pre-Integrated SSS → Screen-Space SSS | SSS 演进线 |
| 卡通渲染角色专属特性 | 面部阴影控制、描边、头发渲染等角色特有需求，依赖角色模型和 M3 场景卡通渲染基础 |
