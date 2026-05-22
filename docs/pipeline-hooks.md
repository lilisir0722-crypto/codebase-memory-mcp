# Pipeline Hook 框架：自注册扩展机制

## 背景

CBM（codebase-memory-mcp）是开源项目，我们需要在其 pipeline 中插入自定义 pass（如 clangd 语义增强），同时满足两个约束：

1. **最小化上游改动**——长期跟随上游迭代，merge 不能痛苦
2. **零配置注册**——扩展编译进去就自动生效，不需要手动修改注册列表

## 设计

### 核心机制

```
程序加载
  → __attribute__((constructor)) 自动执行
    → cbm_pipeline_hook_register(phase, fn, name)
      → 写入全局 hook 表

Pipeline 运行
  → pass_definitions → pass_lsp_cross
    → cbm_pipeline_hooks_run(AFTER_LSP_CROSS, ...)
      → 遍历 hook 表，依次调用注册的扩展 pass
    → pass_calls → ...
```

### 三个概念

**Phase**：pipeline 中的插入点。每个 phase 代表一个时机：

```c
typedef enum {
    CBM_HOOK_AFTER_LSP_CROSS = 0,  // lsp_cross 和 calls 之间
    CBM_HOOK_POST_RESOLVE    = 1,  // call/usage 解析之后
    CBM_HOOK_POST_EXTRACTION = 2,  // 并行提取之后
} cbm_hook_phase_t;
```

**Hook 函数**：扩展 pass 的入口，签名与现有 pass 一致：

```c
typedef int (*cbm_pipeline_hook_fn)(
    cbm_pipeline_ctx_t *ctx,      // pipeline 上下文（gbuf、registry 等）
    const cbm_file_info_t *files, // 文件列表
    int file_count,               // 文件数
    CBMFileResult **cache         // 提取结果缓存（可为 NULL）
);
```

**自注册**：扩展通过 GCC/Clang 的 `__attribute__((constructor))` 在 `main()` 之前自动注册：

```c
// src/extensions/ext_clangd.c

static int pass_clangd(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                       int file_count, CBMFileResult **cache) {
    // ... clangd 增强逻辑 ...
    return 0;
}

// 程序加载时自动执行，无需手动注册
static void __attribute__((constructor)) register_clangd(void) {
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, pass_clangd, "clangd");
}
```

## 上游改动

整个框架只需要在上游代码中添加 ~15 行：

### pipeline.c（+5 行）

```c
#include "pipeline/pipeline_hooks.h"  // +1 行

// 顺序路径：在 lsp_cross pass 完成后调用
if (rc == 0 && strcmp(seq_passes[si].name, "lsp_cross") == 0) {
    (void)cbm_pipeline_hooks_run(CBM_HOOK_AFTER_LSP_CROSS, ctx, files,
                                 file_count, seq_cache);     // +3 行
}

// 并行路径：在 lsp_cross 和 parallel_resolve 之间
(void)cbm_pipeline_hooks_run(CBM_HOOK_AFTER_LSP_CROSS, ctx, files,
                             file_count, cache);             // +1 行
```

### Makefile.cbm（+3 行）

```makefile
    src/pipeline/pipeline_hooks.c    # 新增源文件

-include Makefile.extensions         # 可选扩展编译（文件不存在时静默跳过）

CFLAGS_TEST = ... -DCBM_TESTING      # 测试构建标志
```

### test_main.c（+3 行）

```c
#include "pipeline/pipeline_hooks.h"
extern void suite_hooks(void);
RUN_SUITE(hooks);
cbm_ext_tests_run();                 // 运行扩展自注册的测试套件
```

### main.c（+3 行）

```c
if (strcmp(argv[i], "--no-clangd") == 0) {
    setenv("CBM_NO_CLANGD", "1", 1);
}
```

## 扩展侧文件结构

扩展代码全部在 `src/extensions/` 目录，上游不存在这个路径，**永远不会产生 merge 冲突**：

```
src/extensions/
├── ext_bidir.h/c          # 双向子进程 IPC
├── ext_lsp_client.h/c     # JSON-RPC 2.0 LSP 客户端
└── ext_clangd.c           # clangd pass + constructor 自注册

tests/
├── test_bidir.c           # IPC 测试（constructor 自注册）
├── test_lsp_client.c      # LSP 客户端测试（constructor 自注册）
├── test_clangd.c          # clangd pass 测试（constructor 自注册）
└── test_hooks.c           # hook 框架本身的测试

Makefile.extensions         # 扩展编译配置（不提交到上游）
```

