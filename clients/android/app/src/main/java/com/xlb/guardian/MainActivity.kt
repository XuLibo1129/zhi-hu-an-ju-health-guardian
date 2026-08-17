package com.xlb.guardian

import android.content.Context
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Send
import androidx.compose.material.icons.rounded.Favorite
import androidx.compose.material.icons.rounded.HealthAndSafety
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Sensors
import androidx.compose.material.icons.rounded.WarningAmber
import androidx.compose.material.icons.rounded.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.sin

private const val DEFAULT_BASE_URL = "http://192.168.1.100:8090"
private const val DEEPSEEK_API_URL = "https://api.deepseek.com/chat/completions"
private const val DEEPSEEK_MODEL = "deepseek-chat"
private const val PREFS_NAME = "guardian_settings"
private const val PREFS_DEEPSEEK_KEY = "deepseek_api_key"

private val Bg = Color(0xFFF1F4F5)
private val SurfaceWhite = Color.White
private val Border = Color(0xFFDFE5E8)
private val TextMain = Color(0xFF14232B)
private val TextMuted = Color(0xFF65747C)
private val Blue = Color(0xFF2C64D7)
private val Green = Color(0xFF0B8A62)
private val Amber = Color(0xFFAD7417)
private val Red = Color(0xFFD64550)
private val Heart = Color(0xFFDC4C68)
private val Breath = Color(0xFF167F78)

private object Txt {
    const val APP = "健康守护终端"
    const val TITLE = "无感健康守护"
    const val MASTER_CONNECTED = "主控已连接"
    const val MASTER_OFFLINE = "主控未连接"
    const val HOST = "主控地址"
    const val CONNECT = "连接"
    const val REFRESH = "刷新"
    const val HEART_NOW = "当前心率"
    const val BREATH_NOW = "当前呼吸"
    const val HEART_LABEL = "心率"
    const val BREATH_LABEL = "呼吸"
    const val BPM = "次/分"
    const val HUMAN = "检测到人体"
    const val NO_HUMAN = "未检测到人体"
    const val RADAR_ON = "生命体征雷达已连接"
    const val RADAR_OFF = "生命体征雷达未连接"
    const val FUSION = "融合跌倒判断"
    const val FUSION_ALARM = "已触发融合跌倒报警"
    const val FUSION_NORMAL = "未确认跌倒"
    const val HISTORY = "历史趋势"
    const val HISTORY_SUB = "最近五天平均"
    const val NO_HISTORY = "暂无历史数据"
    const val NODES = "节点状态"
    const val ONLINE = "已连接"
    const val OFFLINE = "未连接"
    const val FALL = "疑似跌倒"
    const val NORMAL = "状态正常"
    const val K230 = "K230 视觉"
    const val K230_SUB = "倒装摄像头跌倒识别"
    const val LD = "LD6002C 雷达"
    const val LD_SUB = "独立雷达跌倒节点"
    const val CSI = "CSI 无线感知"
    const val CSI_SUB = "ESP32-S3 双机辅助感知"
    const val AGENT = "询问设备 Agent"
    const val ASK = "询问 ESP-Claw"
    const val QUERY = "现在设备状态怎么样"
    const val WAITING = "等待连接主控终端"
    const val CONNECT_FAIL = "无法连接主控终端"
    const val ASK_FAIL = "Agent 请求失败"
    const val NO_REPLY = "Agent 已响应，但没有返回文本。"
    const val ANALYSIS = "AI 健康分析"
    const val ANALYSIS_SUB = "DeepSeek"
    const val API_KEY = "DeepSeek API Key"
    const val ANALYZE = "一键分析"
    const val ANALYZING = "分析中"
    const val ANALYSIS_DEFAULT = "连接主控终端后，可根据当前数据生成健康建议。"
    const val ANALYSIS_NO_KEY = "请先填写 DeepSeek API Key。"
    const val ANALYSIS_NO_DATA = "还没有可分析的设备数据，请先连接并刷新主控终端。"
    const val ANALYSIS_FAIL = "健康分析请求失败"
}

data class NodeState(
    val title: String,
    val subtitle: String,
    val online: Boolean = false,
    val fall: Boolean = false,
    val probability: Float = 0f,
)

