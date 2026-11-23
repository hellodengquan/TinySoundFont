# TinySoundFont 声部预算（Voice Budget）诊断报告

排查对象：`tsf.h`（单文件 SoundFont 2 合成器，当前 HEAD `a3621a1`）。
所有结论均来自源码，并由 `tsf.h` 行号与可复现实验（见文末附录）佐证。实验用的
SoundFont / MIDI 与脚本不在本仓库内，位于独立的分析目录，避免污染工程。

---

## 0. 术语与数据模型

| 概念 | 代码实体 | 说明 |
|---|---|---|
| 区域 region | `struct tsf_region` (`tsf.h:426`) | 一个采样区，带键位/力度范围、`group`（独占类）、包络等 |
| 声部 voice | `struct tsf_voice` (`tsf.h:451`) | 一个 region 的一次发声实例；声部槽 `playingPreset == -1` 表示空闲 (`tsf.h:1177`) |
| 预设 preset | `struct tsf_preset` (`tsf.h:443`) | 含若干 region；**一次 `tsf_note_on` 会为每个匹配 region 各启动一个声部** (`tsf.h:1564-1567`) |
| 独占类 | `region->group`，SF2 生成器 57 *ExclusiveClass* (`tsf.h:612,631`) | `group != 0` 时同类互斥；`group == 0` 表示不参与互斥 (`tsf.h:1570`) |
| 声部预算 | `f->maxVoiceNum` (`tsf.h:339`) | 0 = 不设上限、动态扩容；>0 = 预分配并封顶 |
| 发声序号 | `f->voicePlayIndex` / `v->playIndex` (`tsf.h:340,1617`) | 每次 `note_on` 自增一次 (`tsf.h:1563`)，同一击键触发的多个声部共享一个 playIndex |

一个 MIDI 击键产生多少声部，取决于命中的 region 数：键盘分层（key split）通常命中 1 个；
但**力度分层、立体声配对、分层镲片等预设会一次命中 2 个以上 region**，即一次击键占用多个声部槽。
本仓库自带的 `examples/florestan-subset.sf2` 中每个键位只命中 1 个 region 且全部 `group=0`
（见附录 A 的解析结果），所以复现互斥/分层行为需要使用带 ExclusiveClass 的字体。

---

## 1. 新击键能否完整发声：两条独立的淘汰路径

`tsf_note_on` (`tsf.h:1553-1658`) 对**每个匹配 region 独立**走一遍下面的流程。
两条路径性质完全不同，必须分开理解：

```
对每个匹配 region（tsf.h:1564）：
  1) 扫全部声部槽（tsf.h:1569-1576）
     ├─ region->group != 0：同 preset 且同 group 的旧声部 → tsf_voice_endquick() 【路径①：音色互斥】
     │                      同时记下遇到的第一个空闲槽
     └─ region->group == 0：找第一个空闲槽即停
  2) 没有拿到槽（tsf.h:1578）：
     ├─ maxVoiceNum > 0（封顶模式，tsf.h:1580-1600）
     │    在所有 RELEASE 段声部里，挑“释放得最久”的那个 → tsf_voice_kill() 硬杀 【路径②：复音额度】
     │    一个 RELEASE 声部都没有 → continue，本 region 静默放弃（该击键缺一个声层）
     └─ maxVoiceNum == 0（不限模式，tsf.h:1601-1611）
          voiceNum += 4 重新扩容，永不杀声部（realloc 失败才返回 0）
  3) 拿到槽后初始化包络/LFO/滤波器，新声部从 ATTACK 开始（tsf.h:1614-1655）
```

### 路径① 音色自身的互斥规则（ExclusiveClass，开镲/闭镲共用组）

关键代码 `tsf.h:1570-1575`：

```c
if (region->group)
{
    for (; v != vEnd; v++)
        if (v->playingPreset == preset_index && v->region->group == region->group)
            tsf_voice_endquick(f, v);
        else if (v->playingPreset == -1 && !voice) voice = v;
}
```

