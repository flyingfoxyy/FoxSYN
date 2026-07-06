# csr Phase 0 swap 迁移设计

## 背景与动机

`csr` 的 Phase 0(`run_phase0_relocate`,`src/csr/csr.cpp:974`)目前只做**单节点搬家**:对每个节点,若把它单独迁到某个邻居分区能让自身关联跨分区边严格下降(`delta<0`),就迁。这个杠杆抓不到一类结构性场景:

> 节点 A、B 互为邻居且分属不同分区,单独迁 A(或单独迁 B)都不满足 `delta<0`——因为各自还有其他邻居留在原分区把它拉住——但如果 **A、B 同时互换分区**,两者之间那条跨分区边直接消失,加总收益为负。

这是经典 FM / Kernighan-Lin 里的 swap move。单节点贪心的局部最优会卡在这类耦合上:任一单步都不下降,只有成对移动才下降。本设计新增第二个迁移杠杆——**相邻节点对的分区互换**——补上这个盲区。

与单节点迁移一致:纯 `part_id` 重标记,零面积、零逻辑改动。

## 定位与结构

swap 是一个**独立子循环**,接在现有单节点 relocation 循环**收敛之后**运行,仍属于 Phase 0:

```
run_phase0_relocate:
    single-node relocation 循环(现状,不改)  → 收敛
    swap 子循环(本设计新增)                  → 收敛
```

理由:swap 是这次要验证的新杠杆,跟已验证的单节点迁移隔离开,才能干净地测出 swap 单独贡献了多少额外收益。若隔离版收益明显,后续再考虑把两者交替包一层外循环做交叉解锁(本设计不含,列入后续方向)。

两个子循环都复用现有 `-R`(max_rounds)/ `-S`(stall_limit)配置。

## swap 候选

**只枚举互为 fanin/fanout、且分属不同分区的节点对(仅 part_stat 顶点)。**

- 候选集大小正比于跨分区边数,不是 `O(N²)`,大电路上可控。
- 非相邻对(A、B 之间无边)的 swap 不改变它们之间的任何边,其收益恒等于两个独立单节点迁移之和,已由前一个子循环覆盖,不纳入。swap 的独特价值只存在于相邻对。

枚举方式:扫全网每个节点 A 的 fanin/fanout,取其中分区不同于 A 的相邻 part_stat 节点 B,得到候选对 `(A, B)`。同一对会被 A、B 各枚举一次,用 `A->Id < B->Id` 去重。

## 三重门槛

对每个候选对 `(A, B)`,`P_A = part(A)`,`P_B = part(B)`,`P_A != P_B`:

### 1. cut-edge 严格下降(`ComputeCutEdgeCount` 前后差)

试探性地把 `part(A)=P_B`、`part(B)=P_A` 一起改掉,调一次全网 `ComputeCutEdgeCount` 拿新值,与改之前的当前全局值比,`新值 < 旧值` 才继续。

选前后差而非手写局部 delta:swap 同时移动两个节点,A–B 之间的边会被两侧同时统计,手写局部 delta 需对"A、B 间多条边""A 既是 B 的 fanin 又是 fanout"等情况特判去重,易错。`ComputeCutEdgeCount` 前后差跟 Phase 2 replication 的验收方式(`src/csr/csr.cpp:801`)完全一致,已被验证、彻底规避重复计数 bug。代价是每个候选对一次全网 `ComputeCutEdgeCount`(O(edges)),本设计不评估运行时间,可接受。(局部 delta 是有希望的后续优化,列入后续方向。)

apply-time 重验证:FM 式多轮里,前面接受的 swap 会改变后面候选的邻居分区,使预先算好的 delta 过期。所以每个候选对在真正应用前,用当前 `part_id` 重新走一遍前后差,不再 `<0` 就跳过。

### 2. hop 精确不变差

swap 子循环开始时取一次 `baseline_hop = Abc_NtkComputeHopNum(pNtk)`(即单节点 relocation 跑完之后的 hop)。每个通过 cut-edge 门槛的候选,互换后重算全网 `Abc_NtkComputeHopNum`,`> baseline_hop` 就**回滚两个 `part_id`**、拒绝该 swap。

baseline 取 swap 子循环入口 hop(而非整个 csr 的初始 hop):语义上"每个子阶段都不允许 hop 相对自己入口变差",跟单节点迁移门槛同构,保证 swap 不会把单节点迁移已经达到的 hop 状态推坏。接受的 swap 满足 `new_hop <= baseline_hop`,故 baseline 全程不需更新。

### 3. 平衡:swap 天然保持,无需检查

A 从 `P_A→P_B`、B 从 `P_B→P_A`,`sz[P_A]` 净变化 `-1+1=0`,`sz[P_B]` 同理。**任何 swap 都不改变任何分区的大小**,单节点迁移里的 balance 上限门槛在 swap 上自动满足,无需检查——这是 swap 相对单节点迁移的一个优势(单节点迁移会被平衡上限拒绝,swap 不会)。

### 原子性

A、B 的 `part_id` 要么一起改成对方的、要么一起回滚,不存在只改一个的中间态。回滚发生在 hop 门槛失败时:把 `part(A)`、`part(B)` 一起还原。

## 循环结构(FM 式多轮)