data class Vitals(
    val radarOnline: Boolean = false,
    val humanPresent: Boolean = false,
    val heartValid: Boolean = false,
    val heartBpm: Float = 0f,
    val breathValid: Boolean = false,
    val breathBpm: Float = 0f,
)

data class Fusion(
    val score: Int = 0,
    val threshold: Int = 65,
    val fallAlarm: Boolean = false,
)

data class HistoryDay(
    val day: String,
    val heartValid: Boolean,
    val heartBpm: Int,
    val heartSamples: Int,
    val breathValid: Boolean,
    val breathBpm: Int,
    val breathSamples: Int,
)

data class DashboardState(
    val connected: Boolean = false,
    val loading: Boolean = false,
    val error: String = "",
    val lastUpdate: String = "--",
    val vitals: Vitals = Vitals(),
    val fusion: Fusion = Fusion(),
    val historyStatus: String = Txt.NO_HISTORY,
    val history: List<HistoryDay> = emptyList(),
    val nodes: List<NodeState> = defaultNodes(),
    val agentReply: String = Txt.WAITING,
)

private fun defaultNodes() = listOf(
    NodeState(Txt.K230, Txt.K230_SUB),
    NodeState(Txt.LD, Txt.LD_SUB),
    NodeState(Txt.CSI, Txt.CSI_SUB),
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                GuardianApp()
            }
        }
    }
}

