# TinySoundFont 通道调音与弯音分析报告

> 对象：`tsf.h`（TinySoundFont v0.9，仓库当前 HEAD `a3621a1`）
> 关注：演奏中修改弯音幅度（RPN 0）、修改通道基准音高（RPN 1/2）、转动弯音轮（Pitch Bend）时，
> 正在发声的长音以及此后新击键的实际音高。
> 文中所有数值均在本机用仓库内 `tsf.h` 编译探针程序实测（详见附录 A），不是纯理论推算。

---

## 0. 结论速览

1. **单个通道的变调量是一个三元函数**（`tsf.h:1795-1802`、`tsf.h:1885-1913`）：

   ```
   shift = (wheel == 8192) ? tuning
                           : wheel/16383 * pitchRange * 2 - pitchRange + tuning
   ```

   单位是半音，直接叠加到该通道**所有**正在发声的 voice 上（包括释放段），
   再由 `2^(shift/12)` 换算成采样播放速率比。

2. **典型数值**（RPN0 弯音幅度 = 12 半音，RPN2 coarse = +2，RPN1 fine = +0.5，弯音轮 = 12288）：

   | 量 | 值 |
   |---|---|
   | 实际变调量 `shift` | **+8.501098633 半音**（+850.1099 音分） |
   | 相对中心状态的速率比 | **1.63401914** |
   | 其中弯音轮贡献 | +6.0010987 半音 → ×1.41430332 |
   | 其中调音贡献 | +2.5 半音 → ×1.15535270 |
   | 若样本 22050 Hz、输出 44100 Hz、root key 60、按键 60 | 播放速率 **0.81700957** 源样本/输出样本（≈ 36030 源样本/秒） |
   | 对应绝对音高（key 60 = root key） | 8.176×2^(68.5011/12) ≈ **427.51 Hz** |

3. **当前控制器读数能否解释正在响的音符？**
   — 库导出的三个 getter（`tsf_channel_get_pitchwheel/pitchrange/tuning`）**可以**逐位重建
   通过 `tsf_channel_note_on` 启动的 voice 的变调量（1200 个随机样本实测 0 误差）；
   但 **原始 MIDI 字节（RPN 号 + Data Entry MSB/LSB）不能**，因为 fine tune 每收到一个
   Data Entry 字节就执行一次 `T ← trunc(T) + f`，粗调整数部分会被逐步吃掉，
   映射不是单射（§5.3）；而且 getter 只给出"变调量"，绝对音高还取决于
   voice 所用的 region（root key / transpose / tune / keytrack / sample_rate）。
   另有若干 voice 根本不受当前通道状态支配（§5.9）。

4. **新击键与其它通道**：同一通道的新击键在 note-on 瞬间按**当时**的三元组算好 shift 并固化，
   因此它和仍在响的长音**同弯同调**；其它通道完全独立，新建通道一律是
   `wheel=8192 / range=2 / tuning=0` 的默认值。

5. **必须警惕的缺陷（均已实测复现）**：
   - RPN1 fine tune 每字节执行 `T ← trunc(T) + f`，coarse 与 fine **反号**时 coarse 会被吃掉
     （最多偏 2 半音/轮）；`Data Entry = 0` 时**无界下滑**（§5.3）；
   - RPN1 的 Data Entry 字节会**继承上一个 RPN 的 LSB**（§5.1）；
   - `CC121` 复位幅度与调音却**保留弯音轮**，长音会突跳（§5.4）；
   - 通道未创建时 `tsf_channel_note_on` **静默丢音**且返回 1（§5.8）；
   - 用 `tsf_note_on` 直启的 voice，其 `playingChannel` 未定义，后续弯音行为未定义（§5.9）。

---

## 1. 代码路径与状态模型

### 1.1 通道状态（每个 MIDI 通道一份）

```c
// tsf.h:464-468
struct tsf_channel
{
    unsigned short presetIndex, bank, pitchWheel, midiPan, midiVolume,
                   midiExpression, midiRPN, midiData : 14, sustain : 1;
    float panOffset, gainDB, pitchRange, tuning;
};
```

与音高相关的是 4 个字段：`pitchWheel`(0..16383)、`pitchRange`(半音)、`tuning`(半音)、
以及 RPN 解析用的 `midiRPN` 和 14 位 `midiData`。

新建通道时的默认值（`tsf.h:1783-1790`）：

```c
c->pitchWheel = c->midiPan = 8192;
c->midiRPN = 0xFFFF;        // "无 RPN 选中" 的哨兵
c->midiData = 0;
c->pitchRange = 2.0f;
c->tuning = 0.0f;
```

注意 `tsf_channel_init` 是**按需创建**的：请求通道 n 时，0..n 全部一次性初始化为默认值
（`tsf.h:1759-1793`）。

### 1.2 Voice 固化下来的音高状态

```c
// tsf.h:451-462
struct tsf_voice
{
    int playingPreset, playingKey, playingChannel, heldSustain;
    struct tsf_region* region;
    double pitchInputTimecents, pitchOutputFactor;   // ← 音高的全部秘密
    ...
};
```

**关键：voice 不保存 wheel/range/tuning，只保存已经算好的
`pitchInputTimecents`**。渲染时不看通道，只用这两个 double（`tsf.h:1250-1251`、`1270-1271`）。
所以"通道状态"只是**输入**，一旦写入 voice，voice 就是独立的。

### 1.3 谁会改写 voice 的音高

| 入口 | 位置 | 是否遍历正在响的 voice |
|---|---|---|
| `tsf_channel_set_pitchwheel` | `tsf.h:1885-1893` | 是（若值变化） |
| `tsf_channel_set_pitchrange` | `tsf.h:1895-1903` | 仅当 `pitchWheel != 8192` |
| `tsf_channel_set_tuning` | `tsf.h:1905-1913` | 是（若值变化） |
| `tsf_channel_midi_control`（CC6/38、CC98/99/100/101 选中 RPN，CC121） | `tsf.h:1985-2033` | 间接调用上面三个 |
| `tsf_channel_note_on`（新音） | `tsf.h:1931-1941` → `tsf.h:1621-1624` | 对新 voice 调用一次 |
| `tsf_channel_set_volume/pan/sustain` | 相应函数 | 改音量/声像/包络，**不改音高** |

统一出口：

