# DNESP32S3 豆包实时语音助手（ESP-IDF / C）

这是为正点原子 `ATK_DNESP32S3 V1.2` 开发板编写的完整 ESP-IDF 工程。源码全部为 C，可在 VS Code 的 Espressif IDF 扩展中编译、通过 USB-UART 下载并打开串口监视器。

功能：

- ESP32-S3 连接 2.4 GHz Wi-Fi，并通过 TLS WebSocket 连接豆包端到端实时语音大模型。
- 按住板载 `BOOT` 键讲话，松开后发送结束标记。
- ES8388 板载麦克风录音；上传格式为 PCM、16 kHz、16-bit、单声道、20 ms/包。
- 豆包完成语音识别、对话推理和语音合成；返回 PCM、24 kHz、16-bit、单声道。
- ES8388 + 板载功放/扬声器播放回答。
- ST7789 SPI 屏显示联网状态、用户识别文本和豆包回复；串口同时打印完整文本和错误信息。
- 支持新控制台 `API Key` 和旧版 `App ID + Access Token` 两种鉴权。

## 一个重要的硬件限制

本工程使用接在板上 `SPI LCD` 接口的 ST7789 屏，而不是 40 针 RGB 屏。

原因是这块开发板的 RGB LCD 数据线与 ES8388 的 I2S 音频线复用了 GPIO3、GPIO9、GPIO10、GPIO14、GPIO46 等引脚。RGB 屏和板载麦克风/扬声器无法同时持续工作。厂家“小智 AI”工程也为该板选择了 SPI ST7789 方案。

支持的 SPI 屏可在 `menuconfig` 中选择：

- 2.4 英寸 ST7789，320×240（默认）
- 1.3 英寸 ST7789，240×240

如果你手头只有 40 针 RGB 屏，需要另接一块 SPI ST7789，或放弃板载 ES8388 音频后改用 RGB 屏。

## 硬件引脚

| 功能 | 引脚/器件 |
|---|---|
| ES8388 I2S MCLK | GPIO3 |
| ES8388 I2S BCLK | GPIO46 |
| ES8388 I2S LRCK/WS | GPIO9 |
| ESP32 → ES8388 DAC | GPIO10 |
| ES8388 ADC → ESP32 | GPIO14 |
| I2C SDA / SCL | GPIO41 / GPIO42 |
| ST7789 SCLK / MOSI | GPIO12 / GPIO11 |
| ST7789 CS / DC | GPIO21 / GPIO40 |
| 按住讲话 | GPIO0（BOOT） |
| 扬声器使能 / LCD 背光 | XL9555 P0.2 / P1.0 |

## 准备豆包凭证

1. 在火山引擎豆包语音控制台开通“端到端实时语音大模型”。
2. 新控制台建议创建语音 API Key；旧控制台可使用 App ID 和 Access Token。
3. 资源 ID 保持 `volc.speech.dialog`。
4. 默认模型为 O2.0 `1.2.1.1`，默认音色为 `zh_female_vv_jupiter_bigtts`。模型与音色必须匹配，否则服务端会返回 `InvalidSpeaker`。

官方资料：