@Composable
fun GuardianApp() {
    val context = LocalContext.current
    val preferences = remember { context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE) }
    var baseUrl by remember { mutableStateOf(DEFAULT_BASE_URL) }
    var query by remember { mutableStateOf(Txt.QUERY) }
    var deepSeekKey by remember { mutableStateOf(preferences.getString(PREFS_DEEPSEEK_KEY, "") ?: "") }
    var analysisLoading by remember { mutableStateOf(false) }
    var analysisResult by remember { mutableStateOf(Txt.ANALYSIS_DEFAULT) }
    var state by remember { mutableStateOf(DashboardState()) }
    val scope = rememberCoroutineScope()

    fun refresh(silent: Boolean = false) {
        scope.launch {
            if (!silent) state = state.copy(loading = true, error = "")
            runCatching { fetchStatus(baseUrl) }
                .onSuccess { state = it.copy(connected = true, loading = false, error = "") }
                .onFailure { state = state.copy(connected = false, loading = false, error = it.message ?: Txt.CONNECT_FAIL) }
        }
    }

    fun askAgent() {
        scope.launch {
            state = state.copy(loading = true, error = "")
            runCatching { postAgentQuery(baseUrl, query) }
                .onSuccess { state = state.copy(loading = false, agentReply = it, error = "") }
                .onFailure { state = state.copy(loading = false, error = it.message ?: Txt.ASK_FAIL) }
        }
    }

    fun analyzeHealth() {
        val apiKey = deepSeekKey.trim()
        if (apiKey.isBlank()) {
            analysisResult = Txt.ANALYSIS_NO_KEY
            return
        }
        if (!state.connected) {
            analysisResult = Txt.ANALYSIS_NO_DATA
            return
        }

        preferences.edit().putString(PREFS_DEEPSEEK_KEY, apiKey).apply()
        scope.launch {
            analysisLoading = true
            analysisResult = "正在综合当前生命体征、跌倒判断与历史趋势..."
            runCatching { requestHealthAnalysis(apiKey, state) }
                .onSuccess { analysisResult = it }
                .onFailure { analysisResult = "${Txt.ANALYSIS_FAIL}：${it.message ?: "未知错误"}" }
            analysisLoading = false
        }
    }

    LaunchedEffect(baseUrl) {
        refresh(true)
        while (true) {
            delay(5000)
            refresh(true)
        }
    }

    Surface(color = Bg, modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            HeaderCard(state, onRefresh = { refresh(false) })
            AnimatedVisibility(state.error.isNotBlank()) {
                Notice(text = state.error)
            }
            ConnectionPanel(baseUrl, onBaseUrlChange = { baseUrl = it }, onConnect = { refresh(false) })
            OverviewStrip(state)
            VitalsGrid(state.vitals)
            FusionCard(state.fusion, state.nodes)
            HealthAnalysisCard(
                apiKey = deepSeekKey,
                loading = analysisLoading,
                result = analysisResult,
                vitals = state.vitals,
                fusion = state.fusion,
                onApiKeyChange = {
                    deepSeekKey = it
                    preferences.edit().putString(PREFS_DEEPSEEK_KEY, it).apply()
                },
                onAnalyze = { analyzeHealth() },
            )
            HistoryCard(state.history, state.historyStatus)
            SectionTitle(Txt.NODES, "K230 / LD6002C / CSI")
            state.nodes.forEach { NodeCard(it) }
            AgentPanel(query, state.agentReply, onQueryChange = { query = it }, onAsk = { askAgent() })
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun HealthAnalysisCard(
    apiKey: String,
    loading: Boolean,
    result: String,
    vitals: Vitals,
    fusion: Fusion,
    onApiKeyChange: (String) -> Unit,
    onAnalyze: () -> Unit,
) {
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Green.copy(alpha = 0.55f))
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(Txt.ANALYSIS, Txt.ANALYSIS_SUB)
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color(0xFFF8FAFB)),
            ) {
                AnalysisStat(
                    modifier = Modifier.weight(1f),
                    label = Txt.HEART_LABEL,
                    value = if (vitals.heartValid) vitals.heartBpm.toInt().toString() else "--",
                    showDivider = true,
                )
                AnalysisStat(
                    modifier = Modifier.weight(1f),
                    label = Txt.BREATH_LABEL,
                    value = if (vitals.breathValid) vitals.breathBpm.toInt().toString() else "--",
                    showDivider = true,
                )
                AnalysisStat(
                    modifier = Modifier.weight(1f),
                    label = "风险评分",
                    value = fusion.score.toString(),
                    showDivider = false,
                )
            }
            OutlinedTextField(
                value = apiKey,
                onValueChange = onApiKeyChange,
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                label = { Text(Txt.API_KEY) },
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(
                    capitalization = KeyboardCapitalization.None,
                    keyboardType = KeyboardType.Password,
                ),
                shape = RoundedCornerShape(8.dp),
            )
            Button(
                onClick = onAnalyze,
                enabled = !loading,
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(8.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Green)
            ) {
                if (loading) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(18.dp),
                        color = Color.White,
                        strokeWidth = 2.dp,
                    )
                } else {
                    Icon(Icons.Rounded.HealthAndSafety, contentDescription = null)
                }
                Spacer(Modifier.width(8.dp))
                Text(if (loading) Txt.ANALYZING else Txt.ANALYZE)
            }
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .animateContentSize()
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color(0xFFF3F9F6))
                    .padding(14.dp)
            ) {
                Text(result, color = Color(0xFF304249), fontSize = 14.sp, lineHeight = 21.sp)
            }
        }
    }
}

@Composable
private fun AnalysisStat(modifier: Modifier, label: String, value: String, showDivider: Boolean) {
    Row(modifier = modifier, verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(horizontal = 10.dp, vertical = 9.dp)) {
            Text(label, color = TextMuted, fontSize = 10.sp)
            Text(value, color = TextMain, fontSize = 17.sp, fontWeight = FontWeight.ExtraBold)
        }
        if (showDivider) {
            Box(Modifier.width(1.dp).height(34.dp).background(Border))
        }
    }
}

@Composable
private fun HeaderCard(state: DashboardState, onRefresh: () -> Unit) {
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Border)
    ) {
        Row(
            modifier = Modifier
                .padding(16.dp)
                .fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .width(4.dp)
                    .height(66.dp)
                    .clip(RoundedCornerShape(4.dp))
                    .background(Green)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("ESP-Claw Agent", color = Green, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(Txt.TITLE, color = TextMain, fontSize = 25.sp, fontWeight = FontWeight.ExtraBold)
                    Spacer(Modifier.width(10.dp))
                    StatusDot(state.connected)
                }
                Text(
                    "${if (state.connected) Txt.MASTER_CONNECTED else Txt.MASTER_OFFLINE} - ${state.lastUpdate}",
                    color = TextMuted,
                    fontSize = 13.sp,
                    modifier = Modifier.padding(top = 6.dp)
                )
            }
            IconButton(onClick = onRefresh) {
                Icon(Icons.Rounded.Refresh, contentDescription = Txt.REFRESH, tint = Blue)
            }
        }
    }
}

