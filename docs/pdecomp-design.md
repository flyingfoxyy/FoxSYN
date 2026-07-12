# pdecomp — partition-preserving LUT decomposition 设计

## 背景与动机

`csr` 的实验数据显示 K2（几乎等同门级网表）时收益率比 K4-K6 明显更高，但这组对比一直存在一个混杂变量：K2/K6 是各自独立跑一遍 `if -K` + `hpart`，两次得到的不仅节点粒度不同，**分区边界本身也不同**——不同映射结果 hMetis 划分出的跨分区边集合完全不可比，K 值效应和分区质量波动混在一起，测不出干净的结论（详见 `docs/csr.md` "LUT size（`-K`）敏感性" 一节的排查记录）。

`pdecomp` 要做的是把这两个变量解耦：**固定住已经分好区的 K6 网表，只把每个 LUT 内部展开成更细粒度的门（如 2-input），新节点直接继承父 LUT 的分区标签**。分解只重组单个 LUT 内部的逻辑结构，用的还是原来那些 fanin，不产生任何新的跨分区连接——所以宏观的分区边界、跨分区边集合在分解前后严格不变，唯一变化的是"节点粒度变细、每个分区内部的候选 divisor 池子变大"，这正是我们想单独测量的那个变量。

`pdecomp` 是纯结构变换命令，不判断"划算不划算"——它只负责证明分解本身没有破坏分区不变量（`hop`/`cut-edge` 严格相等），后续拿分解后的网表跑 `csr` 才是真正的优化。定位上，如果实验证明这条路径确实能提升收益率，`pdecomp` 未来有可能演化为 `csr` 流水线里的新阶段（如 "phase -1：先展开再优化"），所以设计上遵循 `csr`/`cpr` 现有模块的工程标准。

## 命令接口

```
pdecomp [-K num]
```

- `-K num`：目标最大 fanin 数，范围 2-6，默认 2（呼应 `if -K` 的语义；典型用途是分解到 2-input 验证"细粒度网表收益更高"的假设，但保留参数以后续测 K3/K4 中间粒度）。
- 前置条件：当前网络必须是逻辑网络且已有分区数据库（`Abc_NtkIsLogic(pNtk)` 为真、`pNtk->pPdb` 非空），否则报错退出——跟 `csr` 的前置检查风格一致（`csr` 在 `src/csr/csr.cpp` 的 `ApplyCsr` 里做的就是这三条检查：网络非空、是逻辑网络、有 pdb）。
- 不需要 `-v` 之类的 verbose 开关（初版不做逐节点日志，只在失败时报错）。

## 算法

### 定位：全网一次性展开 + 整体回滚

`pdecomp` 是"全有或全没有"的操作：遍历整个网络，收集所有 `Abc_ObjFaninNum(pObj) > K` 的节点，依次分解。**不做逐节点决策**（不像 `csr` Phase 0 那样每个节点独立判断是否接受）——因为这里要验证的是一个全局性假设（"整张网表变成 K2 粒度后 csr 收益更好"），不是局部优化，逐节点部分成功没有意义，反而会让实验对照变得不干净。

分解完成后做一次全局断言检查（见下），不通过就整体报错，保留原网络不变。

### 核心分解：复用 cofactor 递归模式，改分区赋值为继承

复用 `src/csr/csr.cpp` 里 `shannon_decompose_csr`（`csr.cpp:165`）已经验证过的 cofactor 递归分解模式：对每个待分解节点取真值表，用 `Kit_TruthCofactor0New`/`Kit_TruthCofactor1New` 按变量递归拆成两个子真值表，生成 mux 树，直到剩余变量数 ≤ K 时用 `Kit_TruthToHop` 建叶子节点的函数。

