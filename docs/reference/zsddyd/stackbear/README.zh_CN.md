<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Stackbear —— 全天候录音卡

基于 FoloToy AI Passport 的 `stackbear` 固件的应用档案与工程记忆。它在设备端持续录音、
本地编码,再通过私有协议流式上传到阿里云 ECS 上的接收服务,按会话落盘,供后续转写。

## 需求

| 项 | 决定 |
| --- | --- |
| 设备角色 | 常开 VAD 录音;只有语音被编码上传 |
| 网络 | WiFi STA 连接手机热点,设备端完成配网 |
| 传输 | 私有二进制帧,长连接 TCP 连到 ECS |
| 云端(本阶段) | 只接收并落盘音频流,不做 ASR/LLM |
| 后续阶段 | 阿里云 ASR -> LLM 抽取 -> 飞书多维表格(暂缓) |
| 界面 | 240x320 屏上重新设计的英文界面;三个物理按键 |

## 架构

```text
[Passport]  ES8311 采集 -> VAD 分段 -> IMA-ADPCM 编码 -> 私有 TCP 帧
                                                              |
                                                              v
[ECS 18080] asyncio 接收 -> data/<设备>/<会话>/audio.adpcm + meta.json
                                |
                                +-> 自动转写(按 VAD 分段)
```

- 固件入口 `main/main.c`;界面 `main/sb_ui.c`;录音链路 `main/sb_recorder.c`;
  编解码 `main/sb_adpcm.c`;分帧 `main/sb_proto.c`;VAD `main/sb_vad.c`;
  WiFi/配网 `main/sb_net.c` 与 `main/sb_net_web.c`;上行 `main/sb_uplink.c`;
  NVS 配置 `main/sb_config.c`;离线队列 `main/sb_cache.c`;OTA `main/sb_ota.c`;
  页面层级 `main/sb_nav.c`;构建信息 `main/sb_version.c`。
- 接收端工程在 `cloud/`,在 ECS 上用 `uv` 运行:
  `PYTHONPATH=$PWD/src .venv/bin/python -m stackbear.server --host 0.0.0.0 --port 18080`
  (或 `./run_server.sh`,日志直接写文件)。
- 音频格式:IMA-ADPCM,4bit,16kHz 单声道,每个 60ms 帧一个自包含块
  (`[2 字节大端长度][4 字节块头 + nibble]`),约 7.8 KB/s。
- 分段边界跟随音频一起走:每个 VAD 语音段的第一块在 AUDIO 帧上置 `SEG_START` 标志,
  接收端把每段的起始偏移与帧数记进 `meta.json`,便于按句提交转写。

### 界面层级

```text
HOME            聚合状态卡(启动时立即填充)+ SEG/SENT/DROP
                 + 两行可选动作:Stop recording(红)/ Settings
 └ SETTINGS     分组菜单(即原来的 STATUS)
     ├ RECORDER  Recording / Segments / Sent / Dropped
     ├ NETWORK   Wi-Fi / SSID / Device IP / Server IP / Uplink
     ├ STORAGE   Cache / Size / Used / Pending / Dropped
     ├ DEVICE    Device ID / Firmware / Slot / Commit / Branch / Built / update
     └ HELP      三个长按按键说明(Hold UP / DOWN / OK)
```

每个页面右上角都显示电量百分比。以前只有 HOME 有,一进子页就看不到;现在由公共的
建屏函数统一创建并登记,电量也只在**一处**读取。

现在每个屏幕都是"选中 + 确认":UP/DOWN 移动高亮,OK 执行高亮那一行。这个手势在
HOME(两行)、SETTINGS 和各个子页完全一致,于是只剩两条**屏幕本身教不出来**的手势
需要说明 —— 那正是 HELP 里写的东西。"轻点"指一次按下并抬起;不设双击手势,因为按键
组件会把间隔小于短按窗口(本板 180ms)的连按合并成一次多击,那会让"按两次"的含义
取决于用户按得多快:

| 页面 | UP 轻点 | DOWN 轻点 | OK 轻点 | UP 长按 | DOWN 长按 | OK 长按 |
| --- | --- | --- | --- | --- | --- | --- |
| HOME | 光标上移 | 光标下移 | 执行选中行:开始/停止录音,或进入 SETTINGS | 开始 / 停止录音(快捷键) | — | 进配网 |
| SETTINGS | 光标上移 | 光标下移 | 进入子页 | — | 返回 HOME | 进配网 |
| RECORDER / NETWORK | 光标上移 | 光标下移 | —(纯展示) | — | 返回 SETTINGS | 进配网 |
| STORAGE | 光标上移 | 光标下移 | 切换选中设置(仅 `Cache` 与 `Size`) | — | 返回 SETTINGS | 进配网 |
| DEVICE | 光标上移 | 光标下移 | 执行选中动作(仅 `Check for update` 行) | — | 返回 SETTINGS | 进配网 |
| HELP | — | — | — | — | 返回 SETTINGS | 进配网 |
| SETUP(配网) | — | — | — | — | 取消并返回 | 取消并返回 |

无论连按多快,每次轻点都只移动一格。SETTINGS 上对 OK 有反应的行用强调色标出,其余
只读;HOME 只强调"破坏性"那一行:停止录音是红色(它会结束会话并封文件),Settings 是
强调色。第一行的文字跟着状态走 —— 暂停时写 `Start recording`,始终说的是"按下去会
怎样",而不是"现在是什么状态"。HOME 一进来高亮就停在那一行。长按 UP 是同一个
开始/停止动作的快捷键,与高亮停在哪一行无关;列表页仍然不绑定录音开关,避免浏览
菜单时误停会话。配网页是模态:短按 UP/DOWN/OK 在那里保持无效,屏幕上写明(`hold DOWN or OK to exit`);
但长按 DOWN 可以退出 —— 和别的页面一样是"长按返回",长按 OK 也仍然有效(它是进/出
配网的同一个开关)。
聚合状态(融合凭据、链路、暂停、上行四层判断)只在 HOME 出现,子页不重复;
Wi-Fi 与 Uplink 分列,因为"WiFi 通"并不等于"能上传"。

### 列表滚动与按键说明页

列表按**虚拟窗口**渲染:页面只声明条目数,屏幕上放得下几行就只创建几个行槽,
滚动时把这些槽重新指向条目的另一段。对象数由屏幕高度决定,不随条目增长,所以
以后往 RECORDER / NETWORK / STORAGE / DEVICE 里加信息不会撑大 LVGL 内存 ——
加出来只是可滚动。右侧那条细滚动条只在页面确实超屏时才出现。现有每个页面都不超过
7 行(正好一屏),所以除了新增的 HELP 行,外观没有任何变化。

早先的 HELP 按页面分组,写了 32 行 —— 滚动能力就是为它加的。等每个屏幕都变成
"选中 + 确认"之后,那些内容大多只是在复述高亮已经说明的事,于是 HELP 改成三行,
用与其它页面相同的 Key/值版式,一屏放得下、不再需要滚动:

| Key | 值 |
| --- | --- |
| Hold UP | Start / stop |
| Hold DOWN | Back a level |
| Hold OK | Set up Wi-Fi |

它没有光标(没有东西可执行),SETTINGS 里那一行也不再加右侧摘要 —— `Help` 这个名字
本身就是完整的说明。取值宽度按这套版式最紧的约束算:最宽的 Key 是 `Hold DOWN`
(88px),右对齐的取值只剩 98px,而三个取值实测 80 / 85 / 90px,都不会被截断。
虚拟窗口机制留在列表里(它才是"页面加内容也不涨内存"的那部分);`main/sb_scroll.h`
是纯逻辑并有主机测试(`tests/test_sb_scroll.c`),覆盖最容易写错的边界:顶部/底部
越界、条目少于可视行、空列表。

DEVICE 的取值不会越出自己的那一列。`Slot` 显示 `A`/`B`,不用内部的 `ota_0`/`ota_1`
(字母就是 OTA 切换的那两个槽);`Branch` 未选中时显示以 `...` 结尾的前缀,把光标停在
该行才开始走马灯,整串分支名会绕环滚动,字符逐个经过最左边,每个字符都能看到。前缀
长度由 `Branch` 这个 Key 自身的宽度算出(`main/sb_text.c` 的 `sb_text_fit_ellipsis`),
所以截断后的值绝不会爬到旁边的 Key 上;同一条路径也给过长的 OTA 失败原因收口。
`Check for update` 不需要再写 `press OK`:它的名字已经用链接色画出来了。

## 排查出来的工程结论

以下问题都是靠串口工具逐个定位的,顺序即发现顺序:

1. **WiFi 状态竞态把设备 abort 掉**。`WIFI_EVENT_STA_START` 里自动
   `esp_wifi_connect()`,导致随后的 `esp_wifi_set_config()` 返回
   `ESP_ERR_WIFI_STATE`,被 `ESP_ERROR_CHECK` 直接 abort。现在连接前先 disconnect,
   且所有 WiFi 调用改为返回错误而不是 abort。
2. **分配顺序与 I2S DMA 冲突**。先建编码器会让 `i2s_alloc_dma_desc` 拿不到内存
   (`ESP_ERR_NO_MEM`)。现在先初始化音频,再建编码器,并且先起录音后连 WiFi。
3. **Opus 在这颗芯片上跑不动**。libopus 需要约 25KB 编码器状态;而 `78/esp-opus`
   用 `-DUSE_ALLOCA` 构建,把 CELT/SILK 的临时缓冲区全部分配到任务栈上(几十 KB)。
   任务栈加到 24KB、32KB 都仍然溢出;尝试改成伪栈模式也没有解决。
4. **决定:改用 IMA-ADPCM 而不是 Opus**。ADPCM 状态只有几个字节、没有大 scratch,
   压缩比约为一半,对"录音留档后转文字"足够。这一改动去掉了 25KB 编码器,
   镜像也小了约 170KB。
5. **上行会往死 socket 里写**。`send()` 失败被静默吞掉,接收端一重启设备就再也
   不重连。现在发送失败会关连接并重连。
6. **任务栈大小**。录音任务需要 32KB 静态栈,才能容纳 `bsp_audio_init()` 加编码循环。
7. **日志绝不能拖垮连接**。接收端曾用 `python -m stackbear.server | tee log` 启动;
   tee 的读端一旦消失,stdout 就没有读者,每次 `print()` 都抛 `BrokenPipeError` ——
   它属于 `OSError`,被连接处理的 `except (OSError, ...)` 吞掉,于是连接刚建立就被
   关闭,设备开始每两秒重连,界面显示 UPLOAD ERR。现在所有诊断都经过 best-effort 的
   `log()`(忽略写失败),并提供 `run_server.sh` 直接写文件而非走管道。
8. **重启后不再使用已保存的凭据**。`sb_net_ensure_connected()` 只看内存里的
   `s_ssid`,而在重连重构后开机没有任何地方填充它,于是每次重启(含 OTA 后)都表现得
   像"未配网"。修复方式是 `sb_net_load_saved()`,在 `sb_net_init()` 之后调用一次。
   另外配网一度彻底失败:`apply_sta_config()` 在配网期间直接拒绝 —— 那道守卫本意只
   约束后台自动重连。
9. **离线队列项按协议上限分配**。每个上行队列槽按 1400 字节开,而 ADPCM 帧只有
   484 字节,24 个槽白占 32.9KB 常驻内存。叠加上行 socket 后,OTA 下载期间堆被耗尽
   (`wifi:alloc eb ... fail`,随后 load access fault)。现在每项只装一帧音频
   (共 7.6KB),并用静态断言绑定到 ADPCM 块与缓存槽;OTA 下载缓冲改为 2KB 静态;
   下载前停掉上行任务,把内存还给 WiFi。
10. **一个永远匹配不上的 JSON 解析**。OTA 清单解析要求 key 后紧跟引号,而实际是
    `"version": "..."`(中间有冒号),于是每次取值都失败,"Check for update" 在下载
    之前就报 `check failed`。现在会跳过分隔符、拒绝被截断的值(截断的 sha256 会让
    校验悄无声息地失效)并要求闭合引号;它独立成 `main/sb_json.[ch]` 并带回归测试。

## 新增工具

| 工具 | 用途 |
| --- | --- |
| `tools/usb_console.py` | 在 Termux/Android 上读取 ESP32 原生 USB-CDC 控制台。Android 没有串口设备节点,minicom/pyserial 都连不上;本工具用 pyusb 包装 `termux-usb` 的设备描述符,读取 bulk-IN 端点。 |
| `tools/adpcm_to_wav.py` | 把落盘的会话(`audio.adpcm`,或旧的 `audio.opus`)解码成可播放的 WAV,并打印时长、峰值与 RMS,不听也能确认录音是否有效。 |
| `tools/make_ota_manifest.py` | 生成 OTA 清单(`version`/`sha256`/`size`/`commit`/`branch`/`url`),版本串与固件里编译进去的完全一致,设备才能识别出"已是最新"。 |

