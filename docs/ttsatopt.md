# ABC 中 `&ttsatopt` 命令说明

本文整理当前实现的 `&ttsatopt` 命令，包括：

- 命令入口
- 参数含义
- 整体算法流程
- 关键数据结构与统计信息
- 与 `&ttopt` 的本质区别
- 当前实现边界


## 1. `&ttsatopt` 是什么

`&ttsatopt` 是一个工作在 ABC `&` 空间中的新命令，属于 `ABC9` 命令组。

相关源码位置：

- 命令注册位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L1349)
- 命令实现入口：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46090)
- 参数结构体定义：[src/abc/src/aig/gia/gia.h](/home/longfei/FoxSYN/src/abc/src/aig/gia/gia.h#L319)
- 核心算法实现入口：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L684)

它的目标不是像 `&ttopt` 那样对局部 truth table 做变量重排和重综合，而是：

1. 在当前 GIA 中选取一组输出根节点。
2. 为这些根节点构造局部逻辑窗口。
3. 在窗口中收集候选信号并生成一些简单候选表达式。
4. 用随机仿真快速筛掉明显不匹配的候选。
5. 对通过筛选的候选做严格 CEC 验证。
6. 只有当替换后局部 AIG `and` 数更少且 `level` 不变差时，才接受该替换。

因此，当前版本的 `&ttsatopt` 更准确地说是：

- simulation-guided local AIG rewrite
- SAT/CEC-checked exact replacement

而不是完整的 truth-table-based resynthesis。


## 2. 命令入口和使用方式

命令入口在：

- [src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46090)

usage 为：

```text
&ttsatopt [-WROILNDGCT num] [-rvh]
```

它要求当前网络已经进入 `&` 空间，并且必须是组合 GIA。  
如果网络中有寄存器，命令会直接拒绝：

- 入口处检查：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46223)
- 核心实现中再次检查：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L690)

典型调用方式：

```abc
read alu4.v
strash
&get
&ttsatopt -R 5 -O 1 -C 20000 -T 120 -v
&ps
```


## 3. 参数说明

参数结构体定义在：

- [src/abc/src/aig/gia/gia.h](/home/longfei/FoxSYN/src/abc/src/aig/gia/gia.h#L319)

命令解析逻辑在：

- [src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46110)

各参数含义如下。

### 3.1 `-W num`

对应参数：

- `nSimWords`

含义：

- 随机仿真使用的 machine word 数。
- 每个 PI 总共会有 `64 * nSimWords` 个仿真 bit。
- 数值越大，仿真误匹配越少，但运行时间和内存开销更高。

默认值：

- `8`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46096)


### 3.2 `-R num`

对应参数：

- `nOuterRounds`

含义：

- 外层优化轮数。
- 每一轮都会基于当前 GIA 再跑一遍完整的窗口优化 pass。
- 如果某一轮没有接受任何替换，会提前停止，不会硬跑满所有轮。

默认值：

- `1`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46097)
- 外层多轮逻辑：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L692)


### 3.3 `-O num`

对应参数：

- `nOuts`

含义：

- 每一组同时优化多少个输出根节点。
- 当前实现会按 support overlap 的贪心策略对 root 分组。

默认值：

- `1`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46098)
- 分组逻辑：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L320)


### 3.4 `-I num`

对应参数：

- `nInputsMax`

含义：

- 局部窗口边界输入数上限。
- 如果窗口扩展后边界输入超过这个数，则该窗口放弃。

默认值：

- `16`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46099)
- 窗口上限检查：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L401)


### 3.5 `-L num`

对应参数：

- `nLevelsMax`

含义：

- 窗口向上扩展的逻辑层数上限。

默认值：

- `8`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46100)
- 窗口构造调用：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L389)


### 3.6 `-N num`

对应参数：

- `nWindowNodesMax`

含义：

- 窗口内部节点数上限。
- 用来控制局部问题规模。

默认值：

- `150`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46101)
- 超限检查：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L401)


### 3.7 `-D num`

对应参数：

- `nDivsMax`

含义：

- 结构候选除数上限。
- 当前实现会从窗口池中挑选一批结构上更值得优先尝试的候选节点。

默认值：

- `32`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46102)
- 结构池筛选：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L164)


