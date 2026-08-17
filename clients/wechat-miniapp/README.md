# 健康守护终端微信小程序

默认主控地址：

```text
http://192.168.1.100:8090
```

入口页面：

```text
miniprogram/pages/index/index
```

开发者工具中如需访问局域网 HTTP 地址，调试阶段请在详情里关闭“校验合法域名、web-view、TLS 版本以及 HTTPS 证书”。

“AI 健康分析”直接请求 DeepSeek。发布前需要在微信公众平台将以下地址加入 request 合法域名：

```text
https://api.deepseek.com
```

API Key 在小程序页面中填写并保存在微信本地存储，不会写入项目源码。

检查 TypeScript：

```powershell
npm run typecheck
```