```c
// tsf.h:1795-1802
static void tsf_channel_applypitch(tsf* f, int channel, struct tsf_channel* c)
{
    struct tsf_voice *v, *vEnd;
    float pitchShift = (c->pitchWheel == 8192 ? c->tuning
        : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) - c->pitchRange + c->tuning));
    for (v = f->voices, vEnd = v + f->voiceNum; v != vEnd; v++)
        if (v->playingPreset != -1 && v->playingChannel == channel)
            tsf_voice_calcpitchratio(v, pitchShift, f->outSampleRate);
}
```

新音路径用的是**完全相同**的表达式（`tsf.h:1753`），见 §7。

---

## 2. 音高模型

### 2.1 从按键到播放速率

```c
// tsf.h:1211-1218
static void tsf_voice_calcpitchratio(struct tsf_voice* v, float pitchShift, float outSampleRate)
{
    double note = v->playingKey + v->region->transpose + v->region->tune / 100.0;
    double adjustedPitch = v->region->pitch_keycenter
                         + (note - v->region->pitch_keycenter) * (v->region->pitch_keytrack / 100.0);
    if (pitchShift) adjustedPitch += pitchShift;
    v->pitchInputTimecents = adjustedPitch * 100.0;
    v->pitchOutputFactor  = v->region->sample_rate
                          / (tsf_timecents2Secsd(v->region->pitch_keycenter * 100.0) * outSampleRate);
}
```

令 `kc = pitch_keycenter`、`kt = pitch_keytrack`（%）、`tr = transpose`、`fn = tune`（音分）：

```
note          = key + tr + fn/100
adjustedPitch = kc + (note - kc)·kt/100 + shift          [半音]
```

渲染时（`tsf.h:1251`）：

```
pitchRatio = 2^(pitchInputTimecents/1200) · pitchOutputFactor
           = 2^(adjustedPitch/12) · sample_rate / (2^(kc/12) · outSampleRate)
```

`pitchRatio` 就是**每输出一个样本，源样本位置前进多少**，即"采样播放速率"
（渲染循环里 `tmpSourceSamplePosition += pitchRatio`，`tsf.h:1304` 等）。
可听频率与它成正比：

```
f_out = 8.176 · 2^(adjustedPitch/12)   [Hz]      (8.176·2^(69/12)=440，即 A440 约定)
```

> 推导：播放速度 = `pitchRatio · outSampleRate / sample_rate`
> = `2^(adjustedPitch/12) / 2^(kc/12)`；乘以样本 natural pitch `8.176·2^(kc/12)`
> 后 `kc` 与采样率全部约掉，得 `8.176·2^(adjustedPitch/12)`。
> 这一结论依赖 SF2 的约定"样本在其 root key 上的原始音高等于该键的十二平均律音高"，
> 与 `sample_rate`、输出采样率、`kt` 都无关。

### 2.2 通道变调量 S 的分段定义

```
S(wheel, R, T) =  T                                   , wheel == 8192   （中心特判）
               =  wheel/16383 · 2R − R + T            , 其它
```

其中 `R = pitchRange`（半音），`T = tuning`（含小数半音）。

两种等价写法（便于核对）：

- 相对中心偏移：`off = (wheel/16383·2 − 1)·R`
- `off` 在 `wheel=0` 时恰好 `−R`，`wheel=16383` 时恰好 `+R`（两端可达满幅）
- 但**线性式的中点是 8191.5**（`8192/16383·2−1 = +6.10e-5`），
  而 MIDI 的弯音中心是 **8192**（14 位 0..16383，负半幅 8192 格、正半幅 8191 格，天然不对称）。
  两者相差 0.5 格，所以代码必须用 `wheel == 8192` 特判把中点钉回 0，
  代价是中点处出现一个很小的步长不连续（见 §5.5）

### 2.3 换算表

| 想要 | 公式 |
|---|---|
| 半音 → 速率比 | `2^(semi/12)` |
| 音分 → 速率比 | `2^(cent/1200)` |
| 一个 LSB 在 `R=12` 时的音分 | `24/16383·100 = 0.14649 cent` |
| `S=8.501098633` | `2^(8.501098633/12) = 1.63401914` |

---

## 3. 实例：一段连续 RPN + 弯音轮动作的完整推导

### 3.1 消息序列（通道 0，正在响的是 key 60 的长音）

| # | MIDI | 语义 |
|---|---|---|
| 1 | `CC101=0, CC100=0` | 选中 RPN 0（Pitch Bend Sensitivity） |
| 2 | `CC6=12, CC38=0` | Data Entry = 12.00 → `pitchRange = 12.00` |
| 3 | `CC101=0, CC100=2` | 选中 RPN 2（Coarse Tuning） |
| 4 | `CC6=66` | 66−64 = **+2** 半音 → `tuning = +2.0` |
| 5 | `CC101=0, CC100=1` | 选中 RPN 1（Fine Tuning） |
| 6 | `CC6=96, CC38=0` | Data Entry = 96·128+0 = 12288 → `(12288−8192)/8192 = +0.5` → `tuning = +2.5` |
| 7 | `PitchBend = 12288` | 弯音轮推到 3/4 位置 |

### 3.2 通道状态逐步变化（实测，含中间态）

| 步骤 | wheel | range | tuning | 长音实际 shift |
|---|---|---|---|---|
| 初始 | 8192 | 2.00 | 0.000000 | +0.000000 |
| 1–2 RPN0 | 8192 | **12.00** | 0.000000 | +0.000000（中心特判，保持不变） |
| 3–4 RPN2 coarse | 8192 | 12.00 | **+2.000000** | +2.000000 |
| 5–6 RPN1 fine | 8192 | 12.00 | **+2.500000** | +2.500000 |
| 7 弯音轮 | **12288** | 12.00 | +2.500000 | **+8.501098633** |

> 表中第 2 步 `(midiData>>7) + 0.01*(midiData&0x7F)`（`tsf.h:2029`）给出 12 + 0.00；
> 第 4 步 `((float)control_value − 64.0f) + 小数部分`（`tsf.h:2031`）给出 +2.0，小数部分为 0；
> 第 6 步 `(int)tuning + (midiData − 8192)/8192`（`tsf.h:2030`）给出 2 + 0.5 = 2.5。

### 3.3 数值结果（精确推导）