要点：

1. **作用域是「同 preset + 同 group」**。不同预设（例如另一套鼓）即使 group 编号相同也互不影响；
   `group==0` 直接走另一条分支，普通钢琴/贝斯等旋律乐器天然不互斥。
2. 处置方式是 `tsf_voice_endquick` (`tsf.h:1198-1209`)：把释放时间强制改成 **0 秒**并让幅度包络
   立即进入 RELEASE 段，**不是**立刻把槽标记为空闲。旧声部还会再响一个最短淡出——
   释放时长取 `TSF_FASTRELEASETIME = 0.01f`（10 ms，`tsf.h:275`，经
   `tsf_voice_envelope_release_samples` `tsf.h:1005-1008` 换算成采样数）。
   渲染线程随后在 `tsf_voice_render` 里发现包络到 DONE 或采样播完，才调用
   `tsf_voice_kill` 把槽释放 (`tsf.h:1350-1354`)。
3. 因此开镲被闭镲打断时，听感是**尾音被快速收掉（约 10 ms 淡出）而不是瞬间消失**；
   若采样本身很短（闭镲），淡出可能比采样剩余长度还短，表现为自然终止。
4. **同一次 `note_on` 内，被 endquick 的旧声部马上变成路径②的候选牺牲品。**
   因为 endquick 后它已处于 RELEASE 段；若本击键后续 region 找不到空闲槽，
   抢占扫描会直接杀掉它复用槽位（附录 B 实验 3 验证）。
5. 扫描与找空闲槽在**同一个循环**里完成：互斥处理不会“预留”槽位，只是顺带记录第一个空闲槽。

### 路径② 复音额度（maxVoiceNum）造成的抢占与丢音

关键代码 `tsf.h:1580-1611`，只在封顶模式（调用过 `tsf_set_max_voices`，`tsf.h:1540`）下存在：

1. **只抢 RELEASE 段的声部**，且选释放进度最深者
   （`releaseSamplesDone` 最大，`tsf.h:1583-1596`）——即优先牺牲已经在淡出、最不重要的声音。
   正在 ATTACK/HOLD/DECAY/SUSTAIN 的声部**不会被抢占**，不管它多老。
2. 抢占是 `tsf_voice_kill` (`tsf.h:1175-1178`)：仅置 `playingPreset = -1`，**无淡出、硬切**，
   槽立刻被新声部覆盖。这与路径①的 10 ms 柔化终止有本质区别。
3. **没有 RELEASE 候选时整个 region 被 `continue` 跳过** (`tsf.h:1597-1598`)：
   函数仍返回 1（成功），但该声层根本没启动。多 region 击键因此可能**只响一部分**
   （例如立体声镲片只剩一边、分层音色只剩一个采样层），且调用方拿不到任何错误码。
4. 抢占按 region 逐个发生、先到先得：一次多声部击键内部，前面的 region 抢到的槽
   不会再让给后面的 region（附录 B 实验 2 中 budget=4 时钢琴击键 3 个 region 只抢到 1 个槽）。
5. 不封顶时（默认）走 `voiceNum += 4` 的 realloc 扩容 (`tsf.h:1605`)，理论上不丢音，
   代价是渲染线程遍历的声部数无界增长、CPU 与内存上升；头文件注释也建议在实时线程外
   预先 `tsf_set_max_voices`，避免播放中 realloc (`tsf.h:135-140`)。

### 两类原因的判定边界（回答“由哪些旧声部让出位置”）