跟原函数的关键区别只有一处：**分区赋值方式**。`shannon_decompose_csr` 每层用 `choose_partition_csr` 重新在候选 fanin 里选一个"跨分区边最少"的分区（因为它是 `csr` Phase 1 里给"多 divisor 合并出的超过 6 输入的新逻辑"分配分区，那些 fanin 本身可能来自不同分区）。`pdecomp` 分解的是**已经存在、原本就完整地属于某一个分区**的 LUT，它的所有 fanin 集合不变，只是内部逻辑结构被重新表达——所以新生成的每一个中间节点和叶子节点都**直接赋值为被分解节点原来的 `part_id`**，不做任何选择。

伪码：

```
decompose_node(pNtk, pTruth, nVars, fanins, target_part_id):
    if nVars <= K:
        叶子节点 = Abc_NtkCreateNode(pNtk)
        叶子节点.fanins = fanins[0..nVars]
        叶子节点.pData = Kit_TruthToHop(pTruth, nVars)
        Abc_ObjSetPartId(叶子节点, target_part_id)   # 直接继承，不选择
        return 叶子节点

    cof0, cof1 = 对变量 0 做 cofactor
    subFanins = fanins[1..nVars]
    n0 = decompose_node(pNtk, cof0, nVars-1, subFanins, target_part_id)
    n1 = decompose_node(pNtk, cof1, nVars-1, subFanins, target_part_id)

    mux节点 = Abc_NtkCreateNode(pNtk)
    mux节点.fanins = [fanins[0], n1, n0]
    mux节点.pData = Hop_Mux(...)
    Abc_ObjSetPartId(mux节点, target_part_id)         # 直接继承，不选择
    return mux节点
```

主流程：

```
run_pdecomp(pNtk, K):
    initial_hop = Abc_NtkComputeHopNum(pNtk)
    initial_cutedge = ComputeCutEdgeCount(pNtk)

    targets = [n for n in pNtk.nodes if Abc_ObjFaninNum(n) > K]
    for pNode in targets:
        pTruth = 取 pNode 的真值表
        partId = Abc_ObjGetPartId(pNode)
        pRoot = decompose_node(pNtk, pTruth, Abc_ObjFaninNum(pNode), pNode.fanins, partId)
        Abc_ObjTransferFanout(pNode, pRoot)
        Abc_NtkDeleteObj_rec(pNode, 1)

    Abc_NtkSweep(pNtk, 0)
    Abc_NtkCleanup(pNtk, 0)

    final_hop = Abc_NtkComputeHopNum(pNtk)
    final_cutedge = ComputeCutEdgeCount(pNtk)

    if final_hop != initial_hop or final_cutedge != initial_cutedge:
        报错("pdecomp: partition invariant violated (hop %d->%d, cut-edge %d->%d), aborting"
             initial_hop, final_hop, initial_cutedge, final_cutedge)
        # 回滚：见下"回滚机制"
        return false
    return true
```

### 为什么 hop/cut-edge 严格相等是断言、不是门槛

已确认 `Abc_NtkComputeHopNum`（`src/abc/src/base/abc/abcPdb.cpp:211`）的定义是纯粹基于跨分区边的：`hop_level(n) = max over fanins fi of (hop_level(fi) + (partition(fi) != partition(n) ? 1 : 0))`，**只在 fanin 跨分区时才 +1，不管两者之间隔了多少层逻辑深度**。`ComputeCutEdgeNum`（`Abc_NtkComputeCutEdgeNum`，`abcPdb.cpp:181`）同样只统计跨分区的 `(driver, consumer)` 对数。

`pdecomp` 分解一个 LUT 时，新生成的中间节点全部继承同一个 `part_id`，只有分解后树的"叶子"（原 LUT 的原始 fanin）可能跨分区——但这些叶子集合跟分解前 LUT 的原始 fanin 集合完全相同，跨分区关系不变。分解只是在同分区内部插入了更多同分区节点，不改变任何跨分区边的存在与否。因此：

- **展开前后跨分区边集合理论上完全不变** → `cut-edge` 必须严格相等。
- **`hop` 只依赖跨分区边是否存在，不依赖层数** → 即使分解让逻辑深度变深（1 个 6-input LUT 展开成 4-5 层 2-input 门），只要新增的中间层都在同一分区内，`hop` 依然严格不变。