```
swap_subloop:
    baseline_hop = Abc_NtkComputeHopNum(pNtk)
    best_cutedges = ComputeCutEdgeCount(pNtk)
    stall = 0
    for round in 0..max_rounds:
        收集所有相邻跨分区候选对 (A,B),A->Id < B->Id 去重
        若无候选 → break
        round_swapped = 0
        for each (A,B):
            若 A/B 已失效(被删/非 node)或已同分区 → skip
            cur = ComputeCutEdgeCount()
            试探: part(A)=P_B, part(B)=P_A
            new = ComputeCutEdgeCount()
            若 new >= cur:                      # cut-edge 门槛(apply-time 重算)
                回滚 → continue
            若 Abc_NtkComputeHopNum() > baseline_hop:  # hop 门槛
                回滚 → continue
            接受(part_id 已是互换后的值), round_swapped++
        total_swaps += round_swapped
        new_cutedges = ComputeCutEdgeCount()
        若 new_cutedges < best_cutedges: best_cutedges=new_cutedges; stall=0
        否则 若 ++stall >= stall_limit: break
        若 round_swapped == 0: break
```

注:上面伪码为清晰把"试探 cut-edge"和"apply-time 重算"合成了一步——真正应用前那次 `ComputeCutEdgeCount` 就用当前 `part_id`,天然是最新的,不存在过期问题(过期问题只在"预先批量算好 delta 再排序应用"的方案里出现;这里逐个候选即算即用,直接规避)。候选对枚举顺序即为处理顺序,不预排序(cut-edge 前后差已是精确增量,无需按 |delta| 排序)。

## 命令接口

**不新增 flag。** swap 子循环跟单节点迁移同属 Phase 0,复用现有 `-L`(`do_relocate`,`src/csr/csr.hpp:20`)总开关一起开关。`-L` 关闭时整个 Phase 0(含 swap)不运行,结果与当前 `main` 一致。

verbose 输出新增 swap 子循环的逐轮行,与单节点迁移的 `csr: phase0 round ...` 风格一致,加 `swap` 标识区分。summary printf 的 `phase0 %d moves` 是否把 swap 计入,见下。

## 文件结构

改动集中在 `src/csr/csr.cpp`:

- 新增静态 helper:枚举相邻跨分区候选对(可复用 `is_part_stat_vertex`)。
- 新增 swap 子循环函数(如 `run_phase0_swap`),或直接把 swap 循环并入 `run_phase0_relocate` 尾部。倾向**独立函数** `run_phase0_swap(pNtk, cfg, int &total_swaps)`,由 `run_phase0_relocate` 在单节点循环收敛后调用,保持单一职责、便于独立读和测。
- summary 计数:`run_phase0_relocate` 现有签名 `int &total_moves`。swap 数单独用 `total_swaps` 统计,verbose 内部打印;是否汇总进 `ApplyCsr` 的 `phase0 %d moves` 需定(见下"待实现时确认")。

复用现有:`is_part_stat_vertex`(`csr.cpp:30`)、`ComputeCutEdgeCount`(`csr.cpp:38`)、`Abc_NtkComputeHopNum`。不需要 cpr 的平衡原语(swap 保平衡)。

## 测试与验收

用已建好的冻结分区消除 hmetis 随机性:`regression/parts_n4_flat/`(88/90 case,两个命名不一致的已知跳过),`scripts/run_csr_regression.py`。

对比两组:
- **swap-off**(仅单节点迁移):当前 `main` 的 relocation-on 结果。已测:81/90 有收益,84 个共同 OK case 总收益 47149→60295。
- **swap-on**(单节点 + swap):本设计。

**验收标准(与单节点迁移一致):**

1. **功能正确性:** 全部 OK case `cec` = EQ。swap 是纯 `part_id` 重标记,逻辑不变,理论上必然 EQ;出现 NOT_EQ 说明实现有 bug。
2. **hop 硬约束:** 每个 OK case swap 后 hop **不大于** swap 前(逐 case 核对)。
3. **cut-edge 收益:** 总 cut-edge 相对 swap-off 进一步下降;有收益的 case 数不低于 swap-off 的 81/90。
4. **平衡:** swap 保平衡,各分区大小与 swap 前一致(理论保证,抽查 pmax 核对)。
5. **无回归:** `-L` 关闭时结果与当前 `main` 完全一致。

**构建:** `make release`,`Built target FoxSYN`,exit 0。

## 待实现时确认(非阻塞)

- **summary 计数展示:** `phase0 %d moves` 是否把 swap 计入,还是新增 `phase0 %d moves / %d swaps`。倾向后者(可分别观察两个杠杆的贡献),实现时定,不影响算法。

## 后续方向(本设计不含)

- **局部 delta 替代全网前后差:** 手写 A/B 关联边变化 + A–B 边去重,把每候选一次 O(edges) 降到 O(deg(A)+deg(B))。正确性验证通过后再优化性能。
- **单节点 + swap 交替到不动点:** 把两个子循环包一层外循环交替跑,让单节点移动打开的新格局立刻被 swap 利用、反之亦然。对应 `csr-relocation-design.md` 的"方向C"。
- **k-way move(k>2):** 三节点及以上的环形分区轮换,收益更稀疏、候选枚举更贵,暂不考虑。