### 3.8 `-G num`

对应参数：

- `nSigCandsMax`

含义：

- 预留给“签名候选数”的参数。

当前状态：

- 这个参数已经进了命令接口和参数结构体。
- 但当前实现里没有独立成型的 signature-ranked candidate pool。
- 现在它主要通过 `nCandPerRootMax` 间接影响每个 root 最多保留多少候选。

默认值：

- `64`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46103)
- 与候选上限的关系：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46184)


### 3.9 `-C num`

对应参数：

- `nBTLimit`

含义：

- 每次局部 CEC 调用允许的 SAT 冲突上限。
- 值越大，证明通过的候选可能更多，但代价更高。

默认值：

- `1000`

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46105)
- CEC 参数传递：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L471)


### 3.10 `-T num`

对应参数：

- `TimeLimit`

含义：

- 全局运行时间限制，单位秒。
- 每轮 pass 内部会检查是否超时。
- 外层多轮优化会使用剩余时间，而不是每轮重新给满。

默认值：

- `0`
- `0` 表示不限制。

代码位置：

- 默认值初始化：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46106)
- pass 内检查：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L633)
- 外层剩余时间处理：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L695)


### 3.11 `-r`

对应参数：

- `fReuseSims`

含义：

- 如果当前 GIA 已经保存了 PI 仿真模式 `vSimsPi`，则复用这些模式。
- 否则重新随机生成仿真输入。

默认值：

- 关闭

代码位置：

- 解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46205)
- 复用逻辑：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L619)


### 3.12 `-v`

对应参数：

- `fVerbose`

含义：

- 打印优化统计信息和计时信息。

代码位置：

- 解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46208)
- 统计打印位置：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L651)


## 4. 总体算法流程

当前实现可以概括为以下流程：

1. 从 PO driver 中选择待优化 root。
2. 按 support overlap 将 root 分组。
3. 为每一组 root 构造局部窗口。
4. 在窗口中收集候选节点池。
5. 为每个 root 生成一批简单候选表达式。
6. 用仿真 signature 做快速精确匹配筛选。
7. 对每个 root 仅保留局部 area 更小、level 不变差的候选。
8. 对这些候选做局部 CEC 验证。
9. 如果整组替换后 area 改善且 level 不变差，再做组级 CEC。
10. 将所有接受的窗口统一插回原网表。

对应主 pass 实现：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L595)

外层多轮迭代实现：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L684)


## 5. 第一步：目标 root 的选择与分组

目标 root 的选择在：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L352)

当前实现只收集：

- PO 的 driver
- 且该 driver 是 AND 节点

逻辑见：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L359)

分组方法是贪心的：

1. 以一个 root 为起点。
2. 在剩余 root 中找 support overlap 最大的那个。
3. 直到达到 `-O` 指定的组大小，或再也找不到 overlap 大于 `0` 的 root。

相关代码：

- support overlap 分组逻辑：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L320)
- 组收集函数：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L352)

这说明当前多输出处理不是复杂聚类，而是启发式分组。


## 6. 第二步：窗口构造

窗口构造在：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L380)

当前做法是：

1. 对组中每个 root 调用 `Gia_RsbWindowCompute()`。
2. 把得到的局部窗口和输入边界做并集。
3. 检查并集后的窗口是否超过 `-I` 和 `-N` 的限制。

调用位置：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L389)

如果窗口过大，就直接放弃这一组，不做后续优化。


## 7. 第三步：候选池收集

窗口建好以后，会收集一个节点池：

1. 窗口输入节点
2. 窗口内部节点
3. 排除当前组自己的 root

对应代码：

- 候选池收集：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L191)

然后会从这个池中再选一批“结构上更重要”的候选节点：

- 主要基于 level 分数
- 只保留前 `nDivsMax` 个

对应代码：

- 结构候选筛选：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L164)


## 8. 第四步：随机仿真

仿真发生在每轮 pass 的前半段：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L619)

流程是：

1. 如果 `-r` 开启且当前 GIA 已保存 `vSimsPi`，则复制复用。
2. 否则，使用 `Vec_WrdStartRandom()` 为所有 PI 生成随机输入模式。
3. 再调用 `Gia_ManSimPatSimOut()` 计算全网所有节点在这些模式下的 signature。

