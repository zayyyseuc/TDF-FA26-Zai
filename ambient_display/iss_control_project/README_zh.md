# ISS Control — 手机网页切换 Live API / Replay（基于现有 Feather V2 原型）

## 功能

- **外圈 NeoPixel / A0, GPIO26**：仅一颗星光，根据 ISS 经度移动，移动时淡入淡出、循环变色。
- **内圈 NeoPixel / A1, GPIO25**：两颗随机独立的渐变呼吸光。
- **DA7280 Qwiic / SDA+SCL**：经度累计变化触发短振动，可通过网页调阈值、振动强度或测试单次脉冲。
- **Live API**：通过一个有互联网访问权限的 Wi-Fi 调用 `api.wheretheiss.at`，间隔约 1.2s。
- **Replay**：使用你之前抓取的真实 `iss_data.json`；保存为 Arduino header，重放速度可从 1×–12× 调节。无需重新请求 API。
- **控制网页**：ESP32 自建 `ISS-Control` Wi-Fi，手机访问 `http://192.168.4.1`。

## 1. 准备文件

在 Arduino IDE 中打开 `iss_control/iss_control.ino`，**保持同一文件夹**中的 `web_assets.h`。

将 `iss_control/secrets.h.example` 复制为同目录下的 `secrets.h`，填写自己的**上游互联网 Wi-Fi** 账号密码：

```cpp
#pragma once
#define SECRET_SSID "你的2.4GHz Wi-Fi名称"
#define SECRET_PASSWORD "你的密码"
```

不要把带有真实密码的 `secrets.h` 上传到公共 GitHub 仓库。

需要安装 Arduino 库：
1. **Adafruit NeoPixel**
2. **ArduinoJson**（v7）
3. **SparkFun Qwiic Haptic Driver DA7280**（其头文件 `Haptic_Driver.h`，不要装成 DRV2605L 库）

ESP32 的 `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Wire`, `WebServer` 属于 ESP32 Arduino Core。

## 2. 把以前的 JSON 导入 Replay

把**之前抓取的真实** `iss_data.json` 放在项目根目录（与本 README 相同层级），然后在 Terminal 中进入项目根目录并运行：

```bash
python3 tools/json_to_header.py
```

或指定历史文件路径：

```bash
python3 tools/json_to_header.py ~/Downloads/iss_data.json
```

脚本会自动生成 `iss_control/iss_data.h`。**无需再次抓取 API**，也不要使用旧 JSON 中按 8 个灯计算的 `led_position` 字段；代码只读取历史 `latitude/longitude/timestamp`，按照当前外圈 LED 数量重算。

没找到 `iss_data.h` 时固件仍可编译，但 Replay 按钮会禁用（页面会显示 saved records = 0）。

## 3. 配置板子和物理连接

Arduino IDE → Board = **Adafruit Feather ESP32 V2**，Upload Speed 建议 **115200**。

在 `iss_control.ino` 顶部把 `OUTER_LEDS`、`INNER_LEDS` 改成真实数量。内圈至少 2 颗。

| 设备 | ESP32 |
|---|---|
| 外圈 DIN | A0 / GPIO26（可串联 330Ω 数据电阻） |
| 内圈 DIN | A1 / GPIO25（可串联 330Ω 数据电阻） |
| 两组 NeoPixel 5V | 合适的外部 5V 电源（较长灯条更推荐） |
| 两组 NeoPixel GND | 电源 GND 与 ESP32 GND **共地** |
| DA7280 Qwiic | Feather V2 的 STEMMA QT（3.3V I²C），或 SDA/SCL/3V/GND |

建议在两条数据线上分别使用 3.3V→5V 逻辑电平转换。**不要**将外圈较长灯条的大电流经由 ESP32 的 3.3V 引脚供电。

## 4. 上传并访问手机控制页面

