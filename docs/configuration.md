# 配置说明

## 主终端

进入 `firmware/main-terminal` 后执行 `idf.py menuconfig`，重点配置：

- `Dual Agent Console -> Wi-Fi SSID / password`
- `Cloud LLM -> URL / model / API key`
- `Cloud ASR -> URL / model / API key`
- `Cloud TTS -> URL / model / voice / API key`
- `A7670C -> emergency phone`，以及自动短信、自动电话开关
- `K230 vision UART` 和 `Radar UART` 引脚

主终端默认天气位置是南京市江北新区；经纬度和天气站点可以在 menuconfig 调整。

## 毫米波节点

在 `firmware/ld6002c-node` 配置：

- Wi-Fi SSID 和密码
- 主终端 URL，例如 `http://192.168.1.100:8090/api/sensor/ld6002c`
- 节点 ID、UART 引脚和上报周期

仓库中的 `192.168.1.100` 只是示例地址，必须换成实际主终端地址。

## CSI 节点

接收端建立 SoftAP，发射端连接该 AP。接收端可选用 STA 同时连接路由器或手机热点，再把特征 UDP 发到电脑。电脑端推理工具将判定结果通过 HTTP 发给主终端。

不要把真实 Wi-Fi 密码、电脑 IP 或主终端 IP 写入源码。推荐在 `idf.py menuconfig` 中设置，发布前保留 `sdkconfig.defaults` 中的空值或示例值。

## K230

将 YOLOv5n 跌倒检测模型以 `yolov5n-falldown.kmodel` 放在设备的 `/sdcard/kmodel/` 下，再运行 `vision/k230/fall_detect.py`。模型权重不在仓库中；需要根据 K230/CanMV 版本安装相应运行时和驱动。

## 客户端

网页、小程序和 Android 默认使用 `http://192.168.1.100:8090` 作为示例地址。实际使用时在网页输入框、小程序设置或 Android 设置页中修改为主终端当前地址。移动端访问局域网 HTTP 服务时，还需要满足微信开发者工具、Android 明文流量和目标网络的权限要求。

## 密钥管理

API Key、Access Token、手机号和语音服务凭据只能放在本地配置或受控服务中。不要把它们写入 Git、截图、日志、README 或固件二进制。
