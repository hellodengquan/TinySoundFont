# 延音踏板下同键重触的释音时序分析（TinySoundFont / tsf.h）

在踩下延音踏板（CC64）期间连续弹同一个琴键，两次击键产生的采样层会同时发声。本文结合 `tsf.h` 的实现说明：每个按键/松键事件究竟作用于哪些仍在响的声部（voice），以及踏板抬起前后各组声音何时进入释放段（release）、哪些尾音会出现在之后的输出音频里。

## 0. 关键状态：每个声部携带的三个字段

`struct tsf_voice`（tsf.h:453-458）中与本文相关的字段：

| 字段 | 含义 |
|---|---|
| `playIndex` | 一次 note-on 的"组号"。`tsf_note_on` 开头取 `voicePlayIndex = f->voicePlayIndex++`（tsf.h:1564），本次按键触发的**所有**匹配区域（region）的声部共享同一个 `playIndex`（tsf.h:1617）。 |
| `heldSustain` | 踏板标记：松键时踏板踩着，则不进入释放段，只打上此标记（tsf.h:1962）。 |
| `ampenv.segment` | 音量包络所处段（`TSF_SEGMENT_*`，tsf.h:387）。`>= TSF_SEGMENT_RELEASE` 表示已在释放段或已结束。 |

另有一个通道级状态 `channel.sustain`（tsf.h:466），由 CC64 设置（tsf.h:2005）。

**核心结论**：一次"击键"= 一个 `playIndex` 组（可能含多个采样层/声部）；一次"松键"只作用于该键上**尚未释放、也未被踏板扣留**的、`playIndex` 最小的那一组。被踏板扣留的组对后续松键"隐身"，这就是两次击键的采样层会叠在一起响的原因。

## 1. 演奏记录中按键/松键与仍在响声部的对应关系

### 1.1 按键（note-on）

`tsf_channel_note_on` → `tsf_note_on`（tsf.h:1553）：

- 取一个新的 `playIndex`；
- 对每个匹配键位/力度的 region 分配一个声部（无空闲声部且设了 `maxVoiceNum` 时，抢走释放段走得最远的声部，tsf.h:1584-1596；否则扩容，tsf.h:1598-1609）；
- 这些声部 `playIndex` 相同、`heldSustain = 0`（tsf.h:1617-1618），包络从 DELAY/ATTACK 开始。

注意：`tsf_note_on` **不会**停掉同键已有的声部（除非 region 设置了 `group` 互斥组，tsf.h:1571-1575）。同键重触纯粹是"再加一组"。

### 1.2 松键（note-off）

`tsf_channel_note_off`（tsf.h:1943-1967）分两步：

**第一步：圈定目标组。** 遍历声部，跳过以下声部（tsf.h:1950）：

- 不属于本通道/本键的；
- `ampenv.segment >= TSF_SEGMENT_RELEASE`（已在释放段，松键不重复作用）；
- **`heldSustain != 0`（被踏板扣留的——这是重触分析的关键）**。

在剩下的候选里取 `playIndex` 最小者，确定区间 `[vMatchFirst, vMatchLast]`（同一次按键可能因声部表扩容而不连续，故记录首末指针，tsf.h:1951-1952）。

**第二步：按踏板状态处理该组（tsf.h:1955-1964）。**

- 踏板**踩着**（`channel.sustain != 0`）：不释放，只置 `v->heldSustain = 1`。包络停留在 SUSTAIN 段，若 region 是 `TSF_LOOPMODE_SUSTAIN` 还会一直循环。
- 踏板**没踩**：对该组每个声部调 `tsf_voice_end`（tsf.h:1183-1197），包络立即从 SUSTAIN 段推进到 RELEASE 段；若是延音循环采样，则把 `loopEnd = loopStart`，让循环停止、采样继续播到自然结尾。

> 旁注：不走通道的 `tsf_note_off`（tsf.h:1670-1687）逻辑相同但**不检查** `heldSustain`（它也没有踏板概念）；踏板行为只存在于 `tsf_channel_*` 这条路径。

### 1.3 从一段重叠的同音演奏反推每组声音的去向

设踏板全程踩着，对键 K 的演奏记录为：`按下① → 松开① → 按下② → 松开② → …`。任意时刻仍在响的声部可这样归组：

1. **按下Ⓝ** 产生第 N 组声部（`playIndex = N` 次按键对应的递增值），包络从头部开始。
2. **松开Ⓝ** 时，第 1…N-1 组要么已在释放段、要么 `heldSustain=1`，全部被第一步的过滤条件跳过；唯一"合格"的是第 N 组，于是它被打上 `heldSustain=1`。
3. 因此**每一组声音的去向是确定的**：第 N 次击键的采样层，由第 N 次松键（或之后的踏板抬起，见第 2 节）决定其释放时刻。松键事件与击键组按顺序一一对应——第 N 个松键永远落在第 N 个击键组上，绝不会"提前"释放更早的组。
4. 若某次松键时该键没有任何合格声部（例如所有组都已被踏板扣留），`vMatchFirst` 为空，直接返回（tsf.h:1954），该松键不产生任何效果。