| 观察到的现象 | 路径①独占类 | 路径②复音额度 |
|---|---|---|
| 触发条件 | 新 region 的 `group != 0`，且存在同 preset 同 group 的旧声部 | 预算封顶且无空闲槽 |
| 被处理的旧声部 | **仅**同组声部；其他组/其他预设一律不动 | 任意预设中 RELEASE 进度最深者；无 RELEASE 则不处理、改丢新音 |
| 终止方式 | `endquick`：release=0，约 10 ms 淡出后自然回收 | `kill`：立即硬切，无淡出 |
| 槽位何时可复用 | 淡出结束后的下一次渲染，或同次 note_on 中被抢占扫描选中 | 当次立即复用 |
| 新击键是否可能缺音 | 不会因互斥本身缺音（互斥只腾出同组声部，槽不够仍交给路径②） | 会：无 RELEASE 候选时整个 region 静默跳过 |
| 与 `note_off` 的关系 | 无关，击键瞬间即发生，不需要 MIDI note-off | 只关心包络段；`note_off`/延音松开让声部进 RELEASE 后才成为可抢对象 |

补充：普通 `tsf_note_off` (`tsf.h:1667-1685`) 与通道版 (`tsf.h:1943-1966`)
只结束「同 preset（或同通道）+ 同键 + 最小 playIndex」的那一档击键，
按正常 release 时长进入 RELEASE；延音踏板（CC64）按住时只打 `heldSustain` 标记、
不进 RELEASE (`tsf.h:1961-1962`)，这些声部因此**继续占用预算且不可被抢占**，
是预算被“撑满”的常见隐性来源。

---

## 2. 混合演奏片段的逐事件推演

下面用一个受控片段说明“普通乐器 + 分层镲片”同时出现时的完整决策过程。
字体构造（附录 A）：钢琴预设每个击键 3 个 region（`group=0`，不互斥）；
鼓预设中闭镲 key46 有 2 个分层 region（左右声道，均 `group=7`），
开镲 key49 有 1 个 region（`group=7`），底鼓 key35 为 `group=0`。
预算封顶 6 个声部。真实运行日志见 `diag_mix6.txt`（附录 B 实验 2）。

| # | 事件 | 决策过程 | 结果 |
|---|---|---|---|
| 1 | 钢琴 key60 击键 | 3 个 region 各取 1 空闲槽，`group=0` 不触发互斥 | 占用槽 0-2，3 声部齐响 |
| 2 | 钢琴 key64 击键 | 再取槽 3-5 | 6 槽全满，6 声部齐响 |
| 3 | 底鼓 key35 击键 | `group=0`，无空闲槽；预算扫描找不到 RELEASE 声部 → `continue` | **整音丢失**（额度原因，非互斥） |
| 4 | 开镲 key49 击键 | 同上，无 RELEASE 候选 | **整音丢失**（额度原因） |
| 5 | 钢琴 key60 松键 | 3 个声部按正常 release 进入 RELEASE 段 | 槽 0-2 变为可抢占候选，但仍发声 |
| 6 | 底鼓 key35 重试 | 抢占扫描选中 RELEASE 声部（槽0），`tsf_voice_kill` 硬杀 | 底鼓发声；key60 一个声层尾音被截断 |
| 7 | 闭镲 key46 击键（2 region） | region A：抢占槽1（原 key60 的 RELEASE 声部）；region B：抢占槽2 | 闭镲双声层齐响；被抢的是钢琴尾音，与镲片互斥无关 |
| 8 | 开镲 key49 击键 | **路径①**：同 preset 同 group7 的两个闭镲声部被 `endquick`（10 ms 淡出）；其中槽1 当场被本击键 region 经路径②抢占，槽2 进入 RELEASE | 开镲发声；闭镲尾音被互斥规则收短 |
| 9 | 渲染 12 ms | endquick 的 10 ms 淡出到期，槽 2 等被渲染线程自然回收 | 出现空闲槽 |
| 10 | 闭镲 key46 再击 | 路径① endquick 开镲（槽1→RELEASE）；两个 region 分别取空闲槽 2、3 | 开镲尾音收短，闭镲完整发声；底鼓（group0）全程不受影响 |
| 11 | 钢琴 key72 击键（3 region） | 槽1（RELEASE 中的开镲）、槽2（RELEASE 中的闭镲）被抢占，槽4 为空闲槽 | 3 声层齐响；被截断的又是镲片尾音——但这次是**额度抢占**，不是互斥 |

