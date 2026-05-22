# Codebase-Memory-MCP 使用指南

## 一、快速安装

### 1.1 从源码编译

```bash
git clone https://github.com/anthropics/codebase-memory-mcp.git
cd codebase-memory-mcp
make -f Makefile.cbm cbm
```

编译产物：`build/c/codebase-memory-mcp`

```bash
# 安装到 PATH
cp build/c/codebase-memory-mcp ~/.local/bin/
```

### 1.2 启用 clangd 增强（可选，仅 C++ 项目需要）

```bash
# 安装 clangd >= 18（macOS）
brew install llvm
echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc

# 验证
clangd --version  # 应显示 18+
```

### 1.3 启用扩展编译（clangd pass）

确保项目根目录下存在 `Makefile.extensions`：

```makefile
EXT_SRCS += src/extensions/ext_bidir.c
EXT_SRCS += src/extensions/ext_lsp_client.c
EXT_SRCS += src/extensions/ext_clangd.c

EXT_TEST_SRCS += tests/test_bidir.c
EXT_TEST_SRCS += tests/test_lsp_client.c
EXT_TEST_SRCS += tests/test_clangd.c

PIPELINE_SRCS += $(EXT_SRCS)
ALL_TEST_SRCS += $(EXT_TEST_SRCS)
```

重新编译：

```bash
make -f Makefile.cbm cbm
```

---

## 二、配置 MCP Server

### 2.1 Claude Code

在 `~/.claude/settings.json` 中添加：

```json
{
  "mcpServers": {
    "codebase-memory": {
      "command": "/path/to/codebase-memory-mcp"
    }
  }
}
```

### 2.2 Cursor

在 `.cursor/mcp.json` 中添加：

```json
{
  "mcpServers": {
    "codebase-memory": {
      "command": "/path/to/codebase-memory-mcp"
    }
  }
}
```

### 2.3 其他 MCP 客户端

codebase-memory-mcp 通过 stdin/stdout 运行 JSON-RPC 2.0 协议，兼容所有支持 MCP 的客户端。

---

## 三、索引项目

### 3.1 基本索引

```bash
codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/your/project"}'
```

### 3.2 配置参数追踪（param_whitelist）

在项目根目录创建 `.codebase-memory.json`：

```json
{
  "param_whitelist": [
    "use_balance_shuffle",
    "gpu_memory_fraction",
    "learning_rate",
    "batch_size"
  ]
}
```

**作用**：索引时会扫描所有源文件，将这些参数名的出现关联到所在函数，建立 `ConfigParam` 节点和 `PARAM_READ` 边。

**适用场景**：
- 跨语言配置追踪（Python config → C++ runtime）
- 关键参数的影响分析
- 配置变更的风险评估

### 3.3 配置文件扩展名映射（可选）

如果项目使用非标准文件扩展名：

```json
{
  "extra_extensions": {
    ".blade.php": "php",
    ".mjs": "javascript",
    ".cuh": "cpp"
  },
  "param_whitelist": [
    "use_balance_shuffle"
  ]
}
```

---

## 四、C++ 项目的 clangd 增强

### 4.1 生成 compile_commands.json

clangd pass 只分析 `compile_commands.json` 中列出的文件。

**CMake 项目**：

```bash
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -B build
ln -s build/compile_commands.json .
```

**Bear + Make**：

```bash
bear -- make
```

**KBuild 项目**：

```bash
kbuild gen_compile_json //your/project
```

**手动生成（Python 脚本）**：

```python
import json, os, glob

project = '/path/to/project'
includes = [
    '-I' + project,
    '-I/path/to/deps/include',
    '-I/path/to/tensorflow/include',
    # ... 添加所有需要的 -I 路径
]

entries = []
for f in glob.glob(os.path.join(project, '**/*.cc'), recursive=True):
    rel = os.path.relpath(f, project)
    if rel.startswith('third_party/'):
        continue  # 跳过第三方代码
    entries.append({
        'directory': project,
        'file': rel,
        'arguments': ['clang++', '-std=c++17', '-x', 'c++'] + includes + ['-c', rel]
    })

with open(os.path.join(project, 'compile_commands.json'), 'w') as fp:
    json.dump(entries, fp, indent=2)

print(f'Generated {len(entries)} entries')
```

**关键点**：
- 只包含你关心的源文件（决定 clangd 分析范围）
- Include 路径必须能让 clangd 找到头文件
- 排除 `third_party/` 可显著减少运行时间

### 4.2 运行索引

```bash
codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
```

日志中会看到：

```
time=... level=info msg=hooks.register phase=AFTER_LSP_CROSS name=clangd
time=... level=info msg=pass.clangd.found compile_db_files=429
time=... level=info msg=pass.clangd.poll event=ready file=src/foo.cc attempt=1 func_syms=12
time=... level=info msg=pass.clangd.progress done=100 total=429 edges=1234 elapsed=157s
time=... level=info msg=pass.clangd.done files=411 gflag_nodes=165 edges=2633
```

