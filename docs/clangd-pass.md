# Clangd LSP Enhancement Pass — 使用说明

## 概述

为 codebase-memory-mcp 添加了可选的 clangd LSP 语义增强 pass，解决 C/C++ 项目中 tree-sitter 无法处理的：
- `DEFINE_bool` 等 GFlags 宏定义
- 跨文件虚函数/模板的调用关系
- 精确的 callHierarchy 调用链

当检测到 `clangd` + `compile_commands.json` 时自动启用，否则零开销跳过。

---

## 前置条件

1. **clangd >= 18**（推荐 22+）在 `$PATH` 中可用
   - Apple 自带的 clangd 15.0 **不支持 callHierarchy**，需要安装新版：
   ```bash
   brew install llvm
   echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"' >> ~/.zshrc
   source ~/.zshrc
   clangd --version  # 应显示 18+ 或 22+
   ```
2. 目标 C/C++ 项目根目录下有 **`compile_commands.json`**
   - CMake：`cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
   - Bear：`bear -- make`
   - KBuild：`kbuild gen_compile_json //your/project`
   - 手动生成（Python 脚本，见下方示例）

两者缺一则 pass 自动跳过，不影响正常索引。

### compile_commands.json 手动生成示例

```python
import json, os, glob

project = '/path/to/project'
includes = ['-I' + project, '-I/path/to/deps/include', ...]

entries = []
for f in glob.glob(os.path.join(project, '**/*.cc'), recursive=True):
    rel = os.path.relpath(f, project)
    if rel.startswith('third_party/'):
        continue  # 排除第三方代码
    entries.append({
        'directory': project,
        'file': rel,
        'arguments': ['clang++', '-std=c++17', '-x', 'c++'] + includes + ['-c', rel]
    })

with open(os.path.join(project, 'compile_commands.json'), 'w') as fp:
    json.dump(entries, fp, indent=2)
```

**关键点**：
- 只包含你关心的源文件（clangd pass 只处理 compile_commands.json 中列出的文件）
- Include 路径必须能让 clangd 找到头文件（否则解析失败，该文件跳过）
- 排除 `third_party/` 可显著减少运行时间

---

## 使用方式

### 正常使用（自动启用）

```bash
codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/cpp-project"}'
```

日志中会看到：
```
time=2026-05-21T06:42:23 level=info msg=pass.clangd.found compile_db_files=429
time=2026-05-21T06:42:25 level=info msg=pass.clangd.poll event=ready file=src/foo.cc attempt=1 func_syms=12
time=2026-05-21T06:45:00 level=info msg=pass.clangd.progress done=100 total=429 edges=1234 elapsed=157s file=src/bar.cc
time=2026-05-21T06:50:00 level=info msg=pass.clangd.done files=411 gflag_nodes=165 edges=2633
```

### 禁用 clangd pass

```bash
# 方式 1: CLI flag
codebase-memory-mcp --no-clangd cli index_repository '{"repo_path": "/path/to/project"}'

# 方式 2: 环境变量
CBM_NO_CLANGD=1 codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
```

---

## 产生的图数据

### GflagDef 节点

对 `DEFINE_bool`、`DEFINE_int32`、`DEFINE_string`、`DEFINE_double` 等宏调用，创建 `GflagDef` 标签节点：

```
label: GflagDef
name: FLAGS_use_balance_shuffle2
qualified_name: my_project.__gflag__FLAGS_use_balance_shuffle2
file_path: src/config.cpp
properties: {"strategy": "clangd_document_symbol"}
```

查询示例：
```cypher
MATCH (g:GflagDef) RETURN g.name, g.file_path
```

### CALLS 边增强

通过 clangd 的 `callHierarchy/outgoingCalls` 补充/覆盖调用边：

```
type: CALLS
properties: {"strategy": "clangd_call_hierarchy", "confidence": 0.95}
```