**结论性回答：**

- 新击键能否完整发声，先看空闲槽与可抢占的 RELEASE 声部够不够覆盖它命中的 region 数；
  不够就按 region 顺序静默丢层（事件 3、4）。
- 为新击键让出位置的旧声部有两种来源：(a) 同独占类的旧声部被 endquick 柔化终止
  （事件 8、10，只可能是镲片等带 ExclusiveClass 的音色）；(b) 任意预设中释放得最久的
  RELEASE 声部被硬杀（事件 6、7、11，可能是钢琴尾音也可能是镲片尾音）。
- 同组互斥**保证同组音色不重叠**，但不保证新音有槽——槽位不足时兜底的仍是额度逻辑，
  且 endquick 制造的 RELEASE 声部正好成为额度逻辑的首选牺牲品，两条路径在同一次击键内衔接。

---

## 3. 用于判定录音缺音/尾音缩短的事件记录方案

TinySoundFont 本身不输出任何诊断日志。要在录音里归因，需要在以下 5 个代码点打点，
每条记录带上时间戳（渲染采样帧号）、声部槽号、preset/key/playIndex/group、包络段。
打点位置即两条路径的唯一入口与出口，能把两类原因无重叠地区分开：

| 日志事件 | 建议打点位置 | 必带字段 | 归因含义 |
|---|---|---|---|
| `VOICE_START` | `tsf_note_on` 初始化完成处 `tsf.h:1614-1655` 之后 | 槽号、preset、key、playIndex、group、region 序号/本击键 region 总数 | 新声部实际启动；汇总同 playIndex 的 START 数即可发现“缺层” |
| `EXCL_ENDQUICK` | `tsf.h:1573` 调用点 | 被终止声部的槽号/preset/key/group、触发它的新 key/playIndex | **互斥规则**导致的尾音缩短；尾音应再持续约 10 ms |
| `BUDGET_STEAL` | `tsf.h:1599` `tsf_voice_kill(voice)` 处 | 被杀声部槽号/其已释放采样数、新击键 playIndex/region 序号 | **复音额度**导致的硬截断（无淡出） |
| `BUDGET_SKIP` | `tsf.h:1597-1598` 的 `continue` 前 | 新击键 preset/key/playIndex、被跳过的 region 序号、当时 active 数 | **复音额度**导致的整层/整音缺失（函数仍返回 1，只能靠此日志发现） |
| `VOICE_RELEASE` / `VOICE_DONE` | `tsf_voice_end` (`tsf.h:1180`) 与渲染回收点 `tsf.h:1350-1354` | 槽号、进入 RELEASE 的原因（note_off / 延音释放 / endquick）、release 时长 | 正常松键淡出与上述两种异常终止的对照基线 |

### 录音对照分析方法

1. **缺音（音符完全没响或只剩部分声层）**
   - 在 MIDI 事件流里找到该击键的 note-on 时间与它命中的 region 数 N；
   - 同一 playIndex 下若 `VOICE_START` 数 < N：差额就是 `BUDGET_SKIP` 数 → **额度原因**；
   - 互斥路径不会产生缺音（它只终止旧声部），若日志只有 `EXCL_ENDQUICK` 而 START 数齐，
     说明新音完整、只是旧音被收短；
   - 注意 `tsf_note_on` 对跳过不报错，返回值仍是 1，不打点无法从外部观测。
2. **尾音缩短**
   - 旧声部在 note-off 时间点之前就终止：若终止记录是 `EXCL_ENDQUICK`，
     且终止时刻恰好有同 group 的新 `VOICE_START` → **互斥规则**，尾音残留约 10 ms（柔切）；
   - 若终止记录是 `BUDGET_STEAL`，终止时刻有新击键且被杀声部处于 RELEASE → **额度抢占**，
     尾音立即消失（硬切），波形上表现为零淡出的不连续点；
   - 若终止发生在 note-off 之后且走 `VOICE_RELEASE` 正常时长 → 属正常演奏，不是故障；
   - 延音踏板期间的声部不进 RELEASE (`tsf.h:1961-1962`)，既不可被抢也不会让出槽，
     排查“踏板段落频繁丢音”时应同时记录 CC64 与 `heldSustain` 状态。