@Composable
private fun StatusDot(connected: Boolean) {
    Box(
        modifier = Modifier
            .size(10.dp)
            .clip(CircleShape)
            .background(if (connected) Green else Red)
    )
}

@Composable
private fun Notice(text: String) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFFFF1F0))
            .padding(14.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Rounded.WarningAmber, contentDescription = null, tint = Red)
            Spacer(Modifier.width(8.dp))
            Text(text, color = Color(0xFFB42318), fontSize = 13.sp)
        }
    }
}

@Composable
private fun ConnectionPanel(baseUrl: String, onBaseUrlChange: (String) -> Unit, onConnect: () -> Unit) {
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFFF8FAFB)),
        border = BorderStroke(1.dp, Border)
    ) {
        Row(
            modifier = Modifier.padding(10.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedTextField(
                value = baseUrl,
                onValueChange = onBaseUrlChange,
                modifier = Modifier.weight(1f),
                singleLine = true,
                label = { Text(Txt.HOST) },
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None),
                shape = RoundedCornerShape(8.dp),
            )
            Button(
                onClick = onConnect,
                shape = RoundedCornerShape(8.dp),
                colors = ButtonDefaults.buttonColors(containerColor = TextMain),
                modifier = Modifier.height(56.dp)
            ) {
                Icon(Icons.Rounded.Wifi, contentDescription = null)
                Spacer(Modifier.width(4.dp))
                Text(Txt.CONNECT)
            }
        }
    }
}

@Composable
private fun OverviewStrip(state: DashboardState) {
    val onlineCount = state.nodes.count { it.online }
    val quality = when {
        state.vitals.heartValid && state.vitals.breathValid -> "数据完整"
        state.vitals.heartValid || state.vitals.breathValid -> "部分有效"
        else -> "暂无数据"
    }
    val monitorText = when {
        !state.connected -> "等待连接"
        state.fusion.fallAlarm -> "发现跌倒风险"
        else -> "持续监护中"
    }
    val monitorColor = if (state.fusion.fallAlarm) Red else Green

    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Border),
    ) {
        Column {
            Row(Modifier.fillMaxWidth()) {
                OverviewCell(
                    modifier = Modifier.weight(1f),
                    label = "监护状态",
                    value = monitorText,
                    valueColor = monitorColor,
                    emphasized = true,
                    endDivider = true,
                    bottomDivider = true,
                )
                OverviewCell(
                    modifier = Modifier.weight(1f),
                    label = "在线节点",
                    value = "$onlineCount / 3",
                    bottomDivider = true,
                )
            }
            Row(Modifier.fillMaxWidth()) {
                OverviewCell(
                    modifier = Modifier.weight(1f),
                    label = "生命体征",
                    value = quality,
                    endDivider = true,
                )
                OverviewCell(
                    modifier = Modifier.weight(1f),
                    label = "数据更新",
                    value = state.lastUpdate,
                )
            }
        }
    }
}

@Composable
private fun OverviewCell(
    modifier: Modifier,
    label: String,
    value: String,
    valueColor: Color = TextMain,
    emphasized: Boolean = false,
    endDivider: Boolean = false,
    bottomDivider: Boolean = false,
) {
    Box(
        modifier = modifier
            .background(if (emphasized) Color(0xFFF3F9F6) else SurfaceWhite),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 13.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (emphasized) {
                Box(Modifier.size(9.dp).clip(CircleShape).background(valueColor))
                Spacer(Modifier.width(9.dp))
            }
            Column(Modifier.weight(1f)) {
                Text(label, color = TextMuted, fontSize = 10.sp)
                Text(value, color = valueColor, fontSize = 14.sp, fontWeight = FontWeight.Bold)
            }
        }
        if (endDivider) {
            Box(Modifier.align(Alignment.CenterEnd).width(1.dp).height(46.dp).background(Border))
        }
        if (bottomDivider) {
            Box(Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(1.dp).background(Border))
        }
    }
}