```
wheel 项 = 12288/16383 × 12 × 2 − 12
        = 0.75004578 × 24 − 12
        = 18.001098700 − 12
        = +6.001098700 半音          (弯音轮贡献，实数)

tuning = +2.500000000 半音           (RPN2 + RPN1 贡献)

S = 6.001098700 + 2.500000000 = +8.501098700 半音
  = +850.109870 音分
```

`S` 以 `float` 存入 `pitchShift`（`tsf.h:1798`），实测 voice 中的
`pitchInputTimecents = 6850.109863281`，即 `S = 68.501098633 − 60 = 8.501098633` 半音
（float32 量化误差 −6.7e-8 半音，可忽略）。下文同时给出两种精度：

| 量 | 实数推导值 | float32 实际存放值 |
|---|---|---|
| `S` | +8.501098700 半音 | **+8.501098633 半音** |
| `2^(S/12)` | 1.634019150 | **1.634019144** |
| `0.5 × 2^(S/12)` | 0.817009575 | **0.817009572** |

### 3.4 换算为采样播放速率比

```
速率比（相对未变调） = 2^(S/12)
                     = 2^(8.501098633/12)
                     = 1.63401914           （float32 实测）
                     = 1.63401915           （实数推导）
```

分解校验（实数精度）：

```
弯音轮单独: 2^(6.0010987/12) = 1.41430332
调音单独  : 2^(2.5/12)       = 1.15535270
乘积      : 1.41430332 × 1.15535270 = 1.63401915   ✓
```

**代入具体 sample**（`pitch_keycenter=60, pitch_keytrack=100, key=60, sample_rate=22050, 输出=44100`）：

```
pitchRatio = 2^(adjustedPitch/12) × sample_rate / (2^(kc/12) × outSampleRate)
adjustedPitch = 60 + 8.501098633 = 68.501098633
           = 2^(68.501098633/12) × 22050 / (2^(60/12) × 44100)
           = 52.28861 × 22050 / (32 × 44100)
           = 0.5 × 1.63401914
           = 0.81700957   源样本/输出样本
```

即 44.1 kHz 输出时每秒消耗 `0.81700957 × 44100 = 36030.12` 个源样本；
可听基频 = `8.176 × 2^(68.5010986/12) ≈ 427.51 Hz`
（若未变调则 261.63 Hz，比值同为 1.634）。

### 3.5 弯音轮位置 → 数值全表（`R=12, T=2.5`，key 60，样本 22050/输出 44100）

| wheel | S（半音） | S（音分） | 速率比 2^(S/12) | pitchRatio |
|---|---|---|---|---|
| 0 | −9.500000000 | −950.000 | 0.57767635 | 0.28883817 |
| 2048 | −6.499816895 | −649.982 | 0.68698409 | 0.34349205 |
| 4096 | −3.499633789 | −349.963 | 0.81697501 | 0.40848750 |
| 6144 | −0.499450684 | −49.945 | 0.97156277 | 0.48578138 |
| 8191 | +2.499267578 | +249.927 | 1.15530382 | 0.57765191 |
| **8192** | **+2.500000000** | **+250.000** | **1.15535270** | **0.57767635** |
| 8193 | +2.502197266 | +250.220 | 1.15549934 | 0.57774967 |
| 10240 | +5.500915527 | +550.092 | 1.37402631 | 0.68701315 |
| **12288** | **+8.501098633** | **+850.110** | **1.63401914** | **0.81700957** |
| 14336 | +11.501281738 | +1150.128 | 1.94320775 | 0.97160387 |
| 16383 | +14.500000000 | +1450.000 | 2.31070539 | 1.15535270 |

### 3.6 音频侧验证

用渲染出的波形做零交叉周期估计（key 60，`R=12`，最小 SF2 的 50 样本锯齿循环）：

| 状态 | 实测速率比 | 理论 2^(S/12) | 相对误差 |
|---|---|---|---|
| coarse+2, fine+0.5（wheel=8192） | 1.155347173 | 1.155352697 | 4.8e-6 |
| 再加 wheel=12288 | 1.634041861 | 1.634019144 | 1.4e-5 |

误差来自周期估计的整样本量化（`44100/周期` 的离散化）与线性插值，量级符合预期，确认公式成立。

---

## 4. 长音在控制消息下的行为

### 4.1 谁会被改

`tsf_channel_applypitch` 的过滤条件是
`v->playingPreset != -1 && v->playingChannel == channel`：

- **同通道所有 voice**（同一按键的多个 region、同时按下的多个键、已经进入释放段的音）**一起改**；
- 不同通道、以及不经通道 API 启动的 voice **不受影响**（§5.9）；
- 改动是**瞬时阶跃**，没有 portamento/平滑；`sourceSamplePosition` 连续保留，
  因此波形相位不重置，只会在切换点产生一个不连续（通常是一声轻微 click）。

### 4.2 何时生效

`pitchRatio` 在 `tsf_voice_render` 入口按 `v->pitchInputTimecents` 算一次
（`tsf.h:1250-1251`），仅当 region 有 LFO/ModEnv 调制音高时才在每个 64 样本块内重算
（`tsf.h:1270-1271`）。因此：

- 控制消息在**下一次 `tsf_render_*` 调用**时才影响声音；
- 对**没有** LFO/ModEnv 音高调制的 region，`pitchRatio` 在 `tsf_voice_render` 入口只算一次，
  所以一次 `tsf_render_float(buf, N)` 内部**不会**中途变调（N 再大也一样）；
  只有 `dynamicPitchRatio` 为真的 region 才会在每个 64 样本块重读
  `pitchInputTimecents`，此时跨线程修改会在半途生效；
- 若渲染线程与 MIDI 线程并行且未加锁，存在读到半更新状态的数据竞争
  （`tsf.h:127-141` 的注释已声明需外部同步）。

### 4.3 释放段也会变

释放段的 voice 仍然 `playingPreset != -1` 且 `playingChannel` 匹配，
所以弯音轮在 note-off 之后继续有效。实测：key60 进入 release 后，把弯音轮推到 12000
（该通道为默认 `R=2`），其 `pitchInputTimecents` 由 6000 变为 6092.986631
（+0.9299 半音）——释放尾音会跟着"滑"。

### 4.4 幅度与钳位

代码**没有任何钳位**：

