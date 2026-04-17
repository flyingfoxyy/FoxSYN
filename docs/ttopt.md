# ABC 中 `&ttopt` 命令整理

## 1. `&ttopt` 是什么

`&ttopt` 是 ABC 的一个基于真值表的 AIG 优化命令，属于 `ABC9` 命令组。

相关源码位置：

- 命令注册位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L1348)
- 命令实现入口：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46003)
- 核心算法实现：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1109)

它工作在 ABC 的 `&` 空间，也就是当前的 GIA 网络上。可以把它理解为：

1. 取出当前 GIA 网络中的一小组输出。
2. 为这组输出收集支持变量集合。
3. 计算对应的局部真值表。
4. 对真值表对应的 BDD 变量顺序做重排，尽量减小 BDD。
5. 再根据这个更紧凑的表示重建新的 AIG。
6. 用新网络替换当前网络。


## 2. 命令行参数

ABC 中的 usage 位于：

- [src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46060)

命令格式：

```text
&ttopt [-IORX num] [-vh] <file>
```

参数含义：

- `-I num`
  truth table 计算时允许的输入支持规模上限。
  默认值：`6`
  解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L45983)

- `-O num`
  一次成组处理多少个输出。
  默认值：`2`
  解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L45996)

- `-R num`
  care-set 模式下使用的阈值参数，和仿真模式/稀有模式统计有关。
  默认值：`0`
  解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46006)

- `-X num`
  变量重排优化轮数。
  默认值：`20`
  解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46015)

- `-v`
  verbose 开关。
  解析位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46024)

- `<file>`
  可选仿真信息文件。给了文件就进入 care-aware 模式。
  文件检查和调用分支位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46041)


## 3. 普通模式和 care 模式

命令入口会根据是否提供文件走两个分支：

- 无文件：
  调用 `Gia_ManTtopt(...)`
  调用位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46055)

- 有文件：
  调用 `Gia_ManTtoptCare(...)`
  调用位置：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46053)

对应核心实现：

- 普通模式入口：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1109)
- care 模式入口：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1157)

其中 care 模式会额外读取仿真数据，并据此构造 care 信息，只对 care 条件下真正重要的函数行为做优化。


## 4. `Gia_ManTtopt()` 的主流程

主循环在：

- [src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1109)

可以概括成：

1. 创建新的输出 GIA 管理器。
2. 遍历输出，按 `nOuts` 个输出为一组处理。
3. 收集这一组输出的支持集：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1127)
4. 计算每个输出在该支持集上的 truth table：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1133)
5. 调用 `tt.RandomSiftReo(nRounds)` 做变量重排优化：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1144)
6. 将最优变量顺序应用回 truth table：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1145)
7. 由重排后的表示生成 AIG：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1148)

所以，`&ttopt` 不是直接在原 AIG 上做局部重写，而是：

- 先提取局部功能
- 再在更适合优化的中间表示上做换序
- 最后重新综合回 AIG


## 5. `tt.RandomSiftReo(nRounds)` 到底在做什么

这句代码的含义是：

- 在 truth table 表示上搜索更好的 BDD 变量顺序
- 优化目标是尽量减小 BDD 节点数
- 通过多轮随机起点加局部 sifting 搜索，减少陷入局部最优的概率

对应实现位置：

- `RandomSiftReo(...)`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L396)
- `SiftReo()`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L324)
- 相邻变量交换 `BDDSwap()`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L313)
- 真正执行位级顺序交换的 `Swap()`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L276)


## 6. 为什么变量顺序会影响 BDD 大小

BDD 的构造依赖固定变量顺序。对同一个布尔函数：

- 顺序好时，很多子函数会相同或者互补，能够共享节点
- 顺序差时，原本可以共享的结构被切碎，产生更多不同的残余子函数

因此，BDD 大小对变量顺序非常敏感。

在这份代码里，BDD 大小主要通过以下过程体现：

- 构建节点：`BDDBuildOne(...)`
  位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L213)

- 按层构建：`BDDBuildLevel(...)`
  位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L232)

- 整体构建并统计节点数：`BDDBuild(...)`
  位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L243)

关键点在于：

- 如果两个子函数相同，就能复用
- 如果一个节点的两个 cofactor 相同，这个节点就冗余，可以消去

变量顺序不同，这两件事发生的频率就不同。


## 7. `SiftReo()` 的具体工作方式

`SiftReo()` 是典型的 sifting 式变量重排。

流程如下：

1. 先在当前顺序下构建 BDD：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L325)

2. 统计每一层的节点数，给变量一个处理优先顺序：
   代码位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L333)

3. 对每个变量，执行“上下扫动”：
   - 先向下，每次和相邻变量交换一层
   - 每交换一次就局部重建，并重新统计节点数
   - 如果更优，就保存当前状态
   - 然后恢复，再向上执行同样过程

4. 最后保留这一轮搜到的最好位置

对应代码在：

- [src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L346)

其中真正的“相邻交换并重算代价”是：

- `BDDSwap(i)`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L351)

这说明 `SiftReo()` 不是拍脑袋换顺序，而是每次真的交换变量、重建局部 BDD、比较节点数。


## 8. `RandomSiftReo()` 在 `SiftReo()` 之上加了什么

`SiftReo()` 是局部搜索，有可能卡在局部最优。

所以 `RandomSiftReo(nRounds)` 做的是：

1. 先跑一次普通的 `SiftReo()`：
   位置：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L397)