@Composable
private fun VitalsGrid(vitals: Vitals) {
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        MetricCard(
            modifier = Modifier.weight(1f),
            iconColor = Heart,
            title = Txt.HEART_NOW,
            value = if (vitals.heartValid) vitals.heartBpm.toInt().toString() else "--",
            valid = vitals.heartValid,
            isBreath = false,
            note = if (vitals.humanPresent) Txt.HUMAN else Txt.NO_HUMAN,
            icon = { Icon(Icons.Rounded.Favorite, contentDescription = null, tint = Heart) }
        )
        MetricCard(
            modifier = Modifier.weight(1f),
            iconColor = Breath,
            title = Txt.BREATH_NOW,
            value = if (vitals.breathValid) vitals.breathBpm.toInt().toString() else "--",
            valid = vitals.breathValid,
            isBreath = true,
            note = if (vitals.radarOnline) Txt.RADAR_ON else Txt.RADAR_OFF,
            icon = { Icon(Icons.Rounded.Sensors, contentDescription = null, tint = Breath) }
        )
    }
}

@Composable
private fun MetricCard(
    modifier: Modifier,
    iconColor: Color,
    title: String,
    value: String,
    valid: Boolean,
    isBreath: Boolean,
    note: String,
    icon: @Composable () -> Unit,
) {
    Card(
        modifier = modifier,
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Border)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(30.dp)
                        .clip(CircleShape)
                        .background(iconColor.copy(alpha = 0.1f)),
                    contentAlignment = Alignment.Center,
                ) {
                    icon()
                }
                Spacer(Modifier.width(8.dp))
                Text(title, color = TextMuted, fontSize = 12.sp)
            }
            Row(verticalAlignment = Alignment.Bottom, modifier = Modifier.padding(top = 12.dp)) {
                Text(value, color = iconColor, fontSize = 34.sp, fontWeight = FontWeight.ExtraBold)
                Spacer(Modifier.width(4.dp))
                Text(Txt.BPM, color = TextMuted, fontSize = 12.sp, modifier = Modifier.padding(bottom = 7.dp))
            }
            MiniWave(valid = valid, isBreath = isBreath, color = iconColor)
            Text(note, color = TextMuted, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun MiniWave(valid: Boolean, isBreath: Boolean, color: Color) {
    Canvas(
        modifier = Modifier
            .fillMaxWidth()
            .height(44.dp)
            .padding(vertical = 7.dp),
    ) {
        val count = 36
        val step = size.width / (count - 1)
        val center = size.height * 0.55f
        val path = Path()
        for (index in 0 until count) {
            val x = index * step
            val y = if (!valid) {
                center
            } else if (isBreath) {
                center + sin(index / 3.4).toFloat() * size.height * 0.30f
            } else {
                val shape = floatArrayOf(0f, 0.03f, -0.04f, 0.12f, -0.65f, 0.35f, -0.10f, 0.03f, 0f)
                center + shape[index % shape.size] * size.height
            }
            if (index == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        drawPath(
            path = path,
            color = color.copy(alpha = if (valid) 0.88f else 0.24f),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round),
        )
    }
}

@Composable
private fun HistoryCard(history: List<HistoryDay>, status: String) {
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Border)
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(Txt.HISTORY, "${Txt.HISTORY_SUB} - $status")
            if (history.isEmpty()) {
                Text(Txt.NO_HISTORY, color = TextMuted, fontSize = 13.sp)
            } else {
                history.forEach { item -> HistoryRow(item) }
            }
        }
    }
}

@Composable
private fun HistoryRow(item: HistoryDay) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(shortDay(item.day), modifier = Modifier.width(58.dp), color = TextMain, fontWeight = FontWeight.Bold)
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            HistoryLine(Txt.HEART_LABEL, item.heartValid, item.heartBpm, 140, Heart)
            HistoryLine(Txt.BREATH_LABEL, item.breathValid, item.breathBpm, 30, Breath)
        }
    }
}

