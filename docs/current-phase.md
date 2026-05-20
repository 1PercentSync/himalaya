# 当前阶段：Reflector Phase 2 — Gaussian Splatting 数据管线

> 目标：实现 PLY → glTF 转换和 GS glTF 加载，为 Phase 3 渲染提供 CPU 端数据。
> 技术决策见 `technical-decisions.md` 第 20-22 节。
> 任务清单见 `tasks/reflector-phase2.md`。

---

## 背景

Phase 1 完成管线精简后，代码库只保留 Path Tracing 核心。Phase 2 构建 Gaussian Splatting 的数据管线——从文件到 CPU 内存的完整路径。渲染（GPU buffer 创建 + compute pipeline）在 Phase 3 实现。

## 范围

### 新增

- **gltf_utils**（App 层）：从 SceneLoader 提取共享的 glTF 解析函数
- **GaussianSplatLoader**（App 层）：加载 `KHR_gaussian_splatting` glTF，输出 CPU 端 SoA 数据结构
- **GaussianSplatScene / GaussianSplatPrimitive**（Framework 层）：GS 数据结构定义，支持多 primitive 场景
- **PLY 转换器**（Framework 层）：INRIA 3DGS PLY → glTF 转换，含激活函数、坐标系转换、缓存
- **tinyply**（third_party）：PLY 文件解析库

### 重构

- SceneLoader 提取 `parse_gltf`、`has_gaussian_splatting`、`transform_aabb` 到 gltf_utils
- Application 层文件路由逻辑（按 extensionsUsed 选择 loader）

### 不在范围内

- GPU buffer 创建和上传（Phase 3）
- GS 渲染 pipeline 和 shader（Phase 3）
- RenderMode::GaussianSplatting 的实际渲染路径（Phase 3）

## 数据格式

### KHR_gaussian_splatting glTF 扩展

Khronos 官方扩展（Release Candidate），在 mesh primitive 上定义 GS 数据：

- 必选 attribute：POSITION、ROTATION、SCALE、OPACITY、SH_DEGREE_0_COEF_0
- 可选 attribute：SH_DEGREE_1-3 系数（最多 15 组 VEC3）
- Extension 属性：kernel、colorSpace、projection、sortingMethod
- Primitive mode：POINTS (0)

### INRIA 3DGS PLY

训练框架输出的事实标准格式。与 glTF 的差异：

- opacity 为 pre-sigmoid 原始值（需 sigmoid 激活）
- scale 为 log-scale（需 exp 激活）
- 四元数为 wxyz 顺序（glTF 为 xyzw）
- 坐标系为 COLMAP/OpenCV（Y 下 Z 前，glTF 为 Y 上 Z 后）

## 实现步骤

共 4 个 Step，详见 `tasks/reflector-phase2.md`。

| Step | 内容 | 说明 |
|------|------|------|
| 0 | 基础设施 | gltf_utils 提取、tinyply 集成、CMake 配置 |
| 1 | PLY 转换器 | tinyply 解析 → 数据转换 → .gltf 输出 → 缓存 |
| 2 | GS glTF 加载器 | 数据结构定义、extension JSON 提取、attribute 读取、AABB 计算 |
| 3 | Application 集成 | 文件路由、PLY 自动转换、相机初始化 |