所以这两个检查不是"可能变差、需要拒绝"的优化门槛（像 `csr` Phase 0 的 `delta < 0` 判据），而是**验证实现正确性的断言**——如果分解逻辑有 bug（比如某个中间节点的 `part_id` 赋值错了、或者真值表分解错了导致引用了不该引用的 fanin），断言就会失败，此时应该整体报错，不能静默接受一个已知违反不变量的结果。

### 回滚机制

由于操作发生在 ABC 网络对象上（`Abc_NtkCreateNode`/`Abc_ObjTransferFanout`/`Abc_NtkDeleteObj_rec` 直接修改当前网络的对象图），断言失败后的"整体回滚"通过**操作前对整个网络做一次深拷贝**（`Abc_NtkDup`）实现：

```
run_pdecomp(pNtk, K):
    pNtkCopy = Abc_NtkDup(pNtk)   # 分解前先拷贝一份，用于失败时还原

    ... 分解逻辑（直接改 pNtk）...

    if 断言失败:
        还原 pNtk 到 pNtkCopy 的状态
        Abc_NtkDelete(pNtkCopy)
        return false

    Abc_NtkDelete(pNtkCopy)
    return true
```

具体"还原"的实现方式（整网替换当前 frame 里的 ntk 指针，还是逐字段拷贝回 pNtk）留给实现阶段依据 ABC 现有的网络替换惯例决定（`main.cpp` 里其他命令处理失败路径时的模式可参考）。

**实现阶段确认**：走的是"整网替换当前 frame 指针"路径——`Abc_NtkDup(pNtk)` 在分解前拍一份快照，分解逻辑直接改 `pNtk` 本体；断言失败时调用 `Abc_FrameReplaceCurrentNetwork(pAbc, pSnapshot)`，该函数会释放当前的 `pNtkCur`（即被污染的 `pNtk`）并把快照设为新的当前网络。之所以可行：读 `abcObj.c:399`（`Abc_NtkDupObj`）确认它对每个被复制的对象都会调 `Abc_ObjSetPartId(pObjNew, Abc_ObjGetPartId(pObj))`，所以 `Abc_NtkDup` 对逻辑网络（`ABC_NTK_LOGIC`）会完整保留每个节点的分区标签，快照本身就是一份带正确分区信息的完整备份，不需要额外补分区数据。

### 实现中发现并修复的架构问题：cofactor 树的 fanout 爆炸

第一次实现按原设计直译（每个新节点直接继承 `targetPartId`，不做额外处理）后，**在所有测试 case 上断言都立即失败**：`hop` 不变，但 `cut-edge` 从几百跳到几千（例如 `cavlc`：392→1446）。这不是偶发 bug，是 cofactor 树形分解的结构性问题——原设计文档"分解只重组 LUT 内部逻辑，不产生新跨分区边"这条论证隐含假设了"每个 fanin 变量在分解后的新结构里还是只被引用一次"，但朴素 cofactor 递归（`cof0`/`cof1` 两个分支各自独立递归、不共享子结构）会让**越晚被切掉的变量，在树里被复制引用的次数按 2^深度增长**。如果这个变量正好是跨分区 fanin，它原本贡献的 1 条跨分区边就会被放大成多条。

**修复**：在调用 `decompose_node` 之前，对每一个跟宿主 LUT 分区不同的 fanin，先插入一个同分区的 1-input 恒等缓冲节点（函数为 `Hop_IthVar(pHopMan, 0)`，直通），让分解树内部所有重复引用都指向这个缓冲节点而不是直接指向原始跨分区 fanin。这样"原始 fanin → 缓冲节点"这条边还是原来那 1 条跨分区边（数量不变），缓冲节点到分解树内部的所有边都是同分区，不管被引用多少次都不计入 `cut-edge`。代价是每个跨分区 fanin 多一个节点，是线性开销，不是指数级。