可主机测试的逻辑都刻意从 LVGL/ESP-IDF 代码里剥离,由 `tests/test_sb_*.c` 覆盖:
分帧(`sb_proto`)、VAD(`sb_vad`)、编解码(`sb_adpcm`)、离线槽位(`sb_cache_slot`)、
清单解析(`sb_json`)、页面层级(`sb_nav`)、列表滚动窗口(`sb_scroll`)、按宽度截断与走马灯(`sb_text`)。

## 界面设计说明(taste 规范)

布局遵循"去装饰"原则而不是堆卡片:一个地面色、一个墨色、一个强调色;选中态用
"亮一档的底色"表达,而不是描边或阴影;主要信息靠放大字号承担层级,而不是加装饰;
说明文字写全,不用图标代替。状态行右侧直接给出取值,不进子页也能读。

## 转写 provider 层

`cloud/src/stackbear/providers.py` 把 ASR 厂商隔离在一个接口之后(厂商、模型、地址、
密钥),接收端不关心由谁转写。配置只从环境变量读取,密钥绝不入库:

```text
STACKBEAR_ASR_PROVIDER  提供方名称(默认 openrouter)
STACKBEAR_ASR_VENDOR    随结果一起记录的厂商标签
STACKBEAR_ASR_MODEL     默认 qwen/qwen3.8-omni-flash
STACKBEAR_ASR_BASE_URL  默认 https://openrouter.ai/api/v1
STACKBEAR_ASR_API_KEY   bearer token
```

目前注册了三个 provider,预设会补齐地址、厂商标签与一个默认的音频模型,所以切换只需
一个变量:

| provider | 地址 | 密钥变量 | 默认模型 |
| --- | --- | --- | --- |
| `openrouter` | `https://openrouter.ai/api/v1` | `STACKBEAR_ASR_API_KEY` | `qwen/qwen3.8-omni-flash` |
| `opencode-go` | `https://opencode.ai/inference/go/v1` | `OPENCODE_API_KEY` | `mimo-v2.6-pro` |
| `local` | 无(进程内) | 无 | `sense-voice-small-int8` |

### 本地识别(真正可用的免费方案)

OpenRouter 上**所有免费音频模型实测都不可用**:两个
(`thinkingmachines/inkling*:free`)返回 HTTP 403"仅限 agentic harness";
`nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free` 对我们 16kHz 单声道 WAV 的每一个
音频块都回"(No spoken words detected)"。能用的付费模型按小时计费,因此 `local`
provider 同时去掉了成本与网络依赖,音频也不出机器。

实现是 sherpa-onnx + SenseVoice-Small int8:`uv pip install sherpa-onnx` 装 wheel
(无需编译),模型从 `hf-mirror.com` 拉取(国内主机直连 `huggingface.co` 超时,镜像可用)。
配置:

```text
STACKBEAR_ASR_PROVIDER=local
STACKBEAR_ASR_MODEL_DIR=<repo>/models/sense-voice   # model.int8.onnx + tokens.txt
STACKBEAR_ASR_THREADS=2
STACKBEAR_ASR_LANGUAGE=zh
```

在 ECS 上实测(2 vCPU Xeon,支持 AVX-512,1.6GB 内存):模型加载 1.6 秒,双线程约
8 倍实时,85 秒录音约 9 秒转完,常驻内存约 250~400MB。测量中得到两个结论:

- **分块长度比模型本身更关键**。语音识别器是在短片段上训练的:同一段 85 秒音频切成
  11 个约 8 秒的块得到 158 字,而整段一次提交只出 1 个字;15 秒和 25 秒介于中间
  (134 字和 79 字)。因此本地 provider 的 `STACKBEAR_ASR_CHUNK_MS` 默认 **8 秒**。
- **要指定语言**。开启自动检测时,开头两个安静块在一段中文对话里被识别成英文
  ("B."、"After.");`STACKBEAR_ASR_LANGUAGE=zh` 消除了这个问题。

### 领域词表(提示词做不到的那部分)

最直觉的想法 —— 给识别器一个"更好的提示词" —— 在这里不成立。SenseVoice 是端到端
模型,没有提示词接口;它的热词能力(sherpa-onnx 的
`hr_dict_dir`/`hr_lexicon`/`hr_rule_fsts`)是基于**通用拼音表**的同音替换。我把官方
热词文件下载下来在真实录音上做了对照,结果几乎没变化 —— 那张表不可能包含一个团队的
专有名词。

