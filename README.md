# 基于 ESP32-P4 的多模态 AI 无感健康守护与交互终端系统

面向居家养老和独居老人场景的多模态健康守护终端。系统以 ESP32-P4 主终端为核心，融合视觉节点、毫米波雷达、Wi-Fi CSI 无线感知、语音交互和 AI 分析能力，在不要求老人佩戴设备的情况下完成生命体征监测、跌倒风险识别、异常告警与远程查看。

本项目曾获**第九届全国大学生嵌入式芯片与系统设计竞赛东部赛区一等奖、全国三等奖（乐鑫赛道）**。

## 功能概览

- 主终端：ESP32-P4 + LVGL 触摸界面，显示心率、呼吸率、时间、天气、历史趋势和告警状态。
- 视觉节点：K230 运行 YOLOv5n 跌倒检测模型，通过单向 UART 将 `FALL/NOFALL` 结果发送给主终端。
- 毫米波节点：读取雷达输出，进行人体、心率、呼吸和跌倒状态采集，通过 HTTP 上报主终端。
- Wi-Fi CSI 节点：两块 ESP32-S3 构成 CSI 发射/接收链路，提取特征并由电脑端模型完成辅助判断，再通过 HTTP 上报结果。
- 多模态判定：主终端对视觉、毫米波和 CSI 的最新有效状态进行时间新鲜度检查、主证据与辅助证据融合和告警锁存。
- AI 交互：主终端通过可配置的 OpenAI-compatible LLM、ASR、TTS 服务实现语音交互；网页、微信小程序和 Android 客户端支持状态查询、历史数据和 DeepSeek 健康分析。
- 求助：A7670C 通过 UART 提供电话、短信和自动告警能力，手机号等私密配置仅在本地 menuconfig 中填写。

## 仓库结构

```text
firmware/
  main-terminal/       ESP32-P4 主终端
  ld6002c-node/        ESP32-P4 毫米波雷达节点
  wifi-csi-tx/         ESP32-S3 CSI 发射端
  wifi-csi-rx/         ESP32-S3 CSI 接收端
vision/k230/           K230 视觉跌倒检测脚本
tools/csi-toolkit/     CSI 采集、训练、实时推理工具
clients/
  web-dashboard/       网页客户端
  wechat-miniapp/      微信小程序
  android/             Android 客户端
hardware/enclosures/   主终端和节点外壳打印件
docs/                  系统架构和配置说明
```

现场模拟跌倒网页、模拟 HTTP 服务、演示覆盖接口、模型权重、采集数据、构建产物和本地语音中转服务均不纳入公开仓库。

## 快速开始

### ESP-IDF 固件

分别进入 `firmware` 下的工程，使用对应 ESP-IDF 环境：

```powershell
idf.py set-target esp32p4
idf.py menuconfig
idf.py build flash monitor
```

主终端和雷达节点使用 ESP32-P4；CSI 发射端和接收端使用 ESP32-S3。首次构建前，请在 menuconfig 中填写 Wi-Fi、主终端 URL、云端 API URL、模型名和密钥。仓库只提供空配置或示例地址。

### CSI 工具

CSI 工具要求 Python 3.10+。先采集数据，再在本地训练模型：

```powershell
cd tools\csi-toolkit
python -m pip install -r requirements.txt
python train_csi_model.py
python csi_realtime_gui.py
```

采集数据和训练权重存放在本地的 `data/`、`models/` 目录，并已被 `.gitignore` 排除。CSI 结果通过界面中的主终端 URL 使用 HTTP POST 发送到 `/api/sensor/csi`。

### 网页客户端

```powershell
cd clients\web-dashboard
python -m http.server 8080
```

浏览器访问 `http://127.0.0.1:8080`，在页面中填写主终端地址。微信小程序和 Android 客户端的使用说明分别见各目录 README。

## 配置与安全

请阅读 [docs/configuration.md](docs/configuration.md)。不要提交 `sdkconfig`、`.env`、API Key、手机号、Wi-Fi 密码、局域网真实 IP、模型权重和数据集。对外部署网页时，不建议将 DeepSeek Key 直接放在浏览器；应改用受控后端代理。

## 许可证

代码和文档采用 MIT License，硬件外壳文件按同一许可证发布。第三方 SDK、字体、模型运行时和传感器资料仍受其原始许可证或厂商条款约束。