@Composable
private fun HistoryLine(label: String, valid: Boolean, value: Int, max: Int, color: Color) {
    val target = if (valid) (value.toFloat() / max.toFloat()).coerceIn(0.08f, 1f) else 0f
    val progress by animateFloatAsState(targetValue = target, animationSpec = tween(320), label = "history-progress")
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = TextMuted, fontSize = 12.sp, modifier = Modifier.width(34.dp))
        LinearProgressIndicator(
            progress = { progress },
            modifier = Modifier
                .weight(1f)
                .height(6.dp)
                .clip(RoundedCornerShape(50)),
            color = color,
            trackColor = Color(0xFFEEF1F5),
        )
        Text(if (valid) value.toString() else "--", color = TextMuted, fontSize = 12.sp, modifier = Modifier.width(34.dp).padding(start = 8.dp))
    }
}

@Composable
private fun FusionCard(fusion: Fusion, nodes: List<NodeState>) {
    val alarm = fusion.fallAlarm
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = if (alarm) Color(0xFFFFF6F4) else SurfaceWhite),
        border = BorderStroke(1.dp, Border)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .width(4.dp)
                        .height(54.dp)
                        .clip(RoundedCornerShape(4.dp))
                        .background(if (alarm) Red else Blue)
                )
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("多模态融合判断", color = TextMuted, fontSize = 12.sp)
                    Text(
                        if (alarm) Txt.FUSION_ALARM else Txt.FUSION_NORMAL,
                        color = TextMain,
                        fontSize = 20.sp,
                        fontWeight = FontWeight.ExtraBold,
                        modifier = Modifier.padding(top = 4.dp)
                    )
                }
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(8.dp))
                        .background(if (alarm) Red else Blue)
                        .padding(horizontal = 16.dp, vertical = 10.dp)
                ) {
                    Text("${fusion.score}/${fusion.threshold}", color = Color.White, fontWeight = FontWeight.Bold)
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                nodes.take(3).forEach { node ->
                    FusionSource(node = node, modifier = Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun FusionSource(node: NodeState, modifier: Modifier) {
    val tone = when {
        !node.online -> TextMuted
        node.fall -> Red
        else -> Green
    }
    val shortName = when {
        node.title.startsWith("K230") -> "视觉"
        node.title.startsWith("LD6002C") -> "雷达"
        else -> "CSI"
    }
    Row(
        modifier = modifier
            .clip(RoundedCornerShape(7.dp))
            .background(tone.copy(alpha = 0.07f))
            .padding(horizontal = 9.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(7.dp).clip(CircleShape).background(tone))
        Spacer(Modifier.width(7.dp))
        Column(Modifier.weight(1f)) {
            Text(shortName, color = TextMain, fontSize = 11.sp, fontWeight = FontWeight.Bold, maxLines = 1)
            Text(
                if (!node.online) Txt.OFFLINE else if (node.fall) "风险" else "正常",
                color = TextMuted,
                fontSize = 9.sp,
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun SectionTitle(title: String, subtitle: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.Bottom) {
        Text(title, color = TextMain, fontSize = 19.sp, fontWeight = FontWeight.ExtraBold)
        if (subtitle.isNotBlank()) {
            Text(subtitle, color = TextMuted, fontSize = 12.sp)
        }
    }
}

@Composable
private fun NodeCard(node: NodeState) {
    val p = node.probability.coerceIn(0f, 1f)
    val progress by animateFloatAsState(targetValue = p, animationSpec = tween(320), label = "node-progress")
    val tone = when {
        !node.online -> TextMuted
        node.fall -> Red
        p > 0.45f -> Amber
        else -> Green
    }
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Border)
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f)) {
                    Text(node.title, color = TextMain, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                    Text(node.subtitle, color = TextMuted, fontSize = 12.sp, modifier = Modifier.padding(top = 2.dp))
                }
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(tone.copy(alpha = 0.1f))
                        .padding(horizontal = 8.dp, vertical = 5.dp)
                ) {
                    Text(if (node.online) Txt.ONLINE else Txt.OFFLINE, color = tone, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                }
            }
            Row(Modifier.padding(top = 14.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                Text(if (node.fall) Txt.FALL else Txt.NORMAL, color = TextMuted, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text("${(p * 100).toInt()}%", color = TextMuted, fontSize = 12.sp)
            }
            LinearProgressIndicator(
                progress = { progress },
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp)
                    .height(6.dp)
                    .clip(RoundedCornerShape(50)),
                color = tone,
                trackColor = Color(0xFFEEF1F5),
            )
        }
    }
}

@Composable
private fun AgentPanel(query: String, reply: String, onQueryChange: (String) -> Unit, onAsk: () -> Unit) {
    Card(
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceWhite),
        border = BorderStroke(1.dp, Border)
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(Txt.AGENT, "")
            OutlinedTextField(
                value = query,
                onValueChange = onQueryChange,
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                shape = RoundedCornerShape(8.dp),
            )
            Button(
                onClick = onAsk,
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(8.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Blue)
            ) {
                Icon(Icons.AutoMirrored.Rounded.Send, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(Txt.ASK)
            }
            Text(reply, color = Color(0xFF344054), fontSize = 14.sp, lineHeight = 21.sp)
        }
    }
}

private suspend fun fetchStatus(baseUrl: String): DashboardState = withContext(Dispatchers.IO) {
    val json = httpRequest("${baseUrl.trimEnd('/')}/api/agent/status")
    val root = JSONObject(json)
    val vitalsJson = root.optJSONObject("vitals")
    val fusionJson = root.optJSONObject("fusion")
    val nodesJson = root.optJSONObject("nodes")
    val historyJson = root.optJSONObject("history")

    val vitals = Vitals(
        radarOnline = vitalsJson?.optBoolean("radar_online") == true,
        humanPresent = vitalsJson?.optBoolean("human_present") == true,
        heartValid = vitalsJson?.optBoolean("heart_valid") == true,
        heartBpm = vitalsJson?.optDouble("heart_bpm", 0.0)?.toFloat() ?: 0f,
        breathValid = vitalsJson?.optBoolean("breath_valid") == true,
        breathBpm = vitalsJson?.optDouble("breath_bpm", 0.0)?.toFloat() ?: 0f,
    )

    val fusion = Fusion(
        score = fusionJson?.optInt("score", 0) ?: 0,
        threshold = fusionJson?.optInt("threshold", 65) ?: 65,
        fallAlarm = fusionJson?.optBoolean("fall_alarm") == true,
    )

    DashboardState(
        connected = true,
        lastUpdate = SimpleDateFormat("HH:mm:ss", Locale.CHINA).format(Date()),
        vitals = vitals,
        fusion = fusion,
        historyStatus = if (historyJson?.optBoolean("sd_ready") == true) "SD OK" else historyJson?.optString("status", Txt.NO_HISTORY) ?: Txt.NO_HISTORY,
        history = parseHistory(historyJson),
        nodes = listOf(
            parseNode(nodesJson?.optJSONObject("k230_vision"), Txt.K230, Txt.K230_SUB),
            parseNode(nodesJson?.optJSONObject("ld6002c"), Txt.LD, Txt.LD_SUB),
            parseNode(nodesJson?.optJSONObject("csi"), Txt.CSI, Txt.CSI_SUB),
        )
    )
}

private fun parseHistory(json: JSONObject?): List<HistoryDay> {
    val array = json?.optJSONArray("days") ?: return emptyList()
    return buildList {
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            add(
                HistoryDay(
                    day = item.optString("day", "--"),
                    heartValid = item.optBoolean("heart_valid"),
                    heartBpm = item.optInt("heart_bpm", 0),
                    heartSamples = item.optInt("heart_samples", 0),
                    breathValid = item.optBoolean("breath_valid"),
                    breathBpm = item.optInt("breath_bpm", 0),
                    breathSamples = item.optInt("breath_samples", 0),
                )
            )
        }
    }
}

