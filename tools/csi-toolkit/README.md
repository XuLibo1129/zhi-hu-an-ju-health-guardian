# ESP32 CSI 无线感知波形工具

这是给 ESP32-S3 Wi-Fi CSI 接收端用的串口波形工具，支持实时看波形、切换动作标签、保存 CSV 数据。

支持的串口格式：

```text
-25,1830,244,185,3200,4100
CSI_DATA,1,aa:bb:cc:dd:ee:ff,-42,...,[...]
```

启动：

```powershell
cd D:\Users\XLB\Desktop\esp32_csi_viewer
.\run.ps1
```

使用：

1. A 板烧录并运行 `wifi_csi_rx`。
2. 启动 `csi_viewer.py`，或直接运行 `run.ps1`。
3. 选择 A 板 COM 口，波特率保持 `115200`，点击“连接”。
4. 采集前选择“空房”“走路”“坐下”“跌倒”“弯腰捡东西”等标签。
5. 设置“采集时长(s)”，点击“开始记录”保存 CSV 数据，到时间会自动停止，默认放在 `data` 文件夹。
6. CSV 使用带 BOM 的 UTF-8 保存，直接用 Excel 打开中文不会乱码。

如果之前保存过乱码 CSV，可以转换旧文件：

```powershell
cd D:\Users\XLB\Desktop\esp32_csi_viewer
python .\fix_csv_encoding.py
```

训练 Wi-Fi CSI 辅助跌倒模型：

```powershell
cd D:\Users\XLB\Desktop\esp32_csi_viewer
python .\train_csi_model.py
```

默认训练二分类模型：`fall` / `non_fall`。输出会保存到 `models` 文件夹。

默认训练会自动比较随机森林和 ExtraTrees，并选择交叉验证表现更好的模型。当前建议稳定运行阈值是 `0.50`，适合作为主终端融合视觉/雷达时的辅助信号。如果只想查看六分类效果：

```powershell
python .\train_csi_model.py --mode multiclass
```

实时推理：

推荐直接打开图形界面：

```powershell
.\start_realtime_gui.bat
```

也可以手动运行：

```powershell
python .\csi_realtime_gui.py
```

界面里可以选择 A 板串口、选择模型、调阈值、调连续确认窗口，并实时查看跌倒概率。
实时推理现在带有开场环境校准和运动门控。点击“开始UDP接收”后，前几个校准窗口内尽量保持空房/无人移动，让软件学习当前 AP+STA、手机热点和房间状态的基线。日志里的“动态阈值”会随基线自动抬高，用来压住空房误报。

如果 A 板不想一直插电脑串口，可以走 UDP：

1. A 板 `wifi_csi_rx` 当前把六路特征发送到电脑的 `34567` UDP 端口。
2. 电脑与 A 板 STA 连接同一个路由器或手机热点。
3. 打开实时推理界面后会自动监听 UDP `34567`；停止后也可手动点击“开始UDP接收”。
4. B 板继续连接 A 板热点并发送 UDP 包，界面收到数据后会直接进入模型判断。

命令行 UDP 接收也可以这样运行：

```powershell
python .\csi_realtime_infer.py --udp-port 34567
```

正式接主终端时建议切到同一个路由器网络：

1. 电脑、主终端 ESP32-P4 连接同一个 2.4GHz 路由器/手机热点。
2. A 板在 `idf.py menuconfig` 里打开 `Also connect A board to router as STA`，填路由器 SSID/密码。
3. 把 `CSI feature UDP target IP` 从默认 `192.168.4.255` 改成电脑在路由器里的 IP。
4. B 板仍然只连接 A 板热点 `CSI_RX_AP`，不需要改。
5. GUI 继续点“开始UDP接收”，识别结果再通过“主终端URL”发给 ESP32-P4。

如果 AP+STA 模式下效果仍然差，直接在最终拓扑下补采数据：

1. 打开 `csi_viewer.py` 或 `run.ps1`。
2. 不选串口，UDP端口保持 `34567`，点击“UDP连接”。
3. 在当前 A板AP+STA、B板连接A板、电脑连接手机热点的状态下，重新采集 `空房`、`站立`、`走路`、`跌倒` 等短样本。
4. 重新运行 `python .\train_csi_model.py`，再用最新模型做实时测试。

发送结果到主终端：

1. 主终端开一个 HTTP 接收接口，例如 `http://主终端IP:8090/api/sensor/csi`。
2. 在实时推理界面的“主终端URL”里填入这个地址。
3. 点击“测试发送”，日志显示成功后再开始实时检测。

发送的 JSON 格式大致如下：

```json
{
  "sensor": "esp32_s3_wifi_csi",
  "node_id": "wifi_csi_01",
  "time": "2026-07-06T15:56:00",
  "fall_probability": 0.82,
  "smooth_probability": 0.76,
  "threshold": 0.70,
  "alarm": true,
  "state": "疑似跌倒",
  "samples": 250
}
```

命令行版本仍然保留：

```powershell
python .\csi_realtime_infer.py --port COM8
```

把 `COM8` 换成 A 板实际串口。脚本会加载 `models` 文件夹里最新的二分类模型，输出 `p_fall`、平滑概率和 `正常/预警中/疑似跌倒`。默认阈值是 `0.50`，连续 4 个窗口超过阈值才报警。GUI 里也提供“稳定运行”和“灵敏测试”两个预设按钮。

如果现场仍然误报，先用更保守的参数：

```powershell
python .\csi_realtime_infer.py --port COM8 --threshold 0.85 --confirm-windows 6
```

用已有 CSV 回放测试：

```powershell
python .\csi_realtime_infer.py --replay .\data\csi_20260706_152250_跌倒.csv --print-normal
```
