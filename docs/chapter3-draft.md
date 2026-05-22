## 三、Code Knowledge Graph：让 Agent 查图而不是搜文本

### 3.1 核心思路

把代码预编译成图——函数、类、接口是节点，调用、继承、导入是边。Agent 不搜文本，查图。一次索引，反复查询。

### 3.2 行业方案对比

Code Knowledge Graph 已经不是新概念。近两年涌现了多个高 star 开源项目：

| 方案 | Stars | 原理 | 核心特点 |
|------|-------|------|---------|
| **Graphify** | 50k+ | Claude Code skill，LLM 提取概念和关系 | 多模态（代码/PDF/图片），edges 标记 EXTRACTED/INFERRED/AMBIGUOUS |
| **GitNexus** | 39k+ | tree-sitter AST + LadybugDB 图存储 | MCP 原生，Web UI + CLI，zero-server 架构 |
| **Sourcegraph** | — | CI 中运行 SCIP indexer | 企业级，精度最高，但需 K8s + CI 集成 |
| **CBM** | — | tree-sitter + 多策略推断 + 可选 LSP 增强 | 本地单二进制，零编译依赖 |

这些方案的共同点是：**解析层都依赖 tree-sitter 或 LLM 推断**。

对大多数语言（Python/TypeScript/Go/Java），这足够了——tree-sitter 能准确提取函数定义和调用关系。

但对 C++ 重宏代码库，所有方案都撞上了同一面墙：

```cpp
DEFINE_bool(use_balance_shuffle2, false, "...");
// GitNexus: tree-sitter 看到一个函数调用 → 建了一条错误的 CALLS 边
// Graphify: LLM 推断 → INFERRED 边，不确定是定义还是调用
// Sourcegraph: 需要 SCIP C++ indexer（依赖完整编译）
// CBM (tree-sitter only): 同样看不到
```

**这就是第四章要解决的问题——不是"建图"，而是"图有盲区，怎么补"。**

### 3.3 效果对比

同一个问题："`use_balance_shuffle2` 影响哪些 C++ 模块？"

| | Agentic Search | Code Knowledge Graph |
|--|---------------|---------------------|
| 过程 | grep × 12, read_file × 8 | query_graph × 2 |
| Token | ~85,000 | ~200 |
| 延迟 | ~30s | <100ms |
| 准确率 | 可能漏调用 | 精确边 |