private fun parseNode(json: JSONObject?, title: String, subtitle: String): NodeState {
    return NodeState(
        title = title,
        subtitle = subtitle,
        online = json?.optBoolean("online") == true,
        fall = json?.optBoolean("fall") == true,
        probability = json?.optDouble("probability", 0.0)?.toFloat() ?: 0f,
    )
}

private suspend fun postAgentQuery(baseUrl: String, query: String): String = withContext(Dispatchers.IO) {
    val payload = JSONObject().put("text", query.ifBlank { "status" }).toString()
    val json = httpRequest("${baseUrl.trimEnd('/')}/api/agent/query", "POST", payload)
    JSONObject(json).optString("reply", Txt.NO_REPLY)
}

private fun buildHealthPrompt(state: DashboardState): String {
    val vitals = state.vitals
    val fusion = state.fusion
    val heart = if (vitals.heartValid) "${vitals.heartBpm.toInt()} 次/分" else "暂无有效数据"
    val breath = if (vitals.breathValid) "${vitals.breathBpm.toInt()} 次/分" else "暂无有效数据"
    val historyText = if (state.history.isEmpty()) {
        "暂无五天历史数据"
    } else {
        state.history.joinToString("；") { day ->
            "${day.day}：心率${if (day.heartValid) day.heartBpm else "--"}，呼吸${if (day.breathValid) day.breathBpm else "--"}"
        }
    }
    val nodeText = state.nodes.joinToString("；") { node ->
        "${node.title}${if (node.online) "在线" else "离线"}${if (node.fall) "，检测到跌倒风险" else ""}"
    }

    return listOf(
        "请根据以下居家老人健康守护终端数据进行一次简短、谨慎的健康分析：",
        "当前心率：$heart",
        "当前呼吸率：$breath",
        "人体状态：${if (vitals.humanPresent) "检测到人体" else "未检测到人体"}",
        "生命体征雷达：${if (vitals.radarOnline) "在线" else "离线"}",
        "跌倒融合结果：${if (fusion.fallAlarm) "已报警" else "未确认跌倒"}，评分 ${fusion.score}/${fusion.threshold}",
        "节点状态：$nodeText",
        "最近五天平均：$historyText",
        "请用简体中文回答，先给出总体状态，再给出3条可执行建议；若数据异常或不足，请明确提醒复测、联系家属或及时就医。不要做确定性医学诊断，控制在220字以内。",
    ).joinToString("\n")
}

