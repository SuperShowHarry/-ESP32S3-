## sdkconfig 配置与生成步骤

`sdkconfig` 是 ESP-IDF 根据当前开发板和用户配置自动生成的文件，不需要下载，也不需要手动创建。

### 一、打开工程

1. 安装 Visual Studio Code 和 ESP-IDF 扩展。
2. ESP-IDF 版本选择 5.1.2。
3. 使用 Visual Studio Code 打开 `dnesp32s3_doubao_voice` 文件夹。
4. 必须打开包含 `CMakeLists.txt`、`main` 和 `sdkconfig.defaults` 的这一层目录。

### 二、选择芯片型号

1. 按下 `Ctrl + Shift + P` 打开命令面板。
2. 搜索并选择“ESP-IDF: Set Espressif Device Target”。
3. 芯片型号选择 `esp32s3`。
4. 等待 ESP-IDF 完成项目配置。

### 三、打开配置界面

1. 再次按下 `Ctrl + Shift + P`。
2. 搜索并选择“ESP-IDF: SDK Configuration Editor”。
3. 等待浏览器式配置页面打开。
4. 在配置页面中找到“DNESP32S3 Doubao Voice Assistant”。

### 四、填写 Wi-Fi 配置

进入项目配置菜单后填写：

1. “Wi-Fi SSID”：填写自己的 2.4GHz Wi-Fi 名称。
2. “Wi-Fi password”：填写自己的 Wi-Fi 密码。

ESP32-S3 通常不能直接连接只开启了 5GHz 的 Wi-Fi，请确认路由器已经启用 2.4GHz 网络。

### 五、选择屏幕型号

在“SPI ST7789 display”中，根据实际屏幕选择：

1. 2.4 英寸屏幕选择 `320×240 ST7789`。
2. 1.3 英寸屏幕选择 `240×240 ST7789`。

正点原子 DNESP32S3 配套的 2.4 英寸屏幕通常选择 `320×240 ST7789`。

### 六、配置豆包鉴权

根据火山引擎控制台提供的凭据选择一种鉴权方式，不要同时混用。

#### 新版 API Key 方式

1. “Doubao authentication mode”选择“New console API Key”。
2. 在“Doubao Voice API Key”中填写自己的豆包语音 API Key。
3. 此方式不需要填写旧版 App ID 和 Access Token。

#### 旧版 App ID 和 Access Token 方式

1. “Doubao authentication mode”选择“Legacy App ID + Access Token”。
2. 在“Legacy App ID”中填写豆包语音服务的 App ID。
3. 在“Legacy Access Token”中填写豆包语音服务的 Access Token。
4. 此方式不需要填写新版 API Key。

注意：这里必须使用豆包语音服务提供的凭据，不能填写 RTC 实时音视频的 AppKey 或临时 Token。

### 七、检查豆包服务参数

建议初次使用时保留工程默认参数：

1. “Realtime voice resource ID”保持为 `volc.speech.dialog`。
2. “Realtime dialogue model”使用工程默认模型。
3. “Voice ID”使用工程默认音色。
4. “Assistant name”可以填写希望显示的助手名称。
5. “System role”可以设置助手使用中文回答以及回答长度。
6. “Speaker volume”可以根据需要调节扬声器音量。

模型和音色必须互相兼容。如果出现音色无效错误，应恢复工程默认模型和默认音色。

### 八、保存并生成 sdkconfig

1. 完成配置后，点击配置页面顶部的“Save”或“保存”按钮。
2. 等待页面提示配置保存成功。
3. 返回 Visual Studio Code 的资源管理器。
4. 工程根目录会自动出现 `sdkconfig` 文件。
5. 如果没有立即显示，可以刷新资源管理器或重新打开工程。
6. 第一次执行项目构建时，ESP-IDF 也会根据 `sdkconfig.defaults` 自动生成 `sdkconfig`。

生成位置如下：

`dnesp32s3_doubao_voice/sdkconfig`

### 九、编译和烧录

1. 在 Visual Studio Code 底部选择开发板对应的串口。
2. 点击 ESP-IDF 的“Build Project”按钮编译工程。
3. 编译成功后点击“Flash Device”烧录固件。
4. 烧录完成后打开“Monitor Device”查看串口日志。
5. 开发板重新启动后，会按照 `sdkconfig` 中保存的设置连接 Wi-Fi 和豆包服务。

### 十、修改配置

以后需要更换 Wi-Fi、豆包密钥、屏幕型号或音量时：

1. 重新打开“ESP-IDF: SDK Configuration Editor”。
2. 修改对应选项。
3. 点击“Save”保存。
4. 重新编译并烧录程序。

不建议直接手工修改 `sdkconfig`，因为重新选择芯片、清理工程或更新配置后，ESP-IDF 可能重新生成该文件。

### 十一、安全说明

`sdkconfig` 可能包含 Wi-Fi 密码、API Key、App ID 和 Access Token，因此本项目不会将它上传到公开仓库。

每位使用者都应按照以上步骤生成自己的 `sdkconfig`。请勿将该文件、完整密钥或密码发布到 GitHub、Gitee、Issue、截图或串口日志中。
