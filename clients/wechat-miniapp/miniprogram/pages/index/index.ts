const DEFAULT_BASE_URL = 'http://192.168.1.100:8090'
const LEGACY_BASE_URL = ''
const STORAGE_BASE_URL = 'guardian_base_url'
const STORAGE_DEEPSEEK_KEY = 'guardian_deepseek_key'
const DEEPSEEK_API_URL = 'https://api.deepseek.com/chat/completions'
const DEEPSEEK_MODEL = 'deepseek-chat'

const T = {
  online: '已连接',
  offline: '未连接',
  normal: '状态正常',
  fall: '疑似跌倒',
  queryDefault: '现在设备状态怎么样',
  waiting: '等待连接主控终端',
  notConnected: '未连接',
  radarOffline: '雷达未连接',
  noHumanShort: '未检测',
  waitingData: '等待数据',
  refreshing: '刷新中',
  asking: '询问中',
  cannotConnect: '无法连接主控终端',
  noReply: 'Agent 已响应，但没有返回文本。',
  requestFailed: 'Agent 请求失败',
  masterOnline: '主控联网',
  masterOffline: '主控离线',
  radarOnline: '生命体征雷达已连接',
  vitalRadarOffline: '生命体征雷达未连接',
  human: '检测到人体',
  noHuman: '未检测到人体',
  fusionAlarm: '已触发融合跌倒报警',
  fusionNormal: '未确认跌倒',
  connectFailed: '连接失败',
  k230Name: 'K230 视觉',
  k230Sub: '倒装摄像头跌倒识别',
  ldName: 'LD6002C 雷达',
  ldSub: '独立雷达跌倒节点',
  csiName: 'CSI 无线感知',
  csiSub: 'ESP32-S3 双机辅助感知',
  noHistory: '暂无历史数据',
  samples: '次采样',
  analysisDefault: '连接主控终端后，可根据当前数据生成健康建议。',
  analysisNoKey: '请先填写 DeepSeek API Key。',
  analysisNoData: '还没有可分析的设备数据，请先连接并刷新主控终端。',
  analysisFailed: '健康分析请求失败',
}

type NodeKey = 'k230_vision' | 'csi' | 'ld6002c'

interface ApiNode {
  online: boolean
  fall: boolean
  probability: number
}

interface ApiHistoryDay {
  day: string
  heart_valid: boolean
  heart_bpm: number
  heart_samples: number
  breath_valid: boolean
  breath_bpm: number
  breath_samples: number
}

interface ApiHistory {
  sd_ready: boolean
  status: string
  count: number
  days: ApiHistoryDay[]
}

interface AgentStatus {
  ok: boolean
  network_ready: boolean
  vitals?: {
    radar_online: boolean
    human_present: boolean
    heart_valid: boolean
    heart_bpm: number
    breath_valid: boolean
    breath_bpm: number
  }
  nodes?: Record<NodeKey, ApiNode>
  fusion?: {
    score: number
    threshold: number
    fall_alarm: boolean
  }
  history?: ApiHistory
}

interface NodeView {
  key: NodeKey
  name: string
  shortName: string
  subtitle: string
  online: boolean
  statusText: string
  fallText: string
  probabilityText: string
  barWidth: number
  tone: 'ok' | 'warn' | 'danger' | 'offline'
}

interface HistoryView {
  day: string
  heartText: string
  breathText: string
  heartWidth: number
  breathWidth: number
  heartSamplesText: string
  breathSamplesText: string
}

interface QueryReply {
  ok: boolean
  route: string
  agent: string
  tool: string
  reply: string
}

interface DeepSeekReply {
  choices?: Array<{
    message?: {
      content?: string
    }
  }>
  error?: {
    message?: string
  }
}

let refreshTimer: number | undefined
let latestStatus: AgentStatus | undefined

function normalizeBaseUrl(url: string): string {
  const trimmed = (url || DEFAULT_BASE_URL).trim()
  return trimmed.endsWith('/') ? trimmed.slice(0, -1) : trimmed
}

function formatBpm(valid?: boolean, value?: number): string {
  if (!valid || typeof value !== 'number' || !Number.isFinite(value)) {
    return '--'
  }
  return Math.round(value).toString()
}

function formatTime(): string {
  const now = new Date()
  const h = now.getHours().toString().padStart(2, '0')
  const m = now.getMinutes().toString().padStart(2, '0')
  const s = now.getSeconds().toString().padStart(2, '0')
  return `${h}:${m}:${s}`
}

function formatDate(): string {
  const now = new Date()
  const month = (now.getMonth() + 1).toString().padStart(2, '0')
  const day = now.getDate().toString().padStart(2, '0')
  return `${month}月${day}日 周${'日一二三四五六'[now.getDay()]}`
}