- [端到端实时语音大模型 API](https://docs.volcengine.com/docs/DoubaoVoice/End-to-endreal-timespeechlargemodelAPIaccessdocument?lang=zh)
- [豆包语音模型列表](https://docs.volcengine.com/docs/DoubaoVoice/model-list?lang=zh)

## VS Code 编译和烧录

本工程已兼容 ESP-IDF `5.1.2`，也可使用 `5.3.x`、`5.4.x` 或 `5.5.x`。代码会在 5.1.2 上自动使用旧版 I2C 驱动，在 5.3 及以上自动切换到新版 I2C Master 驱动。首次构建需要联网，让 IDF Component Manager 下载 `esp_codec_dev`、`esp_websocket_client` 和 LVGL。

1. 安装 VS Code 和 `Espressif IDF` 扩展。
2. 在扩展中安装/选择 ESP-IDF 5.1.2（或 5.3–5.5）。
3. 用 VS Code 打开本工程根目录（即本 README 所在目录）。
4. 执行 `ESP-IDF: Set Espressif Device Target`，选择 `esp32s3`。
5. 执行 `ESP-IDF: SDK Configuration Editor (menuconfig)`，打开 `DNESP32S3 Doubao Voice Assistant`：
   - 填 Wi-Fi SSID 和密码；
   - 选择屏幕尺寸；
   - 选择豆包鉴权模式并填凭证；
   - 按需调整模型、音色、人设和扬声器音量。
6. 将 USB 线接到板上的 `USB UART`（CH340C）接口，在 VS Code 底栏选择对应 COM 口。
7. 点击 `Build`，然后点击 `Flash`，最后打开 `Monitor`（115200 baud）。

也可在已经激活 ESP-IDF 环境的终端执行：

```powershell
idf.py set-target esp32s3
idf.py fullclean
idf.py menuconfig
idf.py build
idf.py -p COM5 flash monitor
```

把 `COM5` 换成设备管理器中实际出现的串口。

如果这个目录以前曾由别的 ESP-IDF 版本编译过，切换到 5.1.2 后必须先执行一次 `idf.py fullclean`，避免旧版本的 CMake 缓存和托管组件配置残留。

## 使用方法

1. 上电后等待屏幕显示“就绪：按住 BOOT 键说话”。
2. 按住 `BOOT` 键讲话；红色 LED 会改变状态。
3. 说完后松开 `BOOT` 键。
4. 屏幕先显示识别到的文字，再显示豆包回复；语音从板载扬声器播放。
5. 播放过程中再次按住 `BOOT` 会清空尚未播放的音频并向服务端发送打断事件。

不要在复位或上电瞬间一直按住 BOOT，否则 ESP32-S3 会进入下载模式。

## 配置和安全

Wi-Fi 密码和豆包 API Key 最终会进入 `sdkconfig` 并编译进固件。不要把含真实凭证的 `sdkconfig` 提交到公开仓库。量产时应改成配网/NVS 安全存储，并在服务端增加额度限制和密钥轮换。

## 常见问题

- **屏幕不亮**：确认使用的是 SPI ST7789，而不是 40 针 RGB 屏；检查 `menuconfig` 中的尺寸选项。
- **连接豆包失败**：先看串口中的 TLS/HTTP 状态；确认开发板能访问 `openspeech.bytedance.com`，并确认凭证对应“端到端实时语音大模型”。
- **`InvalidSpeaker`**：音色与模型不匹配。默认 `1.2.1.1 + zh_female_vv_jupiter_bigtts` 是一组匹配配置。
- **只有文字没有声音**：确认扬声器已装好、音量不为 0；检查串口是否收到事件 352（TTSResponse）。
- **声音断续**：提高 Wi-Fi 信号质量；本工程在播放队列满时会丢弃过晚的音频包并在串口报警。
- **中文出现方框**：LVGL 内置 SimSun 字库只覆盖常用 CJK 字符，生僻字可能显示为占位符；串口日志仍保留完整 UTF-8 文本。

## 源码结构

```text
main/
├── main.c              初始化、按键消抖与主流程
├── board_io.c          I2C 与 XL9555
├── audio_board.c       ES8388、I2S、24k→16k 重采样、播放队列
├── display_ui.c        ST7789 + LVGL 界面
├── wifi_station.c      Wi-Fi STA 与 SNTP（TLS 证书校验需要正确时间）
└── doubao_realtime.c   豆包 WebSocket 二进制协议、ASR/LLM/TTS 事件处理
```

## 当前验证边界

工程已按 V1.2 原理图和厂家 ESP-IDF 5.1.2 BSP 的真实引脚、ES8388、XL9555、ST7789 配置编写，并加入 5.1.2/5.3+ I2C 兼容分支。由于生成环境没有安装 ESP-IDF 工具链、也没有连接实体开发板，仍需要你在本机完成首次 `idf.py build` 和上板验证；串口若出现编译或服务端错误，请保留从第一条错误开始的完整日志。