- `wheel` 是 `unsigned short`，`tsf_channel_set_pitchwheel(int)` 直接强转（`tsf.h:1890`）。
  传 −1 会存成 65535，`S = 65535/16383·2R − R + T`，`R=2` 时等于 **+14 半音**（实测 rate 1.12251）。
  传 20000 也照存，`R=2` 时 `S = +2.882` 半音（实测 rate 0.59060）。
- `pitchRange` 可由 RPN0 推到 **128.27** 半音（`data=16383` → 127 + 1.27，实测；
  此时 `wheel=16383` 得到 `S=+128.27`、`pitchRatio=825.5`），
  也可为 0（此时弯音轮完全失效，实测 rate 恒为 0.5）。
- `coarse` 可为 −64..+63 半音（`control_value 0..127` 减 64，实测 CC6=0 → `tuning=−64`）。

---

## 5. RPN 状态机实现细节与偏差清单（全部实测）

### 5.1 Data Entry 是 14 位组合寄存器，LSB 会残留

```c
// tsf.h:1997-1998
case 6:  c->midiData = (c->midiData & 0x7F)   | (control_value << 7); goto TCMC_SET_DATA;
case 38: c->midiData = (c->midiData & 0x3F80) |  control_value;       goto TCMC_SET_DATA;
```

写 MSB 时**保留旧 LSB**，写 LSB 时保留旧 MSB。后果：

- 先 `range = 12.50`（MSB=12, LSB=50），再只发 `CC6=2`，得到 **2.50** 而不是 2.00；
- 只发 `CC6=32` 而不发 `CC38` 时，fine tune 会吃到上一个 RPN 遗留的 LSB
  （实测得到 `−0.493896` 而不是 LSB=0 时的 `−0.500000`）。

### 5.2 CC6 与 CC38 各自独立触发一次音高更新

`TCMC_SET_DATA`（`tsf.h:2028-2032`）对 **CC6 和 CC38 都**执行，且**不检查**
"该 RPN 是否在本次交互中被选中过"。因此一条标准的两字节 Data Entry
会让长音先后经历两个值：

```
CC6 之后:  T ← trunc(T_old) + f(MSB, LSB_残留)
CC38 之后: T ← trunc(T_中间) + f(MSB, LSB)
```

- RPN0（range）路径是幂等的（`(data>>7)+0.01*(data&0x7F)` 只依赖 data），
  两次更新得到同一值，听不到；
- RPN1（fine）路径**不幂等**（§5.3），两次更新可能不同，
  且 T 的整数部分每过一次就被截断一次；
- RPN2（coarse）只检查 `controller == 6`（`tsf.h:2031`），CC38 不会触发，
  但它用 `(T − trunc(T))` 保留小数，恰好不会丢信息。

### 5.3 RPN1 fine tune 会把 coarse 整数部分"吃掉"（最严重，会让长音逐音跳）

```c
// tsf.h:2030
else if (c->midiRPN == 1)
    tsf_channel_set_tuning(f, channel,
        (int)c->tuning + ((float)c->midiData - 8192.0f) / 8192.0f);
```

即每收到一个 Data Entry 字节（CC6 **或** CC38），都执行一次

```
T ← trunc(T) + f ,      f = (midiData − 8192)/8192 ∈ [−1, +0.99988]
```

`(int)` 在 C 中**向零截断**。这是一个迭代映射，其**不动点**恰好是

```
T = n + f     ，n 为整数，且 f ≥ 0 时要求 n ≥ 0，f < 0 时要求 n ≤ 0
```

（即"整数部分与 f 同号或为零"）。

**边界特例 `data = 0` 必须单独说明**：此时 `f = (0 − 8192)/8192 = −1.0` 恰好是整数，
`trunc(T) + (−1)` 每执行一次就把 `T` 减 1，**永远没有不动点**。
实测（coarse=0 起）：

```
msg #1 → T=−2    msg #2 → T=−4    ...    msg #6 → T=−12
```

即每个 Data Entry 字节 −1 个半音，一条两字节消息 −2 个半音，**无界下滑**；
（`data = 1` 的 `f = −0.99988` 则正常收敛到 −1.99988，说明这是 `−1.0` 这一点的孤立病态。）

其余情况：

- `f ≥ 0` 时，**负的整数部分不是不动点**，会被逐字节向零抬起；
- `f < 0` 时，**正的整数部分不是不动点**，会被逐字节压向零；
- 每发一个 Data Entry 字节，`T` 只移动 ≤1 个半音，所以一条两字节消息最多移动 2 个半音。

而 RPN2 coarse 用的是 `(c->tuning − (int)c->tuning)`（`tsf.h:2031`）保留小数，
两条路径对小数部分的处理不一致（coarse 能正确保留，fine 会把它当成新的整数起点）。

实测（每条 RPN1 消息发 MSB、LSB 两个字节，连续 6 轮；`R=2` 无关）：

| 初始（coarse, f） | 期望 | 第 1 轮后 | 第 2 轮 | 第 3 轮 | 收敛值 | 半音误差 |
|---|---|---|---|---|---|---|
| (+2, +0.50) | +2.50 | +2.50 | +2.50 | +2.50 | **+2.50** | 0（正数且 f≥0，安全） |
| (+2, −0.25) | +1.75 | +0.75 | −0.25 | −0.25 | **−0.25** | −2.0 |
| ( 0, −0.25) | −0.25 | −0.25 | −0.25 | −0.25 | **−0.25** | 0（无 coarse 可丢） |
| (−2, +0.50) | −1.50 | −0.50 | +0.50 | +0.50 | **+0.50** | +2.0 |
| (−2, −0.25) | −2.25 | −2.25 | −2.25 | −2.25 | **−2.25** | 0（负 T 时 trunc 正好保小数） |
| (+5, −0.75) | +4.25 | +3.25 | +1.25 | −0.75 | **−0.75** | −5.0 |
| (−5, +0.75) | −4.25 | −3.25 | −1.25 | +0.75 | **+0.75** | +5.0 |

逐字节核对（`(+2, −0.25)` 第 1 轮）：

```
起始 T = +2
CC6=48 : T ← trunc(2)     + (−0.25) = +1.75
CC38=0 : T ← trunc(1.75)  + (−0.25) = +0.75
```

**结论（比"负值截断"更准确）**：

