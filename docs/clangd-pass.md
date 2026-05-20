# Clangd LSP Enhancement Pass — 使用说明

## 概述

为 codebase-memory-mcp 添加了可选的 clangd LSP 语义增强 pass，解决 C/C++ 项目中 tree-sitter 无法处理的：
- `DEFINE_bool` 等 GFlags 宏定义
- 跨文件虚函数/模板的调用关系
- 精确的 callHierarchy 调用链

当检测到 `clangd` + `compile_commands.json` 时自动启用，否则零开销跳过。

---

## 前置条件

1. **clangd** 在 `$PATH` 中可用（`which clangd` 能找到）
2. 目标 C/C++ 项目根目录下有 **`compile_commands.json`**
   - CMake 项目：`cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
   - Bear：`bear -- make`
   - 手动生成均可

两者缺一则 pass 自动跳过，不影响正常索引。

---

## 使用方式

### 正常使用（自动启用）

```bash
# 索引一个有 compile_commands.json 的 C++ 项目
codebase-memory-mcp index_repository /path/to/cpp-project
```

日志中会看到：
```
level=info msg=hooks.register phase=AFTER_LSP_CROSS name=clangd
level=info msg=pass.clangd.found files=scanning
level=info msg=pass.timing pass=clangd elapsed_ms=12345
level=info msg=pass.clangd.done files=42 gflag_nodes=3 edges=87
```

### 禁用 clangd pass

```bash
# 方式 1: CLI flag
codebase-memory-mcp --no-clangd index_repository /path/to/project

# 方式 2: 环境变量
CBM_NO_CLANGD=1 codebase-memory-mcp index_repository /path/to/project
```

日志中会看到：
```
level=info msg=pass.clangd.skip reason=no clangd or compile_commands.json
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
RETURN f.name, t.name
```

---

## 架构说明

### 隔离设计

所有扩展代码位于 `src/extensions/`，通过 pipeline hook 框架自注册，不直接修改上游 pipeline 核心逻辑：

```
src/extensions/
├── ext_bidir.h/c       — 双向子进程 IPC (pipe + fork)
├── ext_lsp_client.h/c  — JSON-RPC 2.0 LSP 客户端
└── ext_clangd.c        — pass 主逻辑 + constructor 自注册
```

### Hook 框架

`ext_clangd.c` 通过 `__attribute__((constructor))` 在程序加载时自动注册到 `CBM_HOOK_AFTER_LSP_CROSS` phase，在 `lsp_cross` pass 之后、`calls`/`parallel_resolve` 之前执行。

### 容错

- clangd 进程崩溃：自动重启一次，重启失败则 graceful 退出（不 crash）
- 单文件超时：60 秒超时保护
- 取消检查：每 10 文件检查 pipeline cancel 信号

---

## 编译

```bash
# 确保 Makefile.extensions 存在（启用扩展编译）
make -f Makefile.cbm cbm

# 运行测试
make -f Makefile.cbm test
```

如果要禁用扩展编译，删除或重命名 `Makefile.extensions` 即可，`-include` 静默跳过。

---

## 调试

clangd 的 stderr 输出写入 `/tmp/cbm-clangd-stderr.log`，可用于排查 clangd 初始化或索引问题：

```bash
tail -f /tmp/cbm-clangd-stderr.log
```

---

## 与上游同步

上游改动面仅 4 个文件、~15 行：

| 文件 | 改动 |
|------|------|
| `src/pipeline/pipeline.c` | +1 include, +4 行 hook 调用 |
| `Makefile.cbm` | +2 行 (pipeline_hooks.c + -include) |
| `tests/test_main.c` | +1 include, +2 行 |
| `src/main.c` | +3 行 (--no-clangd flag) |

合并上游时只需关注这 4 个文件的冲突。`src/extensions/`、`Makefile.extensions`、`tests/test_*.c` 上游不存在，永远不冲突。