3. **判据速查**：柔切（≈10 ms 淡出）+ 同组新击键 = 互斥；硬切（无淡出）+ 被杀者在 RELEASE = 额度；
   新击键 START 数不足 = 额度跳过；这三类特征在波形与日志中互不混淆。

### 最低成本的无侵入替代方案

若不能改 `tsf.h`，可在调用层实现近似观测：

- 每次 `tsf_note_on` 前后调用 `tsf_active_voice_count` (`tsf.h:1702-1708`)，
  差值小于预期 region 数即发生了 skip/复用；结合 `tsf_set_max_voices` 设定的上限可判断为额度问题。
- 维护自己的 (channel,key)→playIndex 表与时间线，结合 MIDI note-on/off 与 CC64，
  可离线推断哪些音被提前终止；但无法区分 endquick 与 kill，也拿不到被抢声部的身份——
  精确归因仍需上述 5 个打点。

---

## 附录 A：仓库自带素材的实测结构

用独立 SF2 解析脚本（`sf2_dump.py`）解析 `examples/florestan-subset.sf2`：

- 17 个旋律预设、55 个采样，**无 bank 128 鼓组，全部 region 的 ExclusiveClass 为空（group=0）**，
  即该字体不会触发路径①；每个键位只命中 1 个 region，一次击键只占 1 声部。
- `examples/venture.mid`（经 `midi_dump.py` 解析）：9 轨、1049 个 note-on，
  峰值同时按住 10 个 MIDI 音符，通道 9（MIDI 第 10 通道鼓）共 608 个事件
  （304 个 note-on + 304 个 note-off）。
  实际声部峰值 = 同时音符数 × 各自命中 region 数（含释放段未回收的声部），
  在封顶预算下这是触发路径②的直接压力来源。

为验证路径①与多 region 行为，分析目录中另构造了最小 SF2 `voicebudget.sf2`：
钢琴 3 region/击键（group=0），闭镲 key46 双声层 group=7，开镲 key49 group=7，底鼓 group=0，
全部为一次性采样（无循环），结构经同一解析脚本核对无误。

## 附录 B：可复现实验（分析目录，非仓库内容）

脚本与日志均在 `/tmp/tsf-analysis/`（`scripts/` 放代码，`data/` 放输入与日志），
通过参数传路径、Python 一律 `python3 -I` 运行，未在仓库目录内执行任何解释器/构建工具。

1. `voice_diag.c`（预算 8 与不限两种模式，日志 `diag_fixed8.txt` / `diag_unlimited.txt`）
   - 验证：开镲→闭镲时同 group 声部 SUSTAIN→RELEASE（releaseLeft=220/220，即 10 ms@22050Hz）；
     底鼓 group=0 不受影响；封顶模式下满预算且无 RELEASE 时 `note_on` 返回 1 但 active 数不变（skip）；
     不限模式下首次 note_on 使 `voiceNum` 0→4 的扩容行为。
2. `voice_mix_diag.c`（混合片段，预算 6/4，日志 `diag_mix6.txt` / `diag_mix4.txt`）
   - 验证第 2 节全部 11 个事件；预算 4 时钢琴 key64 的 3 个 region 只启动 1 个（缺层）。
3. `voice_edge.c`（预算 3 边界用例，日志 `diag_edge.txt`）
   - 验证同一次 note_on 内：闭镲 region A 先 endquick 开镲，因无空闲槽随即通过抢占扫描
     杀掉该 RELEASE 声部复用槽位；region B 无 RELEASE 候选被跳过——两条路径在一个击键内衔接。
