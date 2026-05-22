# param_whitelist — 配置参数白名单建图

## 概述

`param_whitelist` 是 codebase-memory-mcp 的一个扩展特性，允许用户在 `.codebase-memory.json` 中声明一组**配置字段名白名单**，索引时自动扫描源码并生成 `PARAM_READ` 边，将字段名出现位置精确链接到对应的 `Function`/`Method` 节点。

这解决了一个核心问题：配置参数（如 `use_balance_shuffle2`、`gpu_memory_fraction`）在代码中不以独立的 AST 符号节点存在，因此无法通过常规的 `CALLS`/`READS` 边追踪，也无法被 `search_graph` 的结构查询命中。通过白名单机制，这些字段被提升为一等图节点（`ConfigParam`），并与引用它们的函数建立明确的结构性关联。

---

## 快速开始

在仓库根目录创建或修改 `.codebase-memory.json`：

```json
{
  "param_whitelist": [
    "use_balance_shuffle2",
    "gpu_memory_fraction",
    "local_shuffle_buffer_size"
  ]
}
```

重新索引：

```bash
codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/repo"}'
```

查询哪些函数读取了某个配置参数：

```cypher
MATCH (f)-[:PARAM_READ]->(p:ConfigParam {name: "gpu_memory_fraction"})
RETURN f.name, f.file_path, f.start_line
ORDER BY f.file_path
```

---

## 配置格式

`.codebase-memory.json` 支持与 `extra_extensions` 共存：

```json
{
  "extra_extensions": {
    ".blade.php": "php"
  },
  "param_whitelist": [
    "field_name_1",
    "field_name_2"
  ]
}
```

- 白名单仅从 **project 级别**（仓库根目录的 `.codebase-memory.json`）读取，不支持全局配置
- 字段名大小写敏感，完全匹配（字符串包含，非正则）
- 空字符串条目被静默忽略
- 缺少 `param_whitelist` 键时 pass 自动跳过，不影响其他索引流程

---

## 图模型

### 节点

| label | QN 格式 | 说明 |
|-------|---------|------|
| `ConfigParam` | `{project}.__param__{field_name}` | 虚拟节点，代表一个配置字段 |

节点 properties：

```json
{
  "source": "whitelist",
  "field": "gpu_memory_fraction"
}
```

### 边

| 边类型 | 方向 | 说明 |
|--------|------|------|
| `PARAM_READ` | `Function → ConfigParam` | 函数在某行引用了该配置字段 |

边 properties：

```json
{
  "line": 134,
  "file": "kai/python/tensorflow/config/backend_config.py"
}
```

---

## Cypher 查询示例

**查询哪些函数读取了某个参数：**

```cypher
MATCH (f)-[:PARAM_READ]->(p:ConfigParam {name: "gpu_memory_fraction"})
RETURN f.name, f.file_path, f.start_line
```

**反向：某个函数读取了哪些配置参数：**

```cypher
MATCH (f {name: "create_tf_config_proto"})-[:PARAM_READ]->(p:ConfigParam)
RETURN p.name
```

**多跳：参数被哪些函数间接调用（2 跳）：**

```cypher
MATCH (caller)-[:CALLS]->(f)-[:PARAM_READ]->(p:ConfigParam {name: "gpu_memory_fraction"})
RETURN caller.name, caller.file_path, f.name
ORDER BY caller.file_path
```

**查看所有已索引的 ConfigParam 节点：**

```cypher
MATCH (p:ConfigParam)
RETURN p.name, p.qualified_name
```

---

## 实现原理

### Pipeline 位置

`pass_param_reads` 在 `run_post_extraction` 中运行，位于 `run_tests_and_history` 之后、`run_predump_passes` 之前：

```
run_post_extraction()
  ├── run_tests_and_history()        ← githistory, tests
  ├── cbm_pipeline_pass_param_reads  ← 新增（本特性）
  └── run_predump_passes()           ← configlink, route_match, similarity ...
```

### 算法

```
对每个白名单字段名 field_name：
  1. upsert ConfigParam 节点（QN = project.__param__field_name）
  2. for each 已索引文件：
       逐行扫描源码（fgets，不解析 AST）
       if strstr(line, field_name)：
         find_enclosing_function(file, line_no)
           → 遍历 gbuf 中同文件的 Function/Method 节点
           → 找 start_line ≤ line_no ≤ end_line 的最内层节点
         insert_edge(func_id → param_id, "PARAM_READ", {line, file})
```

### 为什么不用 AST？

pass_param_reads 使用字符串扫描而非 Tree-Sitter AST，原因：

1. **AST 已在 pass_definitions 阶段释放**：pass_param_reads 在其后运行，源码 AST 不在内存中
2. **配置字段不是 AST 符号**：Python dataclass 字段名、C++ gflag 名称等在 AST 里是字符串字面量或标识符的一部分，不以独立符号形式存在
3. **字符串扫描足够精确**：结合 Function 节点的行号范围，已能精确定位包含函数，误报率低

---

## 改动文件清单

本特性遵循最小侵入原则，核心文件改动极小：

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `src/discover/userconfig.h` | 修改（+8行） | `cbm_userconfig_t` 加 `param_whitelist` 字段 |
| `src/discover/userconfig.c` | 修改（+45行） | 解析 `param_whitelist` JSON 数组 + 释放 |
| `src/pipeline/pass_param_reads.c` | **新增**（~180行） | 完整 pass 实现，不依赖任何现有 pass 内部逻辑 |
| `src/pipeline/pipeline_internal.h` | 修改（+6行） | 声明新 pass 函数 + include userconfig.h |
| `src/pipeline/pipeline.c` | 修改（**+1行**） | `run_post_extraction` 中调用新 pass |
| `Makefile.cbm` | 修改（+1行） | 将 `pass_param_reads.c` 加入编译列表 |

---

## 局限性与后续方向

### 当前局限

1. **字符串包含匹配**：`use_balance_shuffle2` 会命中注释行和字符串字面量中的出现，不区分"真正读取"和"字符串引用"
2. **不支持跨文件追踪**：只建立"包含该字符串的函数 → ConfigParam"的直接边，不做数据流传播
3. **白名单需手动维护**：字段名需要用户显式声明，不会自动发现

### 后续方向

- **与 kwai_graph_wiki 集成**：从 `KnowledgeGraph` 的 `ConfigParam` 节点自动生成白名单，不需要手动维护 `.codebase-memory.json`
- **多跳数据流**：在 `PARAM_READ` 边基础上，用 Cypher 查询 `CALLS` 链，自动推导参数影响哪些 metric emission 点
- **注释过滤**：在字符串扫描时跳过注释行（`//`、`#`、`/* */`）