读一段重叠演奏记录时，只要按时间顺序把"按下"编号、把"松开"依次配给编号最小且尚未配对的组，就能推出每组声音此刻是"被踏板扣留的延音"还是"已进入释放段的尾音"。

## 2. 踏板状态改变前后各组声音的释放时刻

### 2.1 踏板踩下（CC64 ≥ 64）

`tsf_channel_set_sustain(f, ch, 1)`（tsf.h:1915-1922）：只置 `channel.sustain = 1`，**不触碰任何声部**。代码注释原文："Turning on sustain does no action now, just starts note_off behaving differently"。此刻所有已在响的组（无论在 SUSTAIN 还是 RELEASE 段）状态不变。

### 2.2 踏板踩着期间的松键

如 1.2 所述：目标组不打释放包络，只置 `heldSustain=1`。这些组**停留在 SUSTAIN 段**，音量保持 sustain 电平，延音循环采样继续循环——它们会完整出现在随后输出的音频中，听感上就是"声音被踏板留住"。

### 2.3 踏板抬起（CC64 < 64）——释放的集中发生点

`tsf_channel_set_sustain(f, ch, 0)`（tsf.h:1923-1928）：遍历本通道所有声部，对满足

```
playingPreset != -1 && playingChannel == channel
&& ampenv.segment < TSF_SEGMENT_RELEASE && heldSustain
```

的每一个声部调用 `tsf_voice_end`。即：**踏板抬起的那一刻，本通道所有被扣留的组（无论来自第几次击键）在同一时刻集体进入 RELEASE 段**。

释放段的参数（`tsf_voice_envelope_nextsegment` 的 SUSTAIN→RELEASE 分支，tsf.h:1089-1103；时长计算 tsf.h:1005-1008）：

- 释放时长 = `region->ampenv.release` 秒；若 ≤ 0 则用 `TSF_FASTRELEASETIME = 0.01s`（tsf.h:275）；
- 音量包络为指数衰减，斜率 `-9.226 / 释放采样数`（约 -100 dB，tsf.h:1095-1098），从当前电平（通常是 sustain 电平）衰减到零；
- 延音循环（`TSF_LOOPMODE_SUSTAIN`）的声部同时被设置 `loopEnd = loopStart`（tsf.h:1190-1193），循环停止，采样顺播到 `region->end` 自然结束。

**不受踏板抬起影响的声部**：键仍物理按着（从未收到松键、`heldSustain=0`）的组继续正常延音；已在 RELEASE 段的组按各自原来的释放曲线继续走。

### 2.4 哪些尾音会出现在随后的输出音频中

踏板抬起后，输出音频是以下成分的叠加：

1. **所有刚被释放的扣留组的释放尾音**——第 1、2、…、N 次击键的采样层各自从 sustain 电平做指数衰减，彼此叠加，直到包络走完全部释放采样数进入 `TSF_SEGMENT_DONE`；
2. **仍被手指按住的组**的完整延音；
3. **此前已在释放段的组**的剩余尾音。

声部的生命终点在渲染循环里判定（`tsf_voice_render`，tsf.h:1350-1354）：当包络到达 `TSF_SEGMENT_DONE`，或采样位置越过 `region->end`，调 `tsf_voice_kill` 置 `playingPreset = -1`，该声部从混音中消失、槽位可被复用。

两个会改变上述"理想时序"的边界情况：

- **声部抢夺**：若用 `tsf_set_max_voices` 限定了声部数，新按键会抢走"释放段走得最远"的声部（`tsf_voice_kill` 直接静音，tsf.h:1584-1596），被抢的尾音**不会**出现在后续音频中；
- **通道级强制消息**：CC123（All Notes Off）→ `tsf_channel_note_off_all`（tsf.h:1969-1975）无视踏板，把本通道所有未释放声部（含 `heldSustain` 的）立即送入释放段；CC120（All Sound Off）→ `tsf_channel_sounds_off_all`（tsf.h:1977-1983）用 `tsf_voice_endquick` 把释放时长强制为 0，尾音只剩 `TSF_FASTRELEASETIME`（10 ms）的急速淡出。

### 2.5 完整时间线示例

踏板踩着弹两次同一个键（假设采样带延音循环、release = 0.4 s）：

| 时刻 | 事件 | 第 1 组 | 第 2 组 |
|---|---|---|---|
| t0 | 按下① | 起音→延音循环 | — |
| t1 | 松开① | `heldSustain=1`，继续循环发声 | — |
| t2 | 按下② | （不变，继续响） | 起音→延音循环 |
| t3 | 松开② | （被跳过） | `heldSustain=1`，继续循环发声 |
| t4 | 踏板抬起 | 进入 RELEASE，停止循环，0.4 s 指数尾音 | 同时进入 RELEASE，0.4 s 指数尾音 |
| t4+0.4s 起 | — | 包络 DONE，声部被 kill | 包络 DONE，声部被 kill |

t1–t4 之间输出的是两组延音的叠加（即"两次击键的采样层同时发声"）；t4 之后输出的是两组各自 0.4 s 的释放尾音的叠加，直到先后（此处同时）衰减完毕。
