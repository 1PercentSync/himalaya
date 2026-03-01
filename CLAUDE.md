# Himalaya 渲染器

基于 Vulkan 1.4 的实时渲染器，以光栅化渲染为起步。

- 设计文档索引：`docs/README.md`
- 任务规划：`tasks/`

## 文档规范

- 文档内容：中文
- 文档文件名：英文

---

## 开发环境

| 项 | 值 |
|---|---|
| 操作系统 | Windows 11 |
| IDE | CLion |
| 编译器 | MSVC |
| CMake | 4.1 |
| 包管理 | vcpkg (manifest mode) |
| Vulkan | 1.4 |
| C++ | C++20 |

代码通过 WSL 中的 Agent 编辑，所有构建、编译、运行操作由用户在 CLion 中手动完成。

### Agent 约束

- **禁止** 执行 cmake、build、run 命令
- **禁止** 修改构建配置文件（CMakeLists.txt、vcpkg.json 等非代码文件），这些由用户在 CLion 中完成
- **只能** 创建和编辑 C++/GLSL 源码文件及文档
- 需要验证时告知用户在 CLion 中操作
- 每完成 `tasks/` 中的**一小项**任务后必须暂停，等待用户审查确认后再继续，**用户确认后才能提交 commit**
- 提交后必须更新 `tasks/` 中对应任务项的状态（勾选 `[x]`），作为单独的 `docs` commit
- 不要求每一小项都能编译，只需要一个 Step 结束时能编译通过即可
- 每完成一小项任务后**必须立即提交**一个 commit，不得遗漏

### Commit 规范

格式：`<type>(<scope>): <description>`

| type | 用途 |
|------|------|
| `feat` | 新功能 / 新代码 |
| `docs` | 文档 |
| `chore` | 构建配置、工具链、杂项维护 |
| `fix` | 修复 bug |
| `refactor` | 重构（不改变行为） |

scope 可选，常用值：`rhi`, `app`, `framework`, `passes`, `shaders`

**分离原则**：代码变更和文档变更必须分开提交，不得混在同一个 commit 中

---

## 编码规范

### 头文件

使用 `#pragma once`，路径带项目前缀：`#include <himalaya/rhi/context.h>`

### 代码文档

- 格式：Javadoc 风格 Doxygen（`/** */`）
- 语言：英文
- `.h` + `.cpp` 配对：`.h` 写接口文档（**what**：做什么、语义、约束），`.cpp` 写实现注释（**why/how**：为什么这么做、算法解释）
- 仅 `.h`（纯头文件库 / 数据结构定义）：文档全部写在 `.h` 中，同时包含 what 和 why
- 仅 `.cpp`（如 `main.cpp`）：文档全部写在 `.cpp` 中，同时包含 what 和 why
- 所有公开和私有接口、字段、枚举值均需写文档
- 文件头文档（`@file`）只写不会随开发变化的内容（文件名、所属模块），不写会过时的功能描述

### 命名

| 元素 | 规范 | 示例 |
|------|------|------|
| 类 / 结构体 | `PascalCase` | `RenderGraph`, `ImageHandle` |
| 方法 / 自由函数 | `snake_case` | `create_image()`, `begin_rendering()` |
| 私有成员 | `snake_case` 后缀 `_` | `device_`, `allocator_` |
| 公有成员 / 局部变量 | `snake_case` | `width`, `format` |
| 命名空间 | `snake_case` | `himalaya::rhi` |
| 枚举值 (scoped) | `PascalCase` | `Format::R8G8B8A8Unorm` |
| 常量 | `kPascalCase` | `kMaxFramesInFlight` |
| 宏 | `SCREAMING_CASE` | `VK_CHECK()` |

### 命名空间

| 命名空间 | 架构层 |
|----------|--------|
| `himalaya::rhi` | Layer 0 — Vulkan 抽象层 |
| `himalaya::framework` | Layer 1 — 渲染框架层 |
| `himalaya::passes` | Layer 2 — 渲染 Pass 层 |

---

## 项目结构

每层独立 CMake static library，编译期强制单向依赖 `rhi ← framework ← passes ← app`。

```
himalaya/
├── CMakeLists.txt
├── vcpkg.json
├── CLAUDE.md
├── rhi/                 # himalaya_rhi
│   ├── CMakeLists.txt
│   ├── include/himalaya/rhi/
│   └── src/
├── framework/           # himalaya_framework → himalaya_rhi
│   ├── CMakeLists.txt
│   ├── include/himalaya/framework/
│   └── src/
├── passes/              # himalaya_passes → himalaya_framework
│   ├── CMakeLists.txt
│   ├── include/himalaya/passes/
│   └── src/
├── app/                 # himalaya_app (exe) → all above
│   ├── CMakeLists.txt
│   └── main.cpp
├── shaders/
├── docs/
└── tasks/
```

禁止反向依赖：`rhi/` 不引用 `framework/`，`framework/` 不引用 `passes/`，`passes/` 各 pass 之间不互相引用。

---

## 关键技术约定

| 约定 | 选择 |
|------|------|
| 渲染模式 | Dynamic Rendering（Vulkan 1.3+ 核心） |
| 同步 API | Synchronization2（Vulkan 1.3+ 核心） |
| 管线动态状态 | Extended Dynamic State（Vulkan 1.3+ 核心） |
| 描述符 | Set 0 传统 Descriptor Set（全局数据） + Set 1 传统 Descriptor Set（Bindless 纹理） |
| 资源句柄 | Generation-based（index + generation） |
| 对象销毁 | 显式 `destroy()` 方法，不依赖析构函数 |
| 帧并行 | 2 Frames in Flight |
| 错误处理 | `VK_CHECK` 宏 + Validation Layer 常开 + `VK_EXT_debug_utils` |

---

## 第三方库

| 库 | 用途 |
|---|---|
| GLFW | 窗口 |
| GLM | 数学（直接使用 GLM 类型，不额外包装） |
| spdlog | 日志 |
| VMA | Vulkan 内存分配 |
| shaderc | 运行时 GLSL → SPIR-V 编译 |
| Dear ImGui | 调试 UI |

---

## Shader 工作流

CMake 构建后拷贝 shader 到 build 目录。开发期编辑 build 目录副本触发热重载，确认后同步回源码目录。
