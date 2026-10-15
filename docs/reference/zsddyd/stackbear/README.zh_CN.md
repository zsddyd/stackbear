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
```

- 固件入口 `main/main.c`;界面 `main/sb_ui.c`;录音链路 `main/sb_recorder.c`;
  编解码 `main/sb_adpcm.c`;分帧 `main/sb_proto.c`;VAD `main/sb_vad.c`;
  WiFi/配网 `main/sb_net.c` 与 `main/sb_net_web.c`;上行 `main/sb_uplink.c`;
  NVS 配置 `main/sb_config.c`。
- 接收端工程在 `cloud/`,在 ECS 上用 `uv` 运行:
  `PYTHONPATH=$PWD/src .venv/bin/python -m stackbear.server --host 0.0.0.0 --port 18080`。
- 音频格式:IMA-ADPCM,4bit,16kHz 单声道,每个 60ms 帧一个自包含块
  (`[2 字节大端长度][4 字节块头 + nibble]`),约 7.8 KB/s。
- 分段边界跟随音频一起走:每个 VAD 语音段的第一块在 AUDIO 帧上置 `SEG_START` 标志,
  接收端把每段的起始偏移与帧数记进 `meta.json`,便于按句提交转写。

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

## 新增工具

| 工具 | 用途 |
| --- | --- |
| `tools/usb_console.py` | 在 Termux/Android 上读取 ESP32 原生 USB-CDC 控制台。Android 没有串口设备节点,minicom/pyserial 都连不上;本工具用 pyusb 包装 `termux-usb` 的设备描述符,读取 bulk-IN 端点。 |
| `tools/adpcm_to_wav.py` | 把落盘的会话(`audio.adpcm`,或旧的 `audio.opus`)解码成可播放的 WAV,并打印时长、峰值与 RMS,不听也能确认录音是否有效。 |

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

目前注册了两个 provider,预设会补齐地址、厂商标签与一个默认的音频模型,所以切换只需
一个变量:

| provider | 地址 | 密钥变量 | 默认模型 |
| --- | --- | --- | --- |
| `openrouter` | `https://openrouter.ai/api/v1` | `STACKBEAR_ASR_API_KEY` | `qwen/qwen3.8-omni-flash` |
| `opencode-go` | `https://opencode.ai/inference/go/v1` | `OPENCODE_API_KEY` | `mimo-v2.6-flash` |

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

## 构建、烧录与测试流程

- **构建**:只走 GitHub Actions(`.github/workflows/build-firmware.yml`),在
  ESP-IDF 5.5.3 下运行 `./tools/validate.sh --firmware`,产出 `0x0` 用的
  `build/FoloToy-AI-Passport-full.bin`。
- **烧录**:经 `termux-usb` 通道执行
  `nrflash write --chip esp32c3 --offset 0x0 --verify`;手机上需点"允许"授权。
- **注意**:`0x0` 合并镜像会覆盖 NVS 分区,WiFi 凭据会丢失,所以每次刷机都要重新
  配网。只刷应用分区(`0x10000`)可以保留凭据。
- **刷后启动**:ESP32-C3 的 USB-Serial/JTAG 能挺过 CPU 复位,刷完看起来像没重启;
  可靠的做法是手动给板子断电重启。
- **可观测性**:用 `tools/usb_console.py`;另外 `pkill -f stackbear.server` 会匹配到
  发起命令的 shell 本身,要用带方括号的写法。

## 验证状态

```text
Build:        PASS  (GitHub Actions,ESP-IDF 5.5.3 / esp32c3)
Host tests:   PASS  (sb_proto、sb_vad、sb_adpcm、cloud 协议与服务端、仓库检查)
Device tests: 核心链路 PASS
              刷写与设备端 MD5、启动、显示、三键、SoftAP 配网、麦克风采集、
              VAD 分段、ADPCM 编码、私有协议上传、ECS 按会话落盘
Unverified:   解码后的语音质量、长时稳定性与续航、断网重连,
              以及 ASR/LLM/飞书阶段
```

## 下一步

1. 支持只刷应用分区,让配网凭据在升级后保留。
2. 云端补上解码与会话列表,为 ASR 做准备。
3. 设备端长时稳定性与功耗实测。
4. 接入阿里云 ASR、LLM 抽取与飞书多维表格。