1. **符号相反**的 coarse 与 fine 一定会互相抵消：`(−2, +0.5)` 两轮后变成 `+0.5`（偏高 2.0 半音）；
   `(+2, −0.25)` 收敛到 `−0.25`（偏低 2.0 半音）。这是常见的损坏场景——
   "降一个全音再往上微调一点点"在乐谱上很自然，在这个实现里会把整个 coarse 丢掉；
2. **符号相同**时安全：`(+2, +0.5)`、`(−2, −0.25)`、`(0, −0.25)` 都幂等；
3. 每个 Data Entry 字节固定移动 ≤1 半音，所以**长音会听到一格一格的台阶式跑调**
   （`(+5, −0.75)` 需 3 轮才到 −0.75，期间经过 +3.25、+1.25）；
4. 只有 `coarse=0` 时才无整数部分可丢（`(0, −0.25)` 实测稳定）——
   这也解释了为什么 §3 的示例（coarse 与 fine 同为正）恰好没问题；
5. **`Data Entry = 0`（`f = −1`）会让 `T` 每次减 1 且永不收敛**：每条消息 −2 半音，
   播放速率每 6 条消息减半。实测 50 条消息后 `T=−100`、速率 0.00155（约 −56 dB），
   100 条后 `T=−200`、速率 4.8e-6（约 −106 dB）——听感上就是**长音持续下滑直至消失**。
   这是把 RPN1 数据写成 0（一种常见的"清空"写法）所能造成的最坏情况。

实测扫描 855 组 (coarse ∈ [−4,4], data ∈ [1,16382])，8 轮消息后
**全部落在上述不动点之一（0 个例外）**，模型可复现。

MIDI 文件里"每次换音色重发 RPN"的常见写法会反复触发这个过程；
若 coarse 与 fine 反号，最终音高会稳定在 `n + f`（通常就是 `f`）而不是 `coarse + f`。
除 `data=0` 这一特例外，它**不会无限漂移**：到达不动点就停住，
所以表现是"跳几个半音后稳定"，而不是单调爬音。`data=0` 则每个字节 −1 半音，无界下滑。

> 复现时注意：`midiData` 的 LSB 会在 RPN 切换时残留（§5.1），
> 上面的 `f` 是按目标 `data` 计算的，前提是发送了完整的 MSB+LSB 对。

**额外陷阱：RPN2 的 coarse 值直接来自 `control_value`，而不是 `midiData`。**

```c
// tsf.h:2031
else if (c->midiRPN == 2 && controller == 6)
    tsf_channel_set_tuning(f, channel,
        ((float)control_value - 64.0f) + (c->tuning - (int)c->tuning));
```

所以只要 RPN2 处于选中状态，**每一条** CC6 都会被解释成 coarse 值。
实测：选好 coarse=+2 后，若还用同一通道发 `CC6=96, CC38=0` 试图改 fine
（忘记先切回 RPN1），则 `CC6=96` 被当成 coarse = +32，
`tuning` 直接变成 **+32.0**（偏 30 个半音），随后切到 RPN1 再发 `CC6=96`
得到 +32.5。序列化 MIDI 时"忘记重发 RPN 选择号"的代价是数量级的。

### 5.4 CC121（All Controllers Off）不重置弯音轮

```c
// tsf.h:2008-2018
case 121:
    c->midiVolume = c->midiExpression = 16383;
    c->midiPan = 8192;  c->bank = 0;
    c->midiRPN = 0xFFFF; c->midiData = 0;
    tsf_channel_set_volume(f, channel, 1.0f);
    tsf_channel_set_pan(f, channel, 0.5f);
    tsf_channel_set_pitchrange(f, channel, 2.0f);   // 幅度回 2
    tsf_channel_set_tuning(f, channel, 0);          // 调音回 0
    return 1;                                        // pitchWheel 未动
```

实测：`wheel=13000, R=12, T=+2` 时 `S=+9.044132`；CC121 之后
`wheel` 仍为 13000，但 `R=2, T=0`，`S` 突变为 **+1.174022**。
即"复位控制器"会让长音**向下跳 7.870110 半音**（速率比 0.843043 → 0.535083），
而且之后弯音轮的工作幅度也变小了。
同时 `midiData=0` 而 `midiRPN=0xFFFF`，RPN 选择被清空。

### 5.5 中心点特判造成的折点（R=12 时 ±0.22 音分）

```
8191 → S = −0.000732   (R=12 时 −0.0732 cent)
8192 → S =  0.000000   （特判）
8193 → S = +0.002197   (+0.2197 cent)
```

即相邻两格在中心处步长不相等（8191→8192 是 0.000732 半音，8192→8193 是 0.002197 半音）。
绝对值很小（R=12 时最大 0.22 音分），听感与音准检测都可忽略，
但做"逐样本可复现"或差分测试时要把它算进去：不能用线性式直接算 `wheel=8192`。

### 5.6 RPN 选择与 NRPN

- `midiRPN` 用 `0xFFFF` 表示"未选中"（`tsf.h:1785`）；
- NRPN（CC98/99）直接把 `midiRPN` 置为 `0xFFFF`（`tsf.h:2003-2004`），
  之后 Data Entry **不会**影响 pitch range / tuning（实测 range 保持 12.00）；
- RPN Null（CC101=127, CC100=127）实测得到 `0x3FFF`（不是 MIDI 规范的 0x7F7F：
  MSB 被 `&0x7F` 截成 0x7F 后左移 7 位存入 14 位字段），不匹配 0/1/2，
  Data Entry 同样被忽略（这一点行为正确，只是内部编码与规范不同）；
- 但 `midiData` **不会**因为切换 RPN 或 NRPN 而清零，这就是 5.1 残留的来源。

### 5.7 幅度 LSB 的单位是"百分之一半音"

`(midiData >> 7) + 0.01f*(midiData & 0x7F)`（`tsf.h:2029`）：
MSB 是整数半音，LSB 是 **0.01 半音**（不是标准 MIDI 的 1 音分）。
`12.50` 合法；`R` 的取值范围是 `0.00 … 127 + 1.27 = 128.27`（实测两端）。

### 5.8 通道未创建时 `tsf_channel_note_on` 静默丢音

```c
// tsf.h:1931-1933
if (!f->channels || channel >= f->channels->channelNum) return 1;  // 仍返回 1
```

实测：从未调用任何 `tsf_channel_*` 就调 `tsf_channel_note_on(f, 0, 60, 1.0f)`，
活动 voice 数为 **0**，函数却返回 1。因此"新击键按什么音高响"的前提是
该通道**事先**被任一 `tsf_channel_*` 调用创建过（`tsf.h:1759-1793`）。

