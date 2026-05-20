# Reflector Phase 2：Gaussian Splatting 数据管线

> 目标：实现 PLY → glTF 转换和 GS glTF 加载，为 Phase 3 渲染提供 CPU 端数据。
> 技术决策见 `docs/technical-decisions.md` 第 20-22 节。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## 决策记录

| # | 问题 | 决定 |
|---|------|------|
| 1 | 模块归属 | App 层独立 `GaussianSplatLoader`，共享 `gltf_utils` 解析函数 |
| 2 | CPU 数据结构 | SoA 布局 |
| 3 | SH 存储 | 按实际 degree 存储，记录 `max_sh_degree` |
| 4 | Component type | 统一转 float |
| 5 | 预计算 vs 原始 | 保留原始 scale + rotation |
| 6 | Color space | 原样保留 SH 系数 + 记录元数据 |
| 7 | Extension JSON | nlohmann/json 二次解析 |
| 8 | Node transform | 保留 transform 矩阵，GPU 实时应用 |
| 9 | PLY 集成 | 转换器输出 .gltf 到磁盘，统一走 glTF 加载路径 |
| 10 | PLY 解析 | tinyply，third_party 源码集成，仅 INRIA 格式 |
| 11 | .gltf 输出 | nlohmann/json 手写，输出 .gltf + .bin |
| 12 | 坐标系转换 | 转换器中完成（COLMAP → glTF） |
| 13 | 缓存策略 | cache.h 基础设施，XXH3_128，`%TEMP%\himalaya\gaussians\` |
| 14 | 转换器位置 | Framework 层；同时通过 CLI 参数暴露为命令行转换模式 |
| 15 | Phase 2 输出边界 | 只交付 CPU 端数据，GPU upload 留给 Phase 3 |
| 16 | 多 primitive 支持 | 支持，GaussianSplatScene 包含 vector&lt;GaussianSplatPrimitive&gt; |
| 17 | 加载入口 | 双入口（PT/GS），用户显式选择模式，各 loader 自行检测数据，不做自动路由 |
| 18 | extensionsRequired 兼容 | GS loader 消毒 JSON 后传给 fastgltf（fastgltf 不认识 KHR_gaussian_splatting，会拒绝 extensionsRequired） |
| 19 | UI 集成 | 两个独立文件选择器（PT Scene + GS Scene），RenderMode 切换与文件选择无关，两场景可同时加载 |

---

## Step 0：基础设施

- [x] 集成 tinyply 到 `third_party/tinyply/`（源码 + CMakeLists.txt）
- [x] 更新顶层 `CMakeLists.txt`（add_subdirectory tinyply）
- [x] 创建 `app/include/himalaya/app/gltf_utils.h` 和 `app/src/gltf_utils.cpp`
- [x] 将 `SceneLoader::load()` 中的 glTF 文件解析逻辑提取为 `gltf_utils::parse_gltf` 函数
- [x] 将 `scene_loader.cpp` 匿名 namespace 中的 `transform_aabb` 提取到 gltf_utils
- [x] ~~添加 `has_gaussian_splatting` 函数到 gltf_utils~~（设计变更：移除，各 loader 自行检测）
- [x] 修改 `SceneLoader` 调用 gltf_utils 中的共享函数
- [x] 更新 `CLAUDE.md` 第三方库表（添加 tinyply）
- [x] 修正 `scene_data.h` 中 `RenderMode::GaussianSplatting` 注释（Phase 2 → Phase 3）
- [x] 编译验证

## Step 1：PLY 转换器

- [x] 创建 `framework/include/himalaya/framework/ply_converter.h`
- [x] 创建 `framework/src/ply_converter.cpp`
- [x] 实现 PLY 解析（tinyply 读取 position、rotation、scale、opacity、SH 系数）
- [x] 实现激活函数（sigmoid opacity、exp scale）
- [x] 实现坐标系转换（COLMAP → glTF：position Y/Z 取反）
- [x] 实现四元数转换（wxyz → xyzw，Y/Z 分量取反）
- [x] 实现 SH 系数坐标系转换（Y/Z 方向项符号翻转）
- [x] 实现 .gltf + .bin 输出（nlohmann/json 构造 JSON，手写 binary buffer）
- [x] 集成缓存（content hash → 缓存路径，命中时跳过转换）
- [x] 更新 `framework/CMakeLists.txt`（链接 tinyply）
- [x] 编译验证

## Step 2：GS glTF 加载器

- [x] 创建 `framework/include/himalaya/framework/gaussian_splat_data.h`（GaussianSplatPrimitive + GaussianSplatMetadata + GaussianSplatScene 结构体定义）
- [x] 创建 `app/include/himalaya/app/gaussian_splat_loader.h`
- [x] 创建 `app/src/gaussian_splat_loader.cpp`
- [x] 实现 extension JSON 提取（nlohmann/json 解析 .gltf JSON 或 .glb JSON chunk）
- [x] 实现 GS primitive 检测（遍历 mesh primitive 查找 KHR_gaussian_splatting extension）
- [x] 实现 attribute 读取（fastgltf iterateAccessor 读 position、rotation、scale、opacity）
- [x] 实现 SH 系数读取（检测 max degree，按实际 degree 分配数组）
- [x] 实现多 primitive 支持（遍历所有 GS primitive，各自构建 GaussianSplatPrimitive）
- [x] 实现 node transform 提取（iterateSceneNodes 获取每个 primitive 的世界变换矩阵）
- [x] 实现 AABB 计算（per-primitive bounds + scene_bounds 并集）
- [x] 更新 `app/CMakeLists.txt`
- [x] 编译验证
- [x] 实现 extensionsRequired 兼容（消毒 JSON 移除 KHR_gaussian_splatting 后传给 fastgltf；.gltf 重新序列化，.glb 重组内存 GLB）

## Step 3：Application 集成

- [x] 实现双入口加载（PT 入口加载 mesh，GS 入口加载 GS；.ply 经转换后由 GS 入口加载）
- [x] 实现 PLY 自动转换（加载 .ply 时调用转换器，使用缓存路径）
- [ ] 实现 GS 场景的相机初始化（从 GaussianSplatScene::scene_bounds）
- [ ] 编译验证

## Step 4：CLI 转换模式

- [ ] 通过 vcpkg 引入 CLI11
- [ ] 更新 `app/CMakeLists.txt`（链接 CLI11）
- [ ] 实现命令行参数解析（无参数 → GUI 渲染器；转换参数 → CLI PLY→glTF 转换）
- [ ] 更新 `CLAUDE.md` 第三方库表（添加 CLI11）
- [ ] 编译验证
