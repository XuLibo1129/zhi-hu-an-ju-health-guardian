# 健康守护终端安卓 App

技术栈：

```text
Kotlin + Jetpack Compose + Material 3
```

默认主控地址示例：

```text
http://192.168.1.100:8090
```

用 Android Studio 打开本目录：

```text
当前目录
```

工程已经允许局域网 HTTP 明文访问：

```xml
android:usesCleartextTraffic="true"
```

“AI 健康分析”直接请求 DeepSeek，API Key 在 App 中填写并保存在本机设置中，不会写入项目源码。

使用工程自带的 Gradle Wrapper 构建：

```powershell
.\gradlew.bat assembleDebug
```