### 5.9 非通道 API 的 voice 不受通道状态支配（未初始化字段）

`tsf_note_on` 直接调用时：

```c
// tsf.h:1621-1631
if (f->channels) f->channels->setupVoice(f, voice);
else { tsf_voice_calcpitchratio(voice, 0, f->outSampleRate); ... }
```

- `channels == NULL` 时 shift 固化为 0，且 **`playingChannel` 从未被赋值**（`tsf.h:1747-1757`
  只在 `setupVoice` 里写）。实测用 0xAA 填充分配器时该字段为 `−1431655766`，
  用零页内存时恰好为 0；
- 之后若创建了同号通道并改弯音，该 voice 是否被 `tsf_channel_applypitch` 命中
  完全取决于这段垃圾值 → 未定义行为；
- 若 `channels != NULL`，则用的是**当时共享的** `activeChannel`
  （`tsf.h:1749`、`1751`），即"最后一次 `tsf_channel_note_on` 用过的通道"。

### 5.10 `tsf_reset` 与通道生命周期

`tsf_reset` 直接 `free(f->channels)`（`tsf.h:1494-1501`），所有通道状态（含调音）全部丢弃，
下次 `tsf_channel_*` 会以默认值重建。**它不是"复位控制器"，而是"删除通道"**。
同理，同一通道上的 note-off / All Notes Off（CC123）/ All Sound Off（CC120）都不改音高状态：
它们只推进包络到 release（实测 `ampenv.segment` 由 5 变 6），
之后继续弯音，释放尾音仍会跟着变调（实测 rate 0.52974 → 0.48478）。
`tsf_reset` 是例外：它把通道整个删掉。

---

## 6. 当前控制器读数能否独立解释正在响的音符

分四个层次回答。

### 6.1 库级 getter 可以精确重建"变调量"

`tsf_channel_get_pitchwheel/pitchrange/tuning`（`tsf.h:2060-2073`）与
`tsf_channel_applypitch` 的公式同源，且 voice 中保存的就是同一公式的结果。因此对
**通过 `tsf_channel_note_on` 启动的 voice**：

```
S_rebuilt = (wheel == 8192) ? T : wheel/16383·2R − R + T
note      = key + region.transpose + region.tune/100
base      = kc + (note − kc)·kt/100             // kt = region.pitch_keytrack
S_stored  = voice.pitchInputTimecents/100 − base
S_rebuilt == S_stored
```

`kt = 100` 时 `base` 化归为 `note`，可以省掉 `kc/kt`；
**`kt ≠ 100` 时必须把它们算进去**，否则重建会差 `(1 − kt/100)·(note − kc)` 半音。
实测（人为把 region 的 `pitch_keytrack` 改成 50、`kc=60`、key 72、`S=+2.0002`）：
按上式 `base = 66.00`，`stored_shift = 68.0002 − 66.00 = +2.0002` ✓；
若误把 `base` 当作 `note = 72`，则会得到 `−3.9998`，错 **6 个半音**。

实测：400 组随机 (wheel, R, T) 组合，4 个通道各 3 个键，共比对
**1200 个 voice-通道样本**，**0 个不匹配，最大误差 0.000000000 半音**。
所以"这个长音现在变调多少"可以只靠三个 getter + 按键/region 完全解释
（前提是该 voice 由 `tsf_channel_note_on` 启动，见 §6.4）。

### 6.2 但原始 MIDI 控制器字节不能

RPN 号 + Data Entry 两个字节**不足以**反推 `tuning`，原因：

1. **迭代有损映射**（§5.3）：fine 每字节执行 `T ← trunc(T) + f`，coarse 的整数部分
   会被逐步吃掉。例：`coarse=−2` 后发 fine `+0.5`，第 1 轮得 −0.5、第 2 轮得 +0.5，
   目标本应是 −1.5。同一个 `midiData` 因此可能对应多个不同 `tuning`；
   反之也成立：`(+2, +0.5)` 与"先 `set_tuning(2.5)`"在 getter 上不可区分。
2. **LSB 残留**（§5.1）：Data Entry 的实际值取决于历史写入顺序，
   单看"当前 RPN 号 + 最后一次 CC6/CC38"无法得知 `midiData`。
3. `midiData` 是 14 位位域，`CC121` 会把 `midiData` 清零但保留 `pitchWheel`（§5.4），
   于是"控制器已复位"这一直觉也是错的。
4. `pitchRange` 的 LSB 单位是 0.01 半音（§5.7），与常见 MIDI 实现的 1 音分不同。

**结论：需要一个应用侧的权威通道状态镜像（自己的 wheel/range/tuning），
或在需要时读库的 getter；不要用 MIDI 字节二次推导。**

### 6.3 getter 只给"变调量"，不给"绝对音高"

绝对音高还需要：`key`、该 voice 所用 region 的 `transpose`、`tune`（音分）、
`pitch_keycenter`、`pitch_keytrack`、`sample_rate`，以及输出采样率。同一个 `S`
在不同 preset 上可能有不同的 `kc`/`transpose`，因此**相同控制器读数 ≠ 相同音高**。

实测（`examples/florestan-subset.sf2`，所有通道均为默认 `wheel=8192, R=2, T=0`，
都按 key 60）：

| preset | `kc` | `transpose` | `tune`(音分) | `adjustedPitch` | `pitchRatio` | 绝对频率 |
|---|---|---|---|---|---|---|
| Piano | 56 | 0 | 0 | 60.00 | 0.62996053 | 261.63 Hz |
| Marimba | 62 | 0 | −25 | 59.75 | 0.43906304 | 257.88 Hz |
| Taiko | 72 | −3 | −26 | 56.74 | 0.20709051 | 216.73 Hz |
| Pan Flute | 73 | 0 | +46 | 60.46 | 0.24232245 | 268.68 Hz |

四处控制器读数完全相同，`pitchRatio` 却相差 **3 倍**（0.207 … 0.630）。
`pitchRatio` 的绝对值只对"该样本原始音高"有意义；而要换算成**可听频率**，
必须用 `8.176 × 2^(adjustedPitch/12)`，其中 `adjustedPitch` 才是与采样率无关的物理量。

换言之：