### Makefile.extensions

```makefile
# 扩展源文件
EXT_SRCS += src/extensions/ext_bidir.c
EXT_SRCS += src/extensions/ext_lsp_client.c
EXT_SRCS += src/extensions/ext_clangd.c

# 扩展测试
EXT_TEST_SRCS += tests/test_bidir.c
EXT_TEST_SRCS += tests/test_lsp_client.c
EXT_TEST_SRCS += tests/test_clangd.c

# 接入构建
PIPELINE_SRCS += $(EXT_SRCS)
ALL_TEST_SRCS += $(EXT_TEST_SRCS)
```

删除 `Makefile.extensions` 即可完全禁用所有扩展，主程序编译和运行不受任何影响。

## 测试框架的自注册

扩展测试也使用同样的 constructor 机制，不需要修改 `test_main.c` 的测试列表：

```c
// tests/test_bidir.c

SUITE(bidir) {
    RUN_TEST(bidir_echo_cat);
    RUN_TEST(bidir_read_timeout);
    RUN_TEST(bidir_which_ls);
    // ...
}

static void __attribute__((constructor)) register_bidir_tests(void) {
    cbm_ext_test_register(suite_bidir, "bidir");
}
```

`test_main.c` 只需一行 `cbm_ext_tests_run()` 就能运行所有已注册的扩展测试：

```
=== hooks ===          ← 框架自身测试（RUN_SUITE 方式）
  hooks_register_and_count    PASS
  hooks_run_empty             PASS
  ...

=== bidir ===          ← 扩展自注册测试
  bidir_echo_cat              PASS
  bidir_read_timeout          PASS
  ...

=== lsp_client ===     ← 扩展自注册测试
  lsp_frame_format            PASS
  ...

=== clangd ===         ← 扩展自注册测试
  clangd_uri_to_rel_path      PASS
  ...

3581 passed, 1 failed
```

## 运行时行为

### 无扩展时（Makefile.extensions 不存在）

```
$ codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
# hook 表为空，cbm_pipeline_hooks_run 直接返回 0，零开销
```

### 有扩展时

```
$ codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
time=... level=info msg=hooks.register phase=AFTER_LSP_CROSS name=clangd
# ... pipeline 正常运行 ...
time=... level=info msg=pass.clangd.found compile_db_files=429
time=... level=info msg=pass.clangd.done files=411 gflag_nodes=165 edges=2633
time=... level=info msg=pass.timing pass=clangd elapsed_ms=...
```

### 有扩展但条件不满足时（无 clangd 或无 compile_commands.json）

```
$ codebase-memory-mcp cli index_repository '{"repo_path": "/path/to/project"}'
time=... level=info msg=hooks.register phase=AFTER_LSP_CROSS name=clangd
time=... level=info msg=pass.clangd.skip reason=no clangd or compile_commands.json
time=... level=info msg=pass.timing pass=clangd elapsed_ms=0
# 零开销跳过
```

## 添加新扩展

如果要添加一个新的扩展 pass（比如 rust-analyzer 增强），只需要：

**1. 创建扩展文件**

```c
// src/extensions/ext_rust_analyzer.c

static int pass_rust_analyzer(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                              int file_count, CBMFileResult **cache) {
    // ... rust-analyzer 增强逻辑 ...
    return 0;
}

static void __attribute__((constructor)) register_rust_analyzer(void) {
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, pass_rust_analyzer, "rust_analyzer");
}
```

**2. 加入 Makefile.extensions**

```makefile
EXT_SRCS += src/extensions/ext_rust_analyzer.c
```

**3. 完成**

不需要改 pipeline.c，不需要改 test_main.c，不需要改任何上游文件。编译后自动注册、自动运行。

## 设计权衡

| 选择 | 理由 |
|------|------|
| 固定大小数组（16 slots）而非动态分配 | constructor 在 `main()` 前执行，避免 malloc 依赖 |
| `const char *name` 借用而非拷贝 | 要求传入字符串字面量，避免生命周期问题 |
| Soft-fail（hook 失败不中断 pipeline） | 扩展是增强不是必须，失败降级好过中断 |
| 无重复注册检查 | constructor 不会重复调用，运行时动态注册不是设计目标 |
| 无 hook 卸载/反注册 | 生命周期与进程一致，无需运行时卸载 |