对应代码：

- 随机生成 PI 仿真模式：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L623)
- 计算节点 signature：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L627)

这里得到的是 bit-parallel simulation signature，不是严格真值表。


## 9. 第五步：候选表达式生成

候选生成在：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L203)

这是当前命令最重要、也最能说明其能力边界的一部分。

当前实现只枚举以下几类表达式：

- `LIT`
- `AND`
- `OR`
- `XOR`
- `MUX`

更具体地说：

- `LIT`
  允许使用任意池中节点，且允许取反。

- `AND/OR/XOR`
  从结构候选池中枚举两输入组合，允许输入分别取反。

- `MUX`
  从结构候选池中枚举三个节点，允许控制端和两个数据端分别取反。

保留候选的条件不是“看起来可能等价”，而是：

- 在当前随机仿真模式下
- 候选表达式的 signature 和目标 root 的 signature 完全一致

相关代码：

- literal 匹配：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L214)
- 二输入候选匹配：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L235)
- MUX 候选匹配：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L282)

这里的 `exact` 统计，表示的是：

- 在当前仿真模式上完全匹配

它不表示已经由 SAT/CEC 证明严格等价。


## 10. 第六步：局部窗口 AIG 构造

一旦某个候选表达式通过仿真精确匹配筛选，就会被放入局部窗口里构造新的局部 GIA。

对应代码：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L415)

具体做法：

1. 窗口输入 `vIns` 变成新 GIA 的 CI。
2. 原窗口内部的 AND 节点照原结构复制。
3. 组中每个 root 用候选表达式重建对应输出。

这一阶段只是形成“原窗口”和“候选窗口”的本地比较对象。


## 11. 第七步：单 root 候选预筛

对每个 root，当前实现先在单输出窗口上比较：

- 候选局部 AIG 的 `and` 数
- 候选局部 AIG 的 `level`

筛选条件为：

- `CandArea < BestArea`
- `CandLevel <= BestLevel`

代码位置：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L537)

也就是说，当前命令真正优化的目标是：

- AIG 层面的 area-first
- 同时约束 level 不变差

如果候选连这道门槛都过不了，就不会进入 CEC。


## 12. 第八步：CEC 严格验证

对通过局部 area/level 预筛的候选，会构造 miter 并调用 `Cec_ManVerify()` 做严格验证。

代码位置：

- CEC 包装函数：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L465)
- miter 构造：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L476)
- 调用 `Cec_ManVerify()`：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L481)

返回结果的含义：

- `1`
  证明等价，验证通过

- `-1`
  未定，通常意味着冲突上限或时间限制导致没证完

- 其他
  视为失败，不接受


## 13. 第九步：组级接受条件

这是当前实现的另一个关键点。

即使某些 root 的单输出候选通过了预筛和 CEC，也不会立刻写回。  
实现会先为组中每个 root 分别选一个当前最优候选，然后重新构造整组的候选窗口，再做一次组级检查。

代码位置：

- 组级构造和接受逻辑：[src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L564)

整组替换必须同时满足：

1. `Gia_ManAndNum(pCandGroup) < OrigArea`
2. `Gia_ManLevelNum(pCandGroup) <= OrigLevel`
3. `Ttsat_VerifyWindows(...)` 返回通过

只有这样，才真正把这组窗口加入待回插列表。


## 14. 第十步：窗口回插

每轮 pass 中所有接受的窗口，最后统一调用：

- `Gia_ManDupInsertWindows(...)`

对应代码：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L639)

这样会生成一个新的 GIA 作为本轮优化结果。


## 15. 外层多轮优化

外层多轮逻辑在：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L684)

其行为是：

1. 最多迭代 `nOuterRounds` 轮。
2. 每轮以当前 GIA 为输入，跑一次完整 pass。
3. 如果某轮没有任何窗口被接受，就提前停止。
4. 如果指定了 `-T`，则后续轮次只能使用剩余时间。

这意味着：

- `-R` 不是简单重复同一轮计算
- 而是在“上一轮已修改后的网表”基础上继续找新机会


## 16. verbose 输出如何理解

verbose 打印位置：