- 想回答"这个音听起来多高" → 用 `adjustedPitch`（或频率），它跨 preset 可比；
- 想回答"样本要放多快" → 用 `pitchRatio`，它包含 `sample_rate/outSampleRate` 的补偿；
- 两者都由同一组 `(S, key, region)` 决定，但**不能互相替代**。

### 6.4 有些 voice 根本不由"当前控制器状态"解释

- 用 `tsf_note_on`（无通道参数）启动、且当时 `f->channels == NULL` 的 voice（§5.9）：
  `playingChannel` 从未被赋值，shift 在启动时固化为 0；
- 用 `tsf_note_on` 启动、且当时 `channels != NULL` 的 voice：它用的是共享的
  `activeChannel`，即"最后一次 `tsf_channel_note_on` 用过的通道"，
  不一定是你以为的那个通道；
- `tsf_reset` 之后所有通道被删除重建，旧 voice 已按 release 结束，不存在遗留；
- 若 region 启用 `modLfoToPitch / modEnvToPitch / vibLfoToPitch`，实际瞬时音高还会
  在 `S` 之上叠加 LFO/包络（`tsf.h:1270-1271`），**控制器读数只能解释其基值**。

### 6.5 可复现的重建伪代码

```c
/* 受控通道上某个正在响的音符的实际变调量（半音） */
float shift_of(int ch) {
    int   w = tsf_channel_get_pitchwheel(f, ch);
    float R = tsf_channel_get_pitchrange(f, ch);
    float T = tsf_channel_get_tuning(f, ch);
    return (w == 8192) ? T : (w / 16383.0f * R * 2.0f - R + T);
}
/* 播放速率比。
   注意 kc 只有在 kt == 100 时才会与 note 中的 kc 抵消；
   kt != 100 时 adjustedPitch 里的 kc 不能被消掉，必须原样保留。 */
double note = key + tr + fn/100.0;
double adjustedPitch = kc + (note - kc) * (kt/100.0) + shift_of(ch);
double ratio = pow(2.0, adjustedPitch/12.0) * sample_rate / (pow(2.0, kc/12.0) * out_rate);
```

---

## 7. 同通道新击键与其它通道

### 7.1 同通道的新击键：与长音"同弯同调"

`tsf_channel_note_on` → `tsf_note_on` → `tsf_channel_setup_voice`（`tsf.h:1747-1757`）
用与 `applypitch` **相同**的表达式在 note-on 瞬间算 shift：

```c
v->playingChannel = f->channels->activeChannel;
tsf_voice_calcpitchratio(v, (c->pitchWheel == 8192 ? c->tuning
    : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) - c->pitchRange + c->tuning)),
    f->outSampleRate);
```

实测（通道 0，`R=12, T=+2.5, wheel=12288`，即 `S=+8.501098633`）：

| 新击键 | 固化 shift | pitchRatio（kc=60, sr=22050, out=44100） | 相对长音 |
|---|---|---|---|
| key 60（长音，先按下） | +8.501098633 | 0.81700957 | 1.000000 |
| key 64（此后新按） | +8.501098633 | 1.02936756 | 1.259921 |
| key 72（此后新按） | +8.501098633 | 1.63401914 | 2.000000 |

要点：

- 新音**不会**从"中心音高"起步再弯上去，而是**一出生就带当前弯音**，
  与仍在响的长音严格同音准；
- 新旧音共享同一 `S`，所以它们之间永远是纯音程（上表 4 半音 → ×1.259921 = 2^(4/12)，
  12 半音 → ×2）；
- 如果此刻正处在一条两字节 RPN 的中间（例如 fine 的 MSB 已到、LSB 未到），
  新音会**固化那个中间态**（§5.2），与长音短暂失谐；第二个 Data Entry 到来时
  `applypitch` 会把两者一起拉到最终值，因此只是瞬时现象。
- 若同一按键的多个 region 都被匹配，它们收到的是同一个 `S`（`pitchShift` 是标量），
  因此它们之间的相对音程不变；但每个 voice 各自的 `base`（`kc`/`transpose`/`tune`）
  不同，绝对频率仍按 §6.3 的规则计算。
- 反过来：长音是按"**上一个** `S`"固化的，所以如果消息队列里有 pending 的 RPN 字节，
  长音与新音的差异就是这两次 `applypitch` 之间的值。

### 7.2 其它通道：完全独立，默认 A440/±2

- 每个通道一个 `tsf_channel`，状态互不影响。实测：通道 0 处于 `S=+8.5011` 时，
  另一通道上 key 60 的 `shift=0`、`pitchRatio=0.5`（kc=60 合成样本），
  绝对音高 261.63 Hz；
- 新通道由 `tsf_channel_init` 惰性创建，**一律** `wheel=8192, range=2, tuning=0`；
- 在通道 A 上改 RPN/弯音，不会迁移到通道 B；`CC121` 只作用于本通道；
- 但**音高绝对值仍取决于各通道的 preset/region**：即使两通道控制器读数相同，
  若 preset 的 `transpose/tune/kc` 不同，实际音高也不同（§6.3 的表：同样 key 60、
  同样默认控制器，Taiko 的 `pitchRatio` 只有 Pan Flute 的 0.85 倍、但绝对频率低 52 Hz）；
- 通道 9（鼓）没有特殊音高逻辑，除非应用层跳过弯音消息；
- 若一个通道从未被任何 `tsf_channel_*` 调用创建，`tsf_channel_note_on` 会静默丢音（§5.8），
  表现为"这个通道没声音"而不是"音高不对"。

### 7.3 汇总对照

| 场景 | 实际音高 |
|---|---|
| 同通道长音（控制消息之后） | 原音高 × `2^(S/12)`，S 为**最新**三元组 |
| 同通道新击键 | 该键自身的音高 + 同一个 `S`（note-on 时固化） |
| 同通道再改弯音 | 长音与新音**一起**变，仍保持同一 `S` |
| 其它通道 | 该通道自己的 `S`（默认 0），与当前通道无关 |
| `tsf_note_on` 直接调用 | shift 来自 `activeChannel` 或 0，**不可预测**（§5.9） |
| 释放段 voice | 仍跟随本通道 `S`（§4.3） |

---

## 8. 给使用者的建议

1. **不要把 RPN 的 Data Entry 当作无状态寄存器**。若需要可重复、可逆的调音，
   直接调 `tsf_channel_set_tuning(f, ch, 半音)` / `set_pitchrange` / `set_pitchwheel`，
   并在应用层维护唯一真相。
