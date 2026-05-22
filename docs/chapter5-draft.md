## 五、应用效果

### 5.1 验证场景

在一个真实的推荐系统训练框架上验证——Python + C++ 混合架构，1479 个文件。

**目标问题**：`use_balance_shuffle` 参数影响了哪些 C++ 模块？

这个问题跨越了 Python 配置层和 C++ runtime 层，传统 grep 搜索只能找到 Python 层的引用，C++ 层完全不可见。

### 5.2 新增 ConfigParam 节点：把配置参数纳入图

现有的 Code Knowledge Graph 方案（GitNexus、Graphify 等）只建模代码结构——函数调用函数、类继承类。但在实际项目中，**配置参数才是控制代码行为的"隐形调用者"**。

我们在 CBM 中新增了 `ConfigParam` 节点类型和 `PARAM_READ` 边类型：

```json
// .codebase-memory.json
{
  "param_whitelist": ["use_balance_shuffle", "gpu_memory_fraction"]
}
```

索引时，`pass_param_reads` 扫描所有源文件，将参数名的文本出现关联到所在函数：

```
节点: ConfigParam:use_balance_shuffle
边:   Method:_load_static_flags ——PARAM_READ——> ConfigParam:use_balance_shuffle
```

这让 Agent 能回答"谁读了这个参数"——而不仅仅是"这个字符串在哪出现过"。

### 5.3 三层叠加贯通链路

| Pass | 产出 | 作用 |
|------|------|------|
| tree-sitter | 函数节点 + 文件内调用边 | 建立基础图结构 |
| pass_param_reads | `ConfigParam` 节点 + `PARAM_READ` 边 | Python 配置层 → 参数关联 |
| clangd callHierarchy | 跨文件 `CALLS` 边（confidence=0.95） | C++ 层调用链补全 |

**最终效果——Agent 一次图查询得到完整链路：**

```
use_balance_shuffle (ConfigParam)                           ← 新增节点类型
  ← PARAM_READ — _load_static_flags (config.py)            ← 新增边类型

BalanceShuffle::EnsureStart (balance_shuffle.cc)
  ← CALLS [clangd] — GetNextInternal (shuffle_dataset_op.cc)
  ← CALLS [clangd] — GetNextInternal (prefetch_dataset_op.cc)

BalanceShuffle::MakeBatch (balance_shuffle.cc)
  ← CALLS — bucket_join.h
```

从 Python 配置到 C++ runtime，三跳贯通。

### 5.4 定量结果

| 指标 | 增强前 | 增强后 |
|------|--------|--------|
| 图边数 | 77,134 | **79,767**（+2,633 clangd 边 +27 PARAM_READ 边） |
| ConfigParam 节点 | 0 | **+3**（白名单声明的参数） |
| GflagDef 节点 | 0 | **+165**（C++ 宏定义发现） |
| 参数可追踪深度 | 1 跳（Python 层） | **3 跳（跨语言贯通）** |

| 对比维度 | Agentic Search | Code Knowledge Graph |
|---------|---------------|---------------------|
| Token 消耗 | ~85,000 | **~200** |
| 延迟 | ~30s | **< 100ms** |
| 准确率 | 可能漏 C++ 层调用 | 精确边 |