private suspend fun requestHealthAnalysis(apiKey: String, state: DashboardState): String = withContext(Dispatchers.IO) {
    val messages = JSONArray()
        .put(JSONObject().put("role", "system").put("content", "你是居家养老健康监测助手。回答要客观、易懂、可执行，并说明设备数据不能替代医生诊断。"))
        .put(JSONObject().put("role", "user").put("content", buildHealthPrompt(state)))
    val payload = JSONObject()
        .put("model", DEEPSEEK_MODEL)
        .put("messages", messages)
        .put("temperature", 0.3)
        .put("max_tokens", 500)
        .put("stream", false)
        .toString()
    val json = httpRequest(
        url = DEEPSEEK_API_URL,
        method = "POST",
        body = payload,
        headers = mapOf("Authorization" to "Bearer $apiKey"),
        connectTimeoutMs = 10000,
        readTimeoutMs = 30000,
    )
    val root = JSONObject(json)
    val choice = root.optJSONArray("choices")?.optJSONObject(0)
    val content = choice?.optJSONObject("message")?.optString("content", "")?.trim().orEmpty()
    if (content.isBlank()) error("DeepSeek 未返回分析文本")
    content
}

private fun shortDay(day: String): String {
    return if (day.length >= 10) day.substring(5) else day.ifBlank { "--" }
}

private fun httpRequest(
    url: String,
    method: String = "GET",
    body: String? = null,
    headers: Map<String, String> = emptyMap(),
    connectTimeoutMs: Int = 4500,
    readTimeoutMs: Int = 6500,
): String {
    val connection = (URL(url).openConnection() as HttpURLConnection).apply {
        requestMethod = method
        connectTimeout = connectTimeoutMs
        readTimeout = readTimeoutMs
        setRequestProperty("Accept", "application/json")
        headers.forEach { (name, value) -> setRequestProperty(name, value) }
        if (body != null) {
            doOutput = true
            setRequestProperty("Content-Type", "application/json; charset=utf-8")
        }
    }

    if (body != null) {
        OutputStreamWriter(connection.outputStream, Charsets.UTF_8).use { it.write(body) }
    }

    val code = connection.responseCode
    val stream = if (code in 200..299) connection.inputStream else connection.errorStream
    val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }.orEmpty()
    connection.disconnect()
    if (code !in 200..299) {
        error("HTTP $code: $text")
    }
    return text
}
