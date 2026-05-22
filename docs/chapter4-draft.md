## 四、用 Language Server 补全代码图的 AST 盲区

第三章提到的三类盲区（宏不可见、跨文件类型断裂、模板追踪丢失），有一个工具能全部解决——clangd，LLVM 项目的 C++ Language Server，内部运行着完整的 Clang 编译器前端。

问题是：**它是为 IDE 交互设计的，不是为批量图构建设计的。**

### 4.1 三个设计决策

#### 决策一：增量补全，而非替换

一种直觉是：既然 tree-sitter 不够准，就用 clangd 替换它。

我们没有这样做。原因：

| | tree-sitter | clangd |
|--|-------------|--------|
| 速度 | 1479 文件 / 1 秒 | 429 文件 / 3 小时 |
| 语言 | 155 种 | C/C++/ObjC |
| 依赖 | 无 | 需要 compile_commands.json + 头文件 |
| 精度 | 文件内调用准确，跨文件靠推断 | 编译器级精确 |

tree-sitter 快但粗，clangd 准但慢且重。**二者互补好过二选一。**

方案：clangd 作为 tree-sitter 之后的**增量 pass**，只补充和修正调用边。

```
tree-sitter pass → CALLS edge (confidence = 0.90, strategy = "in_process")
      ↓
clangd pass    → CALLS edge (confidence = 0.95, strategy = "clangd_call_hierarchy")
```

同一条边被两个 pass 都发现时，高 confidence 的自动覆盖低的。没有 clangd 时，tree-sitter 的边仍然有效。

**效果**：kai 项目原有 77,134 条边，clangd 补充了 2,633 条——不是推翻重来，是在关键盲区精准打补丁。

#### 决策二：编译器做 Oracle，不做主力

clangd 能做什么？对任意一个函数，回答"它调用了哪些其他函数"（callHierarchy/outgoingCalls）。

一种做法是：让 clangd 索引全部代码，全量构建调用图。但这意味着：
- 每个文件需要 didOpen → 等待解析 → documentSymbol → callHierarchy → didClose
- TensorFlow 头文件的 preamble 解析需要 20-30 秒
- 429 个文件 × 平均 45 秒 = 不可接受

**我们的做法：先用 tree-sitter 画粗图，再用 clangd 补关键边。**

具体而言：tree-sitter pass 已经在图中建好了所有函数节点（名字、文件、行号）。clangd pass 不重新发现函数，只对已有节点发起定向 callHierarchy 查询。clangd 返回的 callee 如果在图中已有节点，就建边；如果是外部依赖（如 TensorFlow 内部函数），丢弃。

```
已有图: [BalanceShuffle::EnsureStart] ← tree-sitter 建好的节点
                                        但没有入边（不知道谁调用它）

clangd query: prepareCallHierarchy("EnsureStart") → outgoingCalls
clangd answer: GetNextInternal (shuffle_dataset_op.cc) 调用了它

结果: [GetNextInternal] ——CALLS——> [EnsureStart]   ← 新边，补全了断裂
```

**效果**：clangd 是"顾问"不是"工人"——只在 tree-sitter 无能为力的地方出手，其余时间零消耗。

#### 决策三：可插拔的扩展架构

这是一个重要的工程约束：**CBM 是开源项目，我们需要长期跟随上游迭代**。如果 clangd pass 的代码散布在上游核心文件中，每次 merge 都是痛苦。

方案：pipeline hook 框架 + `__attribute__((constructor))` 自注册。

```
上游 pipeline.c（改动 < 15 行）:
  pass_definitions → pass_lsp_cross →【hook 调用点】→ pass_calls
                                          ↑
                                    一行代码插入

扩展代码（独立目录，上游不存在）:
  src/extensions/ext_clangd.c
  → 程序加载时自动注册到 hook 点
  → 有 clangd + compile_commands.json → 执行增强
  → 没有 → 零开销跳过
```

| | 上游文件改动 | 扩展文件（不冲突） |
|--|------------|------------------|
| pipeline.c | +4 行 | ext_clangd.c（新） |
| Makefile.cbm | +3 行 | ext_lsp_client.c（新） |
| test_main.c | +2 行 | ext_bidir.c（新） |
| main.c | +3 行 | Makefile.extensions（新） |
| **合计** | **~15 行** | **~1500 行** |

**效果**：上游已经迭代了多个版本，merge 零冲突。同样的 hook 架构未来可接入 rust-analyzer、gopls 等其他 Language Server。

### 4.2 工程挑战（简述）

把 clangd 从"IDE 助手"改造成"批量图增强引擎"，还需要解决几个工程问题：

| 挑战 | 解法 |
|------|------|
| Preamble 冷启动（TF 头文件解析 20-30s） | 指数退避 poll：500ms→1s→2s→4s→5s(cap)，检测到函数符号即认为就绪 |
| prepareCallHierarchy 要求光标精确落在函数名上 | 从符号名计算 `ClassName::` 前缀长度作为偏移 |
| 不同版本 clangd 返回格式不同（扁平 vs 层级） | 栈遍历统一处理 DocumentSymbol children |
| clangd 进程可能 crash | 自动重启一次，失败则 graceful skip |
| 分析范围控制 | 只处理 compile_commands.json 中列出的文件（排序数组 + 二分查找） |

### 4.3 实际效果

#### 定量结果（KAI 项目，推荐系统训练框架）

| 指标 | 增强前 | 增强后 | 增量 |
|------|--------|--------|------|
| 图边数 | 77,134 | **79,767** | **+2,633** |
| GflagDef 节点 | 0 | **165** | +165 |
| C++ 调用链覆盖 | 仅文件内 | 跨文件/跨模块 | — |
| 分析范围 | — | 429 个 C++ 文件 | — |

#### 定性效果：从"断裂"到"贯通"

**增强前**——Agent 追踪 `use_balance_shuffle2` 参数：

```
ConfigParam: use_balance_shuffle2
  ← PARAM_READ — _load_static_flags (config.py)
  （链路断裂。C++ 层完全不可见。）
```

**增强后**——同一个查询：

```
ConfigParam: use_balance_shuffle2
  ← PARAM_READ — _load_static_flags (config.py)

BalanceShuffle::EnsureStart (balance_shuffle.cc)
  ← CALLS [clangd] — GetNextInternal (shuffle_dataset_op.cc)
  ← CALLS [clangd] — GetNextInternal (pull_prefetch_dataset_op.cc)

BalanceShuffle::MakeBatch (balance_shuffle.cc)
  ← CALLS — bucket_join.h
```

#### 效率对比

| 方式 | Token 消耗 | 延迟 | 准确率 |
|------|-----------|------|--------|
| Agentic Search (grep+read_file) | ~85,000 tokens | ~30s | 可能漏调用 |
| Code Knowledge Graph 查询 | ~200 tokens | <100ms | 精确边 |

### 4.4 局限与未来方向

**当前局限：**
- 需要 clangd >= 18（Apple 自带版本不支持 callHierarchy，需 `brew install llvm`）
- 首次索引耗时较长（~3 小时），因 preamble 解析 + background index 构建
- 部分文件因缺少头文件或 CUDA 语法而超时跳过（~16%）

**未来方向：**
- 并行化：多个 clangd 实例按文件分片
- 持久化 index：利用 clangd 的 `.cache/clangd/index/` 缓存，二次索引秒级完成
- 更多 Language Server：同样的 hook 架构接入 rust-analyzer、gopls
- 跨语言边自动推断：从 Python 配置模式（`StaticFlagsItem('flag', ..., 'target.cc')`）自动建立 ConfigParam → C++ 文件的边