1. 上传 `iss_control.ino`。Serial Monitor = 115200。
2. ESP32 会创建 **ISS-Control** Wi-Fi，默认密码在代码顶部 `AP_PASSWORD` 中（出于演示方便，**展示前请更换密码**）。
3. 手机连接 `ISS-Control` Wi-Fi。如果手机提示该网络没有互联网，选择“保持连接”。
4. 在 Safari/Chrome 输入 **`http://192.168.4.1`**，注意 HTTP，而非 HTTPS。
5. 页面可切换 **Live API / Replay**；调 Replay Speed（1×–12×）、Longitude Step（0.1–10°）、Haptic Intensity；按 “Test single pulse” 可测试马达。
6. 两组 NeoPixel 一直显示原有动画，切换时会重置经度累计值，避免离线轨迹切换造成错误振动。

### Demo 现场的网络架构

ESP32 使用 **AP+STA 双模式**：
- AP：手机连接 ESP32 的 `ISS-Control` Wi-Fi，访问控制网页；**AP 没互联网也不妨碍页面和 Replay 工作**。
- STA：ESP32 使用 `secrets.h` 中的 Wi-Fi 单独访问 ISS API。此网络**必须确实能连到互联网**。

如果只有一部手机，而且该手机已经连接 ESP32 AP，它通常不能再同时作为给 ESP32 提供网络的手机热点。可以让另一部手机开热点（ESP32 STA 连接），或准备小型 2.4GHz Wi-Fi 路由器。

**校园 Wi-Fi 的客户端隔离**不会影响手机通过 ESP32 的专属 AP 访问 `192.168.4.1`，但校园认证要求可能阻止 ESP32 STA 上网。确保演示前做过真正的 Live API 测试。

## 5. PWA 到底怎么做？

项目 `frontend/` 包含：
- `index.html`：手机控制页面（HTML/CSS/JavaScript，无 React 依赖）
- `manifest.webmanifest`：PWA 安装元数据
- `sw.js`：Service Worker，缓存 UI 静态文件；**不缓存 `/api/...` 实时数据**
- `icon-192.png`, `icon-512.png`：App 图标

页面默认由 ESP32 内置 Web Server 托管。浏览器从 `http://192.168.4.1` 打开时能直接控制，**但 HTTP 局域网 IP 并不是安全上下文，不能保证 PWA Service Worker / 正规安装能力**。手机通过 Safari 的“添加到主屏幕”可尝试创建网页快捷方式，但不要假设这等同于正式 HTTPS 安装式 PWA。

要做完整、可安装的 PWA，可将 frontend 部署在**HTTPS 同源服务器**，并让服务器将 `/api/*` 安全反向代理到 ESP32。这通常需要同一个可达局域网中的电脑/Raspberry Pi、或经鉴权的远程网关。**不要**简单把前端扔到 GitHub Pages 后直接 `fetch('http://192.168.4.1')`；HTTPS 页面请求 HTTP 设备有 mixed-content / private-network access 限制，不可靠。当前原型的**本地网页方案更适合课堂展示**。

## 6. 修改页面样式（不需 LittleFS）

页面文件嵌入到了 `iss_control/web_assets.h` 中。如果修改了 `frontend/index.html`, `sw.js`, `manifest.webmanifest` 或图标，需在项目根目录运行：

```bash
python3 tools/build_web_assets.py
```

这会重新生成 `iss_control/web_assets.h`；然后重新上传 `.ino`。Python 脚本用标准库，不需要 Node / npm。

## 7. Debug

Serial Monitor 里的：
- `Control Wi-Fi: ISS-Control` + `Web UI: http://192.168.4.1` → 手机本地控制界面已经启动。
- `STA Wi-Fi connected: ...` → ESP32 连接了上游 Wi-Fi，但不等于 API 必定可达。
- `LIVE ISS: lat=... lon=...` → API 返回了有效新数据。
- `DA7280 ready` → 振动驱动初始化成功。

**局限与安全**：运行了本地 HTTP 控制服务；通过 AP WPA2 密码保护，但不适合公开互联网暴露。现有示例为了保留你的工作原型沿用了 `client.setInsecure()`，实时 HTTPS 请求未验证服务器证书；正式展示之外的产品版本应使用证书校验或适当信任链。没有实际硬件测试，请在正式展示前排练一次。