2. 然后执行 `nRounds` 轮：
   - 先随机打乱一个变量顺序
   - 用 `Reo(vLevelsNew)` 将 truth table 调整到这个随机顺序
   - 再从该顺序出发执行 `SiftReo()`
   - 如果得到更小的 BDD，就保存这个结果

对应代码：

- 生成随机顺序：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L400)
- 应用顺序 `Reo(...)`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L379)
- 保存最优结果：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L411)

因此它更准确的描述是：

- 随机重启 + sifting 精修


## 9. 为什么更小的 BDD 往往能导出更小的 AIG

最后的 AIG 重建使用：

- `BDDGenerateAigRec(...)`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L420)
- `BDDGenerateAig(...)`：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L446)

这里的逻辑大致是：

- 如果某个子函数已经出现过，就直接复用已有节点
- 如果某些分支满足蕴含关系，就构造更简单的 `AND/OR`
- 否则使用 `MUX` 风格节点

因此，一个更小且共享更多的 BDD，通常意味着：

- 独特子函数更少
- 共享结构更多
- 生成出来的 AIG 节点也更少

严格说，`ttopt` 直接优化的目标是 BDD 节点数，不是 AIG 节点数本身；但对于这种局部真值表驱动的方法，BDD 变小往往是非常有效的中间目标。


## 10. 手工例子一：`f(a,b,c,d) = (a & b) | (c & d)`

考虑这个 4 变量函数：

```text
f(a,b,c,d) = (a & b) | (c & d)
```

这个函数本身很简单，但 BDD 大小会随变量顺序变化。

### 顺序 A：`a, b, c, d`

这个顺序通常比较好，因为：

- `a` 和 `b` 是同一个乘积项里的变量
- `c` 和 `d` 也是同一个乘积项里的变量
- 相关变量保持相邻，结构不容易被打散

对 `a` 做 Shannon 分解：

- `a = 0` 时，函数变成 `c d`
- `a = 1` 时，函数变成 `b + c d`

接着看 `a = 0` 这一支。如果下一层是 `b`，由于 `c d` 和 `b` 无关：

- `b = 0` 还是 `c d`
- `b = 1` 还是 `c d`

那么这个 `b` 节点就是冗余的，可以消掉。  
同时，`c d` 这个子函数也容易在别的路径上复用。

因此这个顺序下的 BDD 往往比较紧凑。

### 顺序 B：`a, c, b, d`

这个顺序通常更差，因为它把相关变量拆开了。

还是先按 `a` 分解：

- `a = 0` 时得到 `c d`
- `a = 1` 时得到 `b + c d`

但下一层不再看 `b`，而是先看 `c`。这样会更早地把局部结构切碎：

- 某些分支出现 `0` 和 `d`
- 某些分支出现 `b` 和 `b + d`

这些残余函数彼此不那么容易共享，于是需要更多 BDD 节点。

函数没有变，变化的只有变量顺序，但中间表示大小已经变了。


## 11. 手工例子二：MUX 风格函数

考虑：

```text
f(x,a,b) = x ? a : b
```

等价写法：

```text
f = x a + x' b
```

### 顺序 A：`x, a, b`

如果 `x` 放在顶层：

- `x = 0` 时直接得到 `b`
- `x = 1` 时直接得到 `a`

于是根节点下面就是两个很简单的子函数，BDD 往往很小。

### 顺序 B：`a, b, x`

如果把 `x` 放后面，前面的分解会先落在数据变量上，而不是选择变量上。这样中间往往会形成更多“混合残余函数”，比顺序 A 更不容易共享。

这类函数说明：

- 真正充当 selector 的变量，放在前面通常有利

但这个结论只对 mux/条件分派结构特别直观，不是对所有函数都成立。


## 12. “控制变量放在 BDD 顶层是不是总最好”

答案是不是。

更准确的说法是：

- 对明显的 MUX 或条件分派函数，选择信号放前面通常比较好
- 但对一般布尔函数，没有一条普适规则说“控制变量总该排最前”

真正影响顺序优劣的是：

- 提前分解某变量后，得到的 cofactors 是否更简单
- 这些 cofactors 是否更容易共享
- 强相关变量是否靠得更近

因此更可靠的经验是：

- 真正起 selector 作用的变量，常常适合靠前
- 强相关变量，常常适合相邻
- 最终顺序质量必须靠代价函数测出来，而不是只凭直觉判断

这也正是 `ttopt` 要做搜索而不是写死规则的原因。


## 13. 总结

`&ttopt` 的核心思想不是“直接改网表拓扑”，而是：

1. 从局部逻辑提取 truth table。
2. 通过变量重排减小中间的 BDD 表示。
3. 再从更紧凑的 BDD 生成新的 AIG。

所以“通过重新排列变量顺序来优化网表”的准确含义是：

- 先优化决策图表示
- 再从优化后的决策图导出更紧凑的逻辑网表

从源码看，整个流程的关键位置是：

- 命令注册：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L1348)
- 命令实现：[src/abc/src/base/abci/abc.c](/home/longfei/FoxSYN/src/abc/src/base/abci/abc.c#L46003)
- 主优化入口：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1109)
- care 模式入口：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L1157)
- BDD 构建：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L243)
- 变量交换：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L313)
- sifting：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L324)
- 随机重启：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L396)
- AIG 重建：[src/abc/src/aig/gia/giaTtopt.cpp](/home/longfei/FoxSYN/src/abc/src/aig/gia/giaTtopt.cpp#L420)