**连带发现**：`Abc_NtkSweep`（原计划里在分解完成后调用它 + `Abc_NtkCleanup` 做清理）会把所有 <2-fanin 的节点直接"焊掉"（`Abc_ObjPatchFanin` 绕过去），这正好包括刚插入的缓冲节点——调用它会让修复失效、cut-edge 又炸回去，而且它还会把网络转成 BDD 表示，跟本命令假设的 AIG/`Hop_Obj_t` 表示不兼容。**改为只调用 `Abc_NtkCleanup`**（只做从 PO 出发的可达性清理，不做结构合并，不动缓冲节点）。修复后在 `cavlc`/`voter`/`log2`/`max`/`arbiter`（EPFL）五个不同规模的 case 上验证：`hop`/`cut-edge` 严格相等，`cec` 确认功能等价。

### 跨模块兼容性问题：`csr` Phase 1 同样调用 `Abc_NtkSweep`

`pdecomp` 命令本身单跑没问题，但接上 `csr` 后在 EPFL 全量对比实验中暴露出同样的根因在另一个模块里重演：`csr.cpp` Phase 1 每轮都无条件调用 `Abc_NtkSweep`（用于折叠 MFS resub 产出的退化单输入节点），这个调用会把 `pdecomp` 插入的跨分区缓冲节点一并焊掉，导致缓冲节点保护的 fanout 爆炸 bug 在 `csr` 内部重新出现——实测 `cavlc` 接上 `csr` 后 cut-edge 收益率从直接 `csr` 的基线反而暴跌到 -253 个百分点（`csr` phase1 单轮 397→1394，`fixed=0`，即 0 个 resub 被接受、cut-edge 却暴涨，与 `pdecomp` 自身修复前的症状完全一致）。

**修复**：`src/csr/csr.cpp` Phase 1 每轮清理去掉 `Abc_NtkSweep`，只保留 `Abc_NtkCleanup`。改动前后用冻结分区（`--save-part`/`--load-part`）跑同一套 EPFL 分区对比 `csr` 自身（不接 `pdecomp`）的行为，确认 19/20 case 仍是 `OK`/`EQ`，无新增崩溃/超时，数值差异都在噪声量级内（个别 case ±1 节点/cut-edge 级别的抖动），判定该改动对 `csr` 既有行为安全。

**残留风险**：这是"哪个模块调 `Abc_NtkSweep` 就得避开缓冲节点"的一次性修复，不是根治。如果未来任何其他命令在 `pdecomp` 之后的流水线里插入了新的 `Abc_NtkSweep` 调用（或者其他会焊掉 <2-fanin 节点的清理逻辑），同样的 bug 会复现。更根本的修复方向是让缓冲节点本身对通用清理逻辑"隐形"（比如探索 `fPersist` 类的持久化标记扩展到 logic network，目前该字段仅用于 strash/AIG 网络），留作后续方向。

### 实验结果：`pdecomp -K 2` 接 `csr` 的实际收益

修复上述兼容性问题后，用 `scripts/run_pdecomp_csr_comparison.py`（冻结分区，隔离 hMetis 随机性）在 EPFL 全量 case（`hyp`/`log2`/`mem_ctrl` 超时排除）上对比"直接 `csr`" vs "`pdecomp -K 2` 后再 `csr`"：

| 配置 | 平均 cut-edge 削减率 |
|---|---|
| 直接 `csr` | 18.99% |
| `pdecomp -K 2` + `csr` | 18.93% |

**几乎没有差异（-0.06 个百分点），跟"LUT size 敏感性"实验（`docs/csr.md`）的结论一致**：`csr` 的收益主要由电路结构决定，节点粒度是次要因素。逐 case 看方向不一致：`sin`(+21.84pp)、`bar`(+9.55pp)、`adder`(+6.25pp) 明显受益，但 `priority`(-8.43pp)、`ctrl`(-7.14pp)、`dec`(-5.74pp) 反而变差,17 个可比 case 里 8 个变好 9 个变差,没有系统性趋势。全部 case 均 `cec` 确认功能等价。