confidence 0.95 会自动覆盖 in-process LSP 产生的 0.90 边（gbuf 去重机制）。

查询示例：
```cypher
MATCH (f)-[r:CALLS]->(t)
WHERE r.strategy = 'clangd_call_hierarchy'
RETURN f.name, t.name, t.file_path
```

---

## 工作原理

### 自适应解析等待

clangd 解析大型头文件（如 TensorFlow）需要时间。pass 采用自适应 poll 机制：

1. `didOpen` 发送源文件给 clangd
2. 以 500ms 起始、指数退避（最大 5s）轮询 `documentSymbol`
3. 当返回的符号中包含函数/方法（kind=6/9/12）时，认为解析就绪
4. 首 3 个文件允许最多 25 次重试（~90s preamble 冷启动）
5. 后续文件 15 次重试（~45s 超时），通常 500ms 内就绪（preamble 已缓存）

### 函数位置修正

clangd 的 `prepareCallHierarchy` 需要光标精确指向函数名。对于 SymbolInformation 格式（`location.range.start.character = 0`），自动计算 `ClassName::` 前缀长度作为偏移。

### 只处理 compile_commands.json 中的文件

启动时解析 `compile_commands.json` 建立文件集合（排序数组 + 二分查找），只处理其中列出的文件。用户通过控制 compile_commands.json 内容来决定分析范围。

---

## 架构说明

### 隔离设计

所有扩展代码位于 `src/extensions/`，通过 pipeline hook 框架自注册：

```
src/extensions/
├── ext_bidir.h/c       — 双向子进程 IPC (pipe + fork + poll)
├── ext_lsp_client.h/c  — JSON-RPC 2.0 LSP 客户端
└── ext_clangd.c        — pass 主逻辑 + constructor 自注册
```

### Hook 框架

`ext_clangd.c` 通过 `__attribute__((constructor))` 在程序加载时自动注册到 `CBM_HOOK_AFTER_LSP_CROSS` phase。

### 容错

- clangd 进程崩溃：自动重启一次，重启失败则 graceful 退出
- 单文件超时：45s 超时 + 自适应 poll（不会固定等待）
- 取消检查：每 10 文件检查 pipeline cancel 信号
- LSP error response：检测并记录，不 crash
- Background index：clangd 使用 `--background-index=true`，持续建索引

---

## 编译

```bash
# 确保 Makefile.extensions 存在（启用扩展编译）
make -f Makefile.cbm cbm

# 运行测试
make -f Makefile.cbm test
```

如果要禁用扩展编译，删除或重命名 `Makefile.extensions` 即可。

---

## 性能参考（kai 项目，429 个 C++ 文件）

| 指标 | 值 |
|------|-----|
| 总文件 | 429 |
| 成功处理 | ~350（其余超时跳过） |
| 新增 CALLS 边 | 2,633 |
| 耗时 | ~3.3 小时（含 preamble 冷启动） |
| 超时文件 | TF Op 注册文件（REGISTER_OP 宏过重） |

**首次运行慢，后续增量会快**（background index 持久化 + 增量索引只重处理变化文件）。

---

## 调试

```bash
# clangd stderr 日志
tail -f /tmp/cbm-clangd-stderr.log

# 检查 clangd 版本
clangd --version

# 验证 compile_commands.json 路径解析
# 在项目目录运行 clangd，看是否报 "file not found" 错误
```

---

## 与上游同步

上游改动面仅 4 个文件、~15 行：

| 文件 | 改动 |
|------|------|
| `src/pipeline/pipeline.c` | +1 include, +4 行 hook 调用 |
| `Makefile.cbm` | +3 行 (pipeline_hooks.c + -include + CFLAGS_TEST) |
| `tests/test_main.c` | +1 include, +2 行 |
| `src/main.c` | +3 行 (--no-clangd flag) |

`src/extensions/`、`Makefile.extensions`、`tests/test_*.c` 上游不存在，永远不冲突。