2. **RPN1 fine tune 会吃掉 coarse 整数部分**（§5.3）：coarse 与 fine **反号**时音高会
   逐步跳到 `f`，`Data Entry = 0`（`f = −1`）时更会**无界下滑**。
   规避方式：只用 RPN2 coarse 做整数半音，fine 用 `set_tuning`；
   或发完 RPN1 后立即用 `set_tuning(期望值)` 覆盖一次；
   或保证 coarse 与 fine 同号、且绝不发送 `Data Entry = 0`。
3. **一条 RPN1 消息会产生两字节 → 最多两次 +1 半音**的台阶，长音会听到跳变；
   发送时把 MSB 与 LSB 视为一个原子操作可以减小但无法消除这个现象（§5.2）。
   需要绝对可靠时，**不要用 RPN1**，直接 `tsf_channel_set_tuning`。
4. **`CC121` 不是"全部复位"**：弯音轮被保留而幅度/调音被重置，长音会跳音。
   需要真正复位时，显式再发一次 `<8192` 的 Pitch Bend。
5. **启动任何通道上的音符之前先创建通道**（任一 `tsf_channel_*` 调用），
   否则 `tsf_channel_note_on` 静默失败。
6. **不要混用 `tsf_note_on` 与通道 API**：前者不会正确登记 `playingChannel`，
   后续通道弯音对它是未定义行为。
7. 弯音轮/幅度数值请自行钳位到 `0..16383` 与合理半音范围（库不做检查，§4.4）。

---

## 附录 A：验证方法（可复现）

全部在本机用仓库内 `tsf.h` 编译（`cc -O2 -lm`），未使用容器：

1. **状态探针**：`#define TSF_IMPLEMENTATION` 后 include `tsf.h`，
   直接读 `f->channels->channels[ch]`（`tsf_channel` 为内部类型，同 TU 内可见），
   以及 `f->voices[i]` 的 `pitchInputTimecents / pitchOutputFactor / playingChannel`。
2. **速率实测**：对单样本调用 `tsf_render_float(f, x, 1, 0)`，
   记录 `sourceSamplePosition` 的增量，取最小值作为 `pitchRatio`；
   与 `2^(tc/1200)·factor` 逐项一致。
3. **音频侧**：渲染 4 万样本，用上升零交叉估计周期，得到基频比。
4. **SoundFont 来源**：
   - 仓库自带 `examples/florestan-subset.sf2`（17 个 preset，Piano 的 region 为
     `kc=56, kt=100, sr=22050`）；
   - 从 `examples/example1.c` 内嵌的 `MinimalSoundFont` 字节数组还原出
     484 字节的最小 SF2（单样本、单 preset），便于得到 `kc=60` 的干净基准。
     注意 `MinimalSoundFont` 的 `TEN0` 宏要展开成 10 个 0 后再逐字节解析，
     且 `phdr` 记录是 38 字节、`shdr` 是 46 字节。
5. **未初始化字段**：用自定义 `TSF_MALLOC/TSF_REALLOC` 以 `0xAA` 填充，
   验证 `tsf_note_on` 路径的 `playingChannel` 未被赋值。
6. **模型扫描**：对 coarse ∈ [−4,4]、data ∈ [1,16382] 共 855 组各发 8 条 RPN1 消息，
   校验最终值是否落在 §5.3 的不动点集合内（全部命中）。

最小复现片段：

```c
#define TSF_IMPLEMENTATION
#include "tsf.h"
#include <math.h>
#include <stdio.h>

static void cc(tsf* f,int ch,int c,int v){ tsf_channel_midi_control(f,ch,c,v); }
int main(void){
    tsf* f = tsf_load_filename("examples/florestan-subset.sf2");
    tsf_set_output(f, TSF_MONO, 44100, 0);
    tsf_channel_set_presetindex(f, 0, 0);
    tsf_channel_note_on(f, 0, 60, 1.0f);
    cc(f,0,101,0); cc(f,0,100,0); cc(f,0,6,12); cc(f,0,38,0);   /* RPN0 = 12 */
    cc(f,0,101,0); cc(f,0,100,2); cc(f,0,6,66);                 /* coarse +2 */
    cc(f,0,101,0); cc(f,0,100,1); cc(f,0,6,96); cc(f,0,38,0);   /* fine +0.5 */
    tsf_channel_set_pitchwheel(f, 0, 12288);
    struct tsf_voice* v = &f->voices[0];   /* 若有多 region 请按 playingKey/Channel 查找 */
    double shift = v->pitchInputTimecents/100.0
                 - (v->playingKey + v->region->transpose + v->region->tune/100.0);
    double ratio = pow(2.0, v->pitchInputTimecents/1200.0) * v->pitchOutputFactor;
    printf("kc=%d shift=%.9f semis, ratio=%.9f (freq x%.9f)\n",
           v->region->pitch_keycenter, shift, ratio, pow(2.0, shift/12.0));
    /* 输出: kc=56 shift=8.501098633 semis, ratio=1.029367558 (freq x1.634019144) */
    tsf_close(f);
}
```

---

## 附录 B：符号与常量速查

| 符号 | 含义 | 来源 |
|---|---|---|
| `wheel` | 0..16383，8192 为中立 | `tsf_channel.pitchWheel` |
| `R` | 弯音幅度（半音，含 0.01 精度 LSB） | RPN 0 → `pitchRange` |
| `T` | 基准音高偏移（半音，可带小数） | RPN 1/2 → `tuning` |
| `S` | 通道实际变调量（半音） | §2.2 |
| `kc` | region 的 root key | `pitch_keycenter` |
| `kt` | 音高按键跟踪（%） | `pitch_keytrack` |
| `tr / fn` | region 粗调/细调（半音/音分） | `transpose` / `tune` |
| `out` | 输出采样率 | `tsf_set_output` |
| 速率比 | `2^(S/12)` | — |
| 音分 | `S×100` | — |
| 中心残差 | `8192/16383·2−1 = +6.10e-5` 相对 R | §5.5 |
| 一格 LSB | `2R/16383` 半音 = `0.1465 cent`（R=12） | — |
| A440 参考 | `f = 8.176·2^(adjustedPitch/12)` | `tsf_cents2Hertz` 同式 |