function makeWave(kind: 'heart' | 'breath', valid?: boolean): number[] {
  return Array.from({ length: 28 }, (_, index) => {
    if (!valid) return 3
    if (kind === 'breath') {
      return Math.round(8 + (Math.sin(index / 2.8) + 1) * 9)
    }
    const p = index % 9
    const shape = [5, 6, 8, 4, 16, 34, 6, 12, 7]
    return shape[p]
  })
}

function shortDay(day: string): string {
  if (!day) {
    return '--'
  }
  return day.length >= 10 ? day.slice(5) : day
}

function nodeView(key: NodeKey, name: string, subtitle: string, node?: ApiNode): NodeView {
  const online = !!(node && node.online)
  const fall = !!(node && node.fall)
  const p = node && typeof node.probability === 'number' ? node.probability : 0
  const probability = Math.max(0, Math.min(1, p))
  return {
    key,
    name,
    shortName: key === 'k230_vision' ? '视觉' : key === 'ld6002c' ? '雷达' : 'CSI',
    subtitle,
    online,
    statusText: online ? T.online : T.offline,
    fallText: fall ? T.fall : T.normal,
    probabilityText: `${Math.round(probability * 100)}%`,
    barWidth: Math.round(probability * 100),
    tone: !online ? 'offline' : fall ? 'danger' : probability > 0.45 ? 'warn' : 'ok',
  }
}

function historyView(history?: ApiHistory): HistoryView[] {
  const days = history && Array.isArray(history.days) ? history.days : []
  return days.map((day) => {
    const heart = day.heart_valid ? Math.round(day.heart_bpm) : 0
    const breath = day.breath_valid ? Math.round(day.breath_bpm) : 0
    return {
      day: shortDay(day.day),
      heartText: day.heart_valid ? String(heart) : '--',
      breathText: day.breath_valid ? String(breath) : '--',
      heartWidth: day.heart_valid ? Math.max(8, Math.min(100, Math.round((heart / 140) * 100))) : 0,
      breathWidth: day.breath_valid ? Math.max(8, Math.min(100, Math.round((breath / 30) * 100))) : 0,
      heartSamplesText: `${day.heart_samples || 0}${T.samples}`,
      breathSamplesText: `${day.breath_samples || 0}${T.samples}`,
    }
  })
}

function buildHealthPrompt(status: AgentStatus): string {
  const vitals = status.vitals
  const fusion = status.fusion
  const nodes = status.nodes || ({} as Record<NodeKey, ApiNode>)
  const history = status.history
  const days = history && Array.isArray(history.days) ? history.days : []
  const heart = vitals && vitals.heart_valid ? `${Math.round(vitals.heart_bpm)} 次/分` : '暂无有效数据'
  const breath = vitals && vitals.breath_valid ? `${Math.round(vitals.breath_bpm)} 次/分` : '暂无有效数据'
  const historyText = days.length
    ? days.map((day) => `${day.day || '--'}：心率${day.heart_valid ? Math.round(day.heart_bpm) : '--'}，呼吸${day.breath_valid ? Math.round(day.breath_bpm) : '--'}`).join('；')
    : '暂无五天历史数据'
  const nodeText = [
    `K230视觉${nodes.k230_vision && nodes.k230_vision.online ? '在线' : '离线'}${nodes.k230_vision && nodes.k230_vision.fall ? '，检测到跌倒风险' : ''}`,
    `LD6002C雷达${nodes.ld6002c && nodes.ld6002c.online ? '在线' : '离线'}${nodes.ld6002c && nodes.ld6002c.fall ? '，检测到跌倒风险' : ''}`,
    `CSI无线感知${nodes.csi && nodes.csi.online ? '在线' : '离线'}${nodes.csi && nodes.csi.fall ? '，检测到跌倒风险' : ''}`,
  ].join('；')

  return [
    '请根据以下居家老人健康守护终端数据进行一次简短、谨慎的健康分析：',
    `当前心率：${heart}`,
    `当前呼吸率：${breath}`,
    `人体状态：${vitals && vitals.human_present ? '检测到人体' : '未检测到人体'}`,
    `生命体征雷达：${vitals && vitals.radar_online ? '在线' : '离线'}`,
    `跌倒融合结果：${fusion && fusion.fall_alarm ? '已报警' : '未确认跌倒'}，评分 ${fusion ? fusion.score : 0}/${fusion ? fusion.threshold : 65}`,
    `节点状态：${nodeText}`,
    `最近五天平均：${historyText}`,
    '请用简体中文回答，先给出总体状态，再给出3条可执行建议；若数据异常或不足，请明确提醒复测、联系家属或及时就医。不要做确定性医学诊断，控制在220字以内。',
  ].join('\n')
}