真正有效的是识别**之后**应用的领域词表(`cloud/glossary.txt` +
`cloud/src/stackbear/glossary.py`),它与引擎无关,因此云端模型同样受益:

```text
# 一行一条,长词优先
应哥 => 运哥
一哥 => 运哥
LE   => C798-ASE
```

替换结果不会被再次扫描(规则不会意外串联);纯 ASCII 的错形会自动加词边界 ——
`LE => C798-ASE` 会改写含该代号的句子,但不会动 `LEVEL` 和 `file-led`。在那段暴露出
问题的 39 秒录音上实测:同一位同事被识别成的三个名字合并成了一个,型号也正确了。

OpenCode Go 订阅按 models.dev 注册表确实包含支持音频输入的模型:`mimo-v2.6-flash`、
`mimo-v2.6-pro`、`mimo-v2.5`、`gemini-4-argon-contributor`、
`muse-spark-1.2/1.3-contributor`。注意其文档写明面向编码代理流量,当作实验路径而非
生产 ASR 通道。

接 OpenCode Go 有两个容易踩的点:

- Cloudflare 会把通用 SDK 的 User-Agent 拦成 **error 1010**;必须发送真实客户端名
  (`STACKBEAR_ASR_USER_AGENT`)以及厂商要求的 `x-opencode-session` 头。
- 它的 MiMo 是推理模型:`content` 返回 null,转写文本落在 `reasoning_content`
  (mimo-v2.6)或 `reasoning`(mimo-v2.5)里,解析器需要按这些字段回退。

同一段 12.5 秒录音实测:`mimo-v2.6-flash` 与 `mimo-v2.5` 都能正确转写(且优于
OpenRouter 上同一模型的路由——后者返回空文本);`gemini-4-argon-contributor` 在
工作区开启"允许会用数据训练的端点"之前拒绝服务。

模型实测(国内主机、一段 12.5 秒真机录音,16kHz 单声道 WAV 以 `input_audio` 提交):
`mistralai/voxtral-small-24b-2507` 与 `qwen/qwen3.8-omni-flash` 都能正确转写;
`nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free` 虽在音频模型列表里,实际会丢掉音频
并反过来索要文件;`google/*`、`openai/*` 的音频模型返回 403“本地区不可用”;
`xiaomi/mimo-v2.6-flash` 返回空文本。换模型只需改这一个环境变量。

`python -m stackbear.transcribe <会话> --per-segment` 会解码会话、按记录的 VAD 分段
切分并逐段转写;`--dry-run` 只检查配置不调用 API,`--write` 把 `transcript.txt` /
`transcript.json` 写回会话目录。

VAD 会切出大量碎段(85 秒语音被切成 62 段,最短 60ms),而语音模型面对这么短的
片段只会回常见短语("嗯""Thank you.")或者干脆描述声音,而不是转写 —— 缺的是上下文。
因此提交前会把相邻段合并成至少 8 秒的块(上限 30 秒);超过上限的单段保持完整而不是
切开;末尾的短块会并回前一块。在那段 85 秒录音上,请求数从 62 降到 8、总覆盖完全一致,
输出也从碎片变成了可读的对话。

接收端也会**自己转写**:会话结束(收到 END 帧、重连或超时)后在工作线程里解码并转写,
不阻塞接收循环,然后把 `transcript.txt`、`transcript.json`、`transcript.status.json`
写进会话目录。`STACKBEAR_ASR_AUTO=0` 可关闭;`STACKBEAR_ASR_PER_SEGMENT`(默认开)选择
按句还是整段转写;提交前会把碎段合并成有上下文的块(见上文)。某一个片段失败不再连累整个会话;
`STACKBEAR_ASR_RETRIES`(默认 3)会重试请求,因为有些网关把同一个模型路由到只有部分
worker 支持音频的池子里。

### 会话与转写的查询入口

转写结果与音频放在一起
(`data/<设备>/<会话>/{audio.adpcm, meta.json, transcript.txt, transcript.json}`),
`/transcripts` 是它们的统一入口:

| 请求 | 结果 |
| --- | --- |
| `GET /transcripts?limit=20` | 会话索引,新的在前:设备、时间、时长、段数、是否有转写、字数与一行预览 |
| `GET /transcripts?dev=<id>` | 同上,按设备过滤 |
| `GET /transcripts?q=<词>` | 内容搜索,每条命中带回匹配的行 |
| `GET /transcript/<设备>/<会话>` | 单个会话的全文 |

索引逐会话给出转写状态,便于区分"已完成"与"待处理";搜索直接读磁盘上的转写文本,
不维护可能和文件不一致的索引。注意非 ASCII 搜索词需要 URL 编码:接收端会先把查询串
还原成 UTF-8 再匹配(请求行按 latin1 解码对请求头是对的,但会让中文词变成乱码而永不命中)。

## 构建、烧录与测试流程

- **构建**:只走 GitHub Actions(`.github/workflows/build-firmware.yml`),在
  ESP-IDF 5.5.3 下运行 `./tools/validate.sh --firmware`,产出两种包:
  `build/FoloToy-AI-Passport-full.bin`(`0x0` 合并镜像)与
  `build/FoloToy-AI-Passport-ota.bin` + `build/ota-latest.json`(OTA 包)。配置了
  ECS secrets 时,工作流会把镜像和清单发布到接收端;没配则打印跳过,需要手动拷贝。
- **烧录**:经 `termux-usb` 通道执行
  `nrflash write --chip esp32c3 --offset 0x0 --verify`;手机上需点"允许"授权。
- **分区布局**:两个 OTA 槽(`ota_0`/`ota_1`,各 2.94MB)、`fota`(otadata,按项目
  约定改名)、约 2MB 的 `audioq` 环形区(离线队列)。备用 OTA 槽同时充当下载目标,
  因此不需要额外的暂存分区。改成双槽时 `factory` 变更**必须线刷一次** —— 这也是
  最后一次需要插线。
- **数据影响**:`0x0` 合并镜像会覆盖 NVS 分区,凭据会丢失,**线刷后要重新配网**;
  **OTA 不碰 NVS**,所以升级后凭据保留。
- **刷后启动**:ESP32-C3 的 USB-Serial/JTAG 能挺过 CPU 复位,刷完看起来像没重启;
  可靠的做法是手动给板子断电重启。
- **可观测性**:用 `tools/usb_console.py`;另外 `pkill -f stackbear.server` 会匹配到
  发起命令的 shell 本身,要用带方括号的写法。

### OTA

设备向接收端请求 `/firmware/latest.json`,比较版本后把镜像下载进备用槽(明文 HTTP),
按清单里的 sha256 校验,设置启动分区并重启。镜像是**仅 app** 的镜像(不含 bootloader
与分区表)。已启用回滚(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`),`app_main` 会确认
正在运行的镜像,所以在确认之前崩溃的固件会被回滚到上一个槽。下载前 OTA 任务会结束
当前录音会话并停掉上行任务,把内存还给 WiFi。

没有用 HTTPS:接收端目前只有公网 IP 与 18080 端口,完整性依赖清单里的 sha256。
`/firmware/` 只接受纯文件名,请求无法越出目录。

## 验证状态

```text
Build:        PASS  (GitHub Actions,ESP-IDF 5.5.3 / esp32c3)
Host tests:   PASS  (sb_proto、sb_vad、sb_adpcm、sb_cache_slot、sb_json、sb_nav、
                     sb_scroll(列表窗口)、sb_text(截断 / 走马灯)、
                     cloud 协议/adpcm/providers/server/autotranscribe/firmware、
                     仓库检查)
Device tests: 核心链路 PASS
              刷写与设备端 MD5、启动、显示、三键、SoftAP 配网、麦克风采集、
              VAD 分段、ADPCM 编码、私有协议上传、ECS 按会话落盘、自动转写,
              以及一次真实 OTA(检查 -> 下载 -> sha256 -> 切槽 -> 重启进新版本)
Unverified:   解码后的语音质量、长时稳定性与续航、断网重连、
              离线队列在真机上的写入,以及 LLM/飞书阶段
```

## 下一步

1. 配置仓库 secrets `ECS_HOST` / `ECS_USER` / `ECS_PASSWORD`,让工作流自动发布
   OTA 包。
2. 设备端长时稳定性与功耗实测,以及断网状态下离线队列的写入/补传验证。
3. 轮换开发期用过的 API key(它们出现在对话里;换完只需改环境变量)。
4. 转写文本的 LLM 抽取,再接入飞书多维表格。