- [src/abc/src/aig/gia/giaTtsatopt.c](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtsatopt.c#L651)

输出字段说明如下：

- `groups`
  本轮处理的 root 分组数

- `roots`
  本轮一共处理了多少个 root

- `accepted`
  最终真正接受的窗口组数

- `exact`
  仿真 signature 上完全匹配的候选数量

- `cec`
  进入严格 CEC 的候选数量

- `pass`
  CEC 成功证明通过的候选数量

- `undec`
  CEC 未定的候选数量

- `gain(and)`
  接受替换后累计减少的 AND 数

- `gain(lev)`
  接受替换后累计降低的 level 数

例如：

```text
ttsatopt: groups = 8 roots = 8 accepted = 0 exact = 289 cec = 0 pass = 0 undec = 0 gain(and) = 0 gain(lev) = 0
```

表示：

- 仿真上找到了很多完全匹配的候选
- 但没有一个通过 area/level 预筛
- 因此没有候选进入 CEC
- 最终也没有任何替换被接受


## 17. 与 `&ttopt` 的本质区别

虽然命令名里也有 `tt`，但 `&ttsatopt` 和 `&ttopt` 不是同类算法。

`&ttopt` 的特点：

- 先计算局部完整 truth table
- 再做 BDD 变量重排
- 最后根据重排后的函数表示重建 AIG

相关入口：

- [src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1109)

`&ttsatopt` 的特点：

- 不做完整 truth table 重综合
- 只枚举少量简单表达式
- 先靠仿真精确匹配筛选
- 再靠 CEC 保证严格等价
- 优化目标是局部 AIG `and` 数和 `level`

因此：

- `&ttopt` 更像函数级重构
- `&ttsatopt` 更像结构保持较强的局部重写


## 18. 当前实现的适用场景

当前版本更适合：

- 局部逻辑本来就接近简单 `LIT/AND/OR/XOR/MUX` 表达式
- 希望安全地消掉一些 AIG 冗余
- 更关心 AIG 层面的 `and/lev`

它不太适合：

- 需要深层函数重构的逻辑
- 强依赖变量重排才能变好的逻辑
- 直接以 LUT mapping QoR 为主目标的优化

特别是算术/XOR-rich 电路，很多时候：

- `&ttopt` 会显著改善 LUT mapping
- 而当前 `&ttsatopt` 可能几乎没有收益

原因不是命令失效，而是目标函数不同。


## 19. 当前实现的边界和未完成项

当前命令已经可用，但仍然不是“完整计划版”。

已经实现的部分：

- 新命令入口
- 参数结构体
- 多轮外层迭代
- 多输出分组
- 窗口构造
- 随机仿真
- 局部候选生成
- 严格 CEC 验证
- 仅在 area 改善、level 不变差时接受替换

尚未完整实现的部分：

- `-G` 所代表的真正 signature-ranked candidate pool
- 完整 truth-table resynthesis
- 变量重排
- care-aware 模式
- LUT-aware 接受准则

因此，当前版本更准确的定位应是：

- simulation + SAT guided local AIG rewrite

而不是：

- full truth-table driven global/local resynthesis


## 20. 一个实用的理解方式

可以把 `&ttsatopt` 理解为一个三层过滤器：

1. `仿真层`
   用随机模式快速筛掉绝大多数不匹配候选

2. `代价层`
   只保留局部 AIG `and` 更少、`level` 不变差的候选

3. `证明层`
   用 CEC 严格确认功能等价

这三层决定了它的特点：

- 很稳健
- 不会轻易引入错误替换
- 但搜索空间明显受限
- 因而对某些网表的 QoR 改善会比较有限


## 21. 小结

当前 `&ttsatopt` 的核心思想是：

- 在局部窗口中
- 用仿真做快筛
- 用 CEC 做严证
- 只接受 AIG area 改善、level 不变差的替换

它不是 `&ttopt` 的直接替代品，而更像一个补充性的局部安全优化器。

如果后续要让它在更多 case 上真正追上 `&ttopt` 的收益，最值得优先增强的方向是：

- 引入更强的候选生成机制
- 增加 truth-table 或更深层函数级重构
- 将接受准则从纯 AIG area 改成 LUT-aware QoR