Component({
  data: {
    baseUrl: DEFAULT_BASE_URL,
    queryText: T.queryDefault,
    connected: false,
    loading: false,
    lastUpdate: '--',
    currentTime: formatTime().slice(0, 5),
    currentDate: formatDate(),
    monitorText: '等待连接',
    nodeCountText: '0 / 3',
    vitalQuality: '暂无数据',
    updateText: '--',
    errorText: '',
    agentReply: T.waiting,
    networkText: T.notConnected,
    heart: '--',
    breath: '--',
    radarText: T.radarOffline,
    humanText: T.noHumanShort,
    fusionScore: 0,
    fusionThreshold: 65,
    fusionText: T.waitingData,
    fusionTone: 'offline',
    nodes: [] as NodeView[],
    historyDays: [] as HistoryView[],
    historyStatus: T.noHistory,
    deepseekKey: '',
    analysisLoading: false,
    analysisStatus: '等待分析',
    analysisResult: T.analysisDefault,
    heartWave: makeWave('heart', false),
    breathWave: makeWave('breath', false),
  },

  lifetimes: {
    attached() {
      const saved = wx.getStorageSync(STORAGE_BASE_URL)
      const savedKey = wx.getStorageSync(STORAGE_DEEPSEEK_KEY)
      this.setData({
      baseUrl: typeof saved === 'string' && saved && saved !== LEGACY_BASE_URL ? saved : DEFAULT_BASE_URL,
        deepseekKey: typeof savedKey === 'string' ? savedKey : '',
      })
      this.refreshStatus(true)
      refreshTimer = setInterval(() => this.refreshStatus(true), 5000) as unknown as number
    },
    detached() {
      if (refreshTimer) {
        clearInterval(refreshTimer)
        refreshTimer = undefined
      }
    },
  },

  methods: {
    onBaseUrlInput(e: any) {
      this.setData({ baseUrl: e.detail.value })
    },

    saveBaseUrl() {
      const baseUrl = normalizeBaseUrl(this.data.baseUrl)
      wx.setStorageSync(STORAGE_BASE_URL, baseUrl)
      this.setData({ baseUrl })
      this.refreshStatus(false)
    },

    onQueryInput(e: any) {
      this.setData({ queryText: e.detail.value })
    },

    onDeepSeekKeyInput(e: any) {
      this.setData({ deepseekKey: e.detail.value })
    },

    saveDeepSeekKey() {
      wx.setStorageSync(STORAGE_DEEPSEEK_KEY, (this.data.deepseekKey || '').trim())
    },

    refreshStatus(silentOrEvent: boolean | object = false) {
      const silent = silentOrEvent === true
      if (!silent) {
        wx.showLoading({ title: T.refreshing, mask: false })
      }
      this.setData({ loading: true, errorText: '' })
      wx.request({
        url: `${normalizeBaseUrl(this.data.baseUrl)}/api/agent/status`,
        method: 'GET',
        timeout: 4500,
        success: (res) => {
          if (res.statusCode >= 200 && res.statusCode < 300) {
            this.applyStatus(res.data as AgentStatus)
          } else {
            this.setOffline(`HTTP ${res.statusCode}`)
          }
        },
        fail: (err) => {
          this.setOffline(err.errMsg || T.cannotConnect)
        },
        complete: () => {
          this.setData({ loading: false })
          if (!silent) {
            wx.hideLoading()
          }
        },
      })
    },

    askAgent() {
      const text = (this.data.queryText || 'status').trim()
      wx.showLoading({ title: T.asking, mask: false })
      wx.request({
        url: `${normalizeBaseUrl(this.data.baseUrl)}/api/agent/query`,
        method: 'POST',
        timeout: 6000,
        header: {
          'content-type': 'application/json',
        },
        data: { text },
        success: (res) => {
          if (res.statusCode >= 200 && res.statusCode < 300) {
            const reply = res.data as QueryReply
            this.setData({
              agentReply: reply.reply || T.noReply,
              errorText: '',
            })
          } else {
            this.setData({ errorText: `${T.requestFailed}: HTTP ${res.statusCode}` })
          }
        },
        fail: (err) => {
          this.setData({ errorText: err.errMsg || T.requestFailed })
        },
        complete: () => wx.hideLoading(),
      })
    },

    analyzeHealth() {
      const apiKey = (this.data.deepseekKey || '').trim()
      if (!apiKey) {
        this.setData({ analysisStatus: '缺少密钥', analysisResult: T.analysisNoKey })
        return
      }
      if (!latestStatus) {
        this.setData({ analysisStatus: '暂无数据', analysisResult: T.analysisNoData })
        return
      }

      wx.setStorageSync(STORAGE_DEEPSEEK_KEY, apiKey)
      this.setData({
        analysisLoading: true,
        analysisStatus: '正在分析',
        analysisResult: '正在综合当前生命体征、跌倒判断与历史趋势...',
      })
      wx.showLoading({ title: '分析中', mask: false })
      wx.request({
        url: DEEPSEEK_API_URL,
        method: 'POST',
        timeout: 30000,
        header: {
          'content-type': 'application/json',
          Authorization: `Bearer ${apiKey}`,
        },
        data: {
          model: DEEPSEEK_MODEL,
          messages: [
            { role: 'system', content: '你是居家养老健康监测助手。回答要客观、易懂、可执行，并说明设备数据不能替代医生诊断。' },
            { role: 'user', content: buildHealthPrompt(latestStatus) },
          ],
          temperature: 0.3,
          max_tokens: 500,
          stream: false,
        },
        success: (res) => {
          const data = res.data as DeepSeekReply
          if (res.statusCode >= 200 && res.statusCode < 300) {
            const choice = data.choices && data.choices.length ? data.choices[0] : undefined
            const content = choice && choice.message && choice.message.content
            this.setData({
              analysisStatus: content ? `分析完成 - ${formatTime()}` : '分析失败',
              analysisResult: content ? content.trim() : 'DeepSeek 未返回分析文本',
            })
          } else {
            const message = data.error && data.error.message ? data.error.message : `HTTP ${res.statusCode}`
            this.setData({ analysisStatus: '分析失败', analysisResult: `${T.analysisFailed}：${message}` })
          }
        },
        fail: (err) => {
          this.setData({ analysisStatus: '分析失败', analysisResult: `${T.analysisFailed}：${err.errMsg || '网络错误'}` })
        },
        complete: () => {
          this.setData({ analysisLoading: false })
          wx.hideLoading()
        },
      })
    },

    applyStatus(status: AgentStatus) {
      latestStatus = status
      const vitals = status.vitals
      const nodes = status.nodes || ({} as Record<NodeKey, ApiNode>)
      const fusion = status.fusion
      const fallAlarm = !!(fusion && fusion.fall_alarm)
      const historyDays = historyView(status.history)
      const history = status.history
      const nodeViews = [
        nodeView('k230_vision', T.k230Name, T.k230Sub, nodes.k230_vision),
        nodeView('ld6002c', T.ldName, T.ldSub, nodes.ld6002c),
        nodeView('csi', T.csiName, T.csiSub, nodes.csi),
      ]
      const heartValid = !!(vitals && vitals.heart_valid)
      const breathValid = !!(vitals && vitals.breath_valid)
      const now = formatTime()

      this.setData({
        connected: true,
        networkText: status.network_ready ? T.masterOnline : T.masterOffline,
        lastUpdate: now,
        currentTime: now.slice(0, 5),
        currentDate: formatDate(),
        monitorText: fallAlarm ? '发现跌倒风险' : '持续监护中',
        nodeCountText: `${nodeViews.filter((item) => item.online).length} / 3`,
        vitalQuality: heartValid && breathValid ? '数据完整' : heartValid || breathValid ? '部分有效' : '暂无数据',
        updateText: '刚刚',
        errorText: '',
        heart: formatBpm(vitals && vitals.heart_valid, vitals && vitals.heart_bpm),
        breath: formatBpm(vitals && vitals.breath_valid, vitals && vitals.breath_bpm),
        radarText: vitals && vitals.radar_online ? T.radarOnline : T.vitalRadarOffline,
        humanText: vitals && vitals.human_present ? T.human : T.noHuman,
        fusionScore: fusion && typeof fusion.score === 'number' ? fusion.score : 0,
        fusionThreshold: fusion && typeof fusion.threshold === 'number' ? fusion.threshold : 65,
        fusionText: fallAlarm ? T.fusionAlarm : T.fusionNormal,
        fusionTone: fallAlarm ? 'danger' : 'ok',
        nodes: nodeViews,
        heartWave: makeWave('heart', heartValid),
        breathWave: makeWave('breath', breathValid),
        historyDays,
        historyStatus: historyDays.length ? (history && history.sd_ready ? 'SD OK' : history && history.status || T.noHistory) : T.noHistory,
      })
    },

    setOffline(message: string) {
      this.setData({
        connected: false,
        networkText: T.connectFailed,
        errorText: message,
        lastUpdate: '--',
        fusionText: T.waitingData,
        fusionTone: 'offline',
        monitorText: '主控连接中断',
        nodeCountText: '0 / 3',
        vitalQuality: '数据暂停',
        updateText: '--',
        currentTime: formatTime().slice(0, 5),
        currentDate: formatDate(),
        heartWave: makeWave('heart', false),
        breathWave: makeWave('breath', false),
        nodes: [
          nodeView('k230_vision', T.k230Name, T.k230Sub),
          nodeView('ld6002c', T.ldName, T.ldSub),
          nodeView('csi', T.csiName, T.csiSub),
        ],
      })
    },
  },
})