这个结果没有验证"先展开到 K2 再优化能提升 `csr` 收益"这个最初的实验假设——反而印证了 `docs/csr.md` 里已经得出的"K 值对 `csr` 收益影响可忽略"的结论，从另一个角度（固定分区、只变粒度）独立复现了同一个发现。

## 文件结构

- `src/pdecomp/`：新模块，独立 `CMakeLists.txt`（参照 `src/csr/CMakeLists.txt` 的结构：链接 `libabc` + 必要的 timer/kit 依赖）。
  - `pdecomp.hpp`：`Config` 结构体（目前只有 `int K = 2`）+ `ApplyPdecomp` 声明。
  - `pdecomp.cpp`：核心实现——分解递归函数、主流程、断言检查、回滚。
- `src/main.cpp`：新增 `Pdecomp_Command`，注册为 ABC 命令 `pdecomp`，参照 `Csr_Command` 的参数解析风格（`-K num` 校验范围 2-6）。

复用（不修改）：`is_part_stat_vertex`/`ComputeCutEdgeCount`（如果 `pdecomp` 独立成模块，需要决定是从 `csr` 里把这两个函数提出来共享，还是在 `pdecomp` 里重新引用 ABC 层面等价的 `Abc_NtkComputeCutEdgeNum`——`abcPdb.cpp` 已经把这个函数暴露在 ABC 层，`pdecomp` 直接调用它即可，不需要依赖 `csr` 模块，保持模块边界干净）、`Abc_NtkComputeHopNum`、`Kit_TruthCofactor0New`/`Kit_TruthCofactor1New`/`Kit_TruthToHop`（跟 `shannon_decompose_csr` 用的是同一组 kit 函数）。

## 测试与验收

无单元测试框架，验证方式是 build + FoxSYN CLI 运行：

1. **构建**：`make release`，`Built target FoxSYN`，exit 0。
2. **不变量验证**（核心正确性）：在若干 EPFL/MCNC case 上跑 `read x.v; st; if -K 6; hpart -N 4; ps; pdecomp -K 2; ps`，确认分解前后 `hop`/`cut-edge`/`pmax`（分区大小分布）打印值完全相等，节点数（`nd`）显著增加（LUT 被拆成多个小节点）。
3. **cec 功能等价性**：`write before.blif; pdecomp -K 2; write after.blif; cec before.blif after.blif`，确认 `Networks are equivalent`（分解不改变逻辑功能，只改结构）。
4. **实验目的验证**（这是本命令存在的原因，不是本命令自身的验收标准，但值得记录预期）：`pdecomp -K 2` 之后跑 `csr -v`，对比同一分区下"未分解直接 csr" vs "先 pdecomp -K 2 再 csr" 的收益率（多次采样取均值，鉴于 `csr` 收益率本身跟具体 case 相关，且 `csr` 内部的 resub 也有随机性来源需要核实——**这组对比留给后续人工实验，不写进本命令的回归脚本**，跟设计约定"命令本身不写回归脚本"一致）。

## 已知限制 / 后续方向

- 分解只处理 `Abc_ObjFaninNum(pNode) > K` 的节点，K 以内的节点保持不变——不会把已经是 2-input 的节点强行拆得更细（没有意义）。
- 断言失败目前设计为整体报错 + 回滚，不提供"哪个节点导致失败"的诊断信息。实现阶段确实撞上过一次系统性失败（见上文"cofactor 树的 fanout 爆炸"），但根因是全局性的算法缺陷、不是某个节点的个例，加逐节点诊断也定位不到它——所以补上跨分区缓冲节点这个结构性修复之后就没再需要逐节点诊断能力，维持初版不做的决定。
- 若后续证明 `pdecomp -K 2; csr` 确实比直接 `csr` 收益更高，可考虑把这个"先展开再优化"的模式正式吸收进 `csr` 流水线（如作为一个可选的前置 phase），但这是本设计明确不做的后续方向，不在本次范围内。