### 4.3 禁用 clangd pass

```bash
# 方式 1：CLI flag
codebase-memory-mcp --no-clangd cli index_repository '{"repo_path": "/path/to/project"}'

# 方式 2：环境变量
CBM_NO_CLANGD=1 codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'

# 方式 3：不提供 compile_commands.json（自动跳过）
```

---

## 五、查询图数据

### 5.1 MCP 工具（通过 Agent 使用）

在 Claude Code / Cursor 中，Agent 会自动调用以下 MCP 工具：

| 工具 | 用途 | 示例问题 |
|------|------|---------|
| `search_graph` | 搜索节点 | "找所有叫 BalanceShuffle 的函数" |
| `trace_path` | 追踪调用链 | "GetNextInternal 调用了谁" |
| `query_graph` | Cypher 查询 | "谁读了 use_balance_shuffle 参数" |
| `get_architecture` | 架构概览 | "这个项目的模块结构是什么" |
| `get_code_snippet` | 读取源码 | "展示 BalanceShuffle 构造函数" |

### 5.2 CLI 直接查询

```bash
# 搜索节点
codebase-memory-mcp cli search_graph '{"project":"your-project","name_pattern":"BalanceShuffle"}'

# Cypher 查询
codebase-memory-mcp cli query_graph '{"project":"your-project","query":"MATCH (m)-[:PARAM_READ]->(n {name:\"use_balance_shuffle\"}) RETURN m.name, m.file_path"}'

# 追踪调用链
codebase-memory-mcp cli trace_path '{"project":"your-project","function_name":"GetNextInternal","mode":"calls","direction":"callees","depth":3}'
```

### 5.3 常用查询示例

**参数影响分析**：

```cypher
MATCH (m)-[r:PARAM_READ]->(n {name:"use_balance_shuffle"})
RETURN m.name, m.file_path
```

**查看 clangd 增强的调用边**：

```cypher
MATCH (a)-[r:CALLS]->(b)
WHERE r.strategy = 'clangd_call_hierarchy'
RETURN a.name, b.name, b.file_path
LIMIT 20
```

**查找谁调用了某个 C++ 函数**：

```cypher
MATCH (m)-[r:CALLS]->(n {name:"EnsureStart"})
RETURN m.name, m.file_path, r.strategy
```

**查看 GflagDef 节点**：

```cypher
MATCH (n)
WHERE n.label = 'GflagDef'
RETURN n.name, n.file_path
```

---

## 六、可视化 UI

### 6.1 编译 UI 版本

```bash
make -f Makefile.cbm cbm-with-ui
cp build/c/codebase-memory-mcp ~/.local/bin/codebase-memory-mcp
```

macOS 需要签名：

```bash
codesign --force --sign - ~/.local/bin/codebase-memory-mcp
```

### 6.2 启动

```bash
codebase-memory-mcp --ui=true --port=9749
```

浏览器访问 http://localhost:9749/

### 6.3 UI 功能

- 代码图可视化（节点/边交互浏览）
- 项目管理（查看/删除已索引项目）
- 在线查询

### 6.4 删除项目索引

```bash
# 通过 UI API
curl -X DELETE "http://localhost:9749/api/project?name=your-project"

# 或直接删除 DB 文件
rm ~/.cache/codebase-memory-mcp/your-project.db
```

---

## 七、增量索引

修改文件后重新运行索引命令，会自动走增量路径：

```bash
codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
```

```
time=... level=info msg=pipeline.route path=incremental stored_hashes=1479
time=... level=info msg=incremental.classify changed=3 unchanged=1476 deleted=0
```

只重新解析变化的文件，未变化的直接复用。

**强制全量重建**：

```bash
rm ~/.cache/codebase-memory-mcp/your-project.db
codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
```

---

## 八、故障排查

### clangd pass 跳过了

```
level=info msg=pass.clangd.skip reason=no clangd or compile_commands.json
```

检查：
1. `which clangd` 能找到且版本 >= 18
2. 项目根目录有 `compile_commands.json`
3. 没有设置 `CBM_NO_CLANGD=1`

### clangd 文件超时

```
level=warn msg=pass.clangd.poll event=timeout file=xxx.cc reason=max_attempts_exceeded
```

原因：该文件依赖的头文件不全（include 路径缺失）或使用了 CUDA 语法。
处理：不影响其他文件，超时文件被跳过。如需解决，补全 `compile_commands.json` 中的 `-I` 路径。

### clangd stderr 日志

```bash
tail -f /tmp/cbm-clangd-stderr.log
```

### 索引后节点/边数为 0

检查 `.gitignore` 是否排除了源码文件，或项目路径是否正确。

### MCP 连接失败

确认 MCP 客户端配置中的 `command` 路径指向正确的 binary。
