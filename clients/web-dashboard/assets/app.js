const DEFAULT_BASE_URL = 'http://192.168.1.100:8090'
const LEGACY_BASE_URLS = []
const STORAGE_BASE_URL = 'guardian-web-base-url'
const STORAGE_DEEPSEEK_KEY = 'guardian-web-deepseek-key'
const DEEPSEEK_API_URL = 'https://api.deepseek.com/chat/completions'
const DEEPSEEK_MODEL = 'deepseek-chat'

const TEXT = {
  waiting: '等待连接主控终端',
  masterOnline: '主控联网',
  masterOffline: '主控未联网',
  connectedFail: '连接失败',
  cannotConnect: '无法连接主控终端',
  human: '检测到人体',
  noHuman: '未检测到人体',
  radarOnline: '生命体征雷达已连接',
  radarOffline: '生命体征雷达未连接',
  fusionAlarm: '多模态融合确认跌倒风险',
  fusionNormal: '多模态融合未确认跌倒',
  online: '已连接',
  offline: '未连接',
  fall: '疑似跌倒',
  normal: '状态正常',
  historyTitle: '最近五天平均',
  noHistory: '暂无历史数据，SD卡采集后会显示',
  heart: '心率',
  breath: '呼吸',
  analysisWaiting: '等待分析',
  analysisRunning: '正在分析',
  analysisDone: '分析完成',
  analysisDefault: '连接主控终端后，可根据当前数据生成健康建议。',
  analysisNoKey: '请先填写 DeepSeek API Key。',
  analysisNoData: '还没有可分析的设备数据，请先连接并刷新主控终端。',
  analysisFailed: '健康分析请求失败',
}

const $ = (id) => document.getElementById(id)

const els = {
  body: document.body,
  connectionDot: $('connectionDot'),
  connectionText: $('connectionText'),
  refreshBtn: $('refreshBtn'),
  connectBtn: $('connectBtn'),
  baseUrlInput: $('baseUrlInput'),
  errorBox: $('errorBox'),
  heartValue: $('heartValue'),
  breathValue: $('breathValue'),
  humanText: $('humanText'),
  radarText: $('radarText'),
  fusionCard: $('fusionCard'),
  fusionText: $('fusionText'),
  fusionScore: $('fusionScore'),
  fusionThreshold: $('fusionThreshold'),
  nodeList: $('nodeList'),
  historyList: $('historyList'),
  historyStatus: $('historyStatus'),
  deepseekKeyInput: $('deepseekKeyInput'),
  analyzeBtn: $('analyzeBtn'),
  analysisStatus: $('analysisStatus'),
  analysisReply: $('analysisReply'),
  desktopModeBtn: $('desktopModeBtn'),
  phoneModeBtn: $('phoneModeBtn'),
  currentTime: $('currentTime'),
  currentDate: $('currentDate'),
  safetyText: $('safetyText'),
  onlineNodeCount: $('onlineNodeCount'),
  vitalQuality: $('vitalQuality'),
  updateAge: $('updateAge'),
  heartWave: $('heartWave'),
  breathWave: $('breathWave'),
  fusionSources: $('fusionSources'),
  analysisHeart: $('analysisHeart'),
  analysisBreath: $('analysisBreath'),
  analysisRisk: $('analysisRisk'),
}

let latestStatus = null
let lastStatusAt = 0
let wavePhase = 0

const nodeMeta = {
  k230_vision: ['K230 视觉', '倒装摄像头跌倒识别'],
  ld6002c: ['LD6002C 雷达', '独立雷达跌倒节点'],
  csi: ['CSI 无线感知', 'ESP32-S3 双机辅助感知'],
}

function nodesForDisplay(nodes = {}) {
  return Object.fromEntries(Object.keys(nodeMeta).map((key) => [
    key,
    Object.assign({}, nodes[key] || {}),
  ]))
}

function normalizeBaseUrl(url) {
  const value = (url || DEFAULT_BASE_URL).trim()
  return value.endsWith('/') ? value.slice(0, -1) : value
}

function formatBpm(valid, value) {
  return valid && Number.isFinite(value) ? String(Math.round(value)) : '--'
}

function timeText() {
  return new Date().toLocaleTimeString('zh-CN', { hour12: false })
}

function updateClock() {
  const now = new Date()
  els.currentTime.textContent = now.toLocaleTimeString('zh-CN', {
    hour: '2-digit', minute: '2-digit', hour12: false,
  })
  els.currentDate.textContent = now.toLocaleDateString('zh-CN', {
    year: 'numeric', month: 'long', day: 'numeric', weekday: 'short',
  })
}

function updateAge() {
  if (!lastStatusAt) {
    els.updateAge.textContent = '--'
    return
  }
  const seconds = Math.max(0, Math.floor((Date.now() - lastStatusAt) / 1000))
  els.updateAge.textContent = seconds < 2 ? '刚刚' : `${seconds} 秒前`
}

function wavePoints(kind, valid) {
  const points = []
  for (let x = 0; x <= 320; x += 8) {
    let y = 34
    if (valid && kind === 'breath') {
      y = 30 + Math.sin((x + wavePhase) / 35) * 11 + Math.sin((x + wavePhase) / 13) * 1.5
    } else if (valid) {
      const p = ((x + wavePhase) % 78) / 78
      y += Math.sin((x + wavePhase) / 16) * 1.2
      if (p > 0.18 && p <= 0.24) y -= 5
      else if (p > 0.24 && p <= 0.30) y += 8
      else if (p > 0.30 && p <= 0.36) y -= 24
      else if (p > 0.36 && p <= 0.43) y += 11
      else if (p > 0.43 && p <= 0.51) y -= 3
    }
    points.push(`${x},${Math.round(y * 10) / 10}`)
  }
  return points.join(' ')
}

function renderWaves(vitals = {}) {
  wavePhase = (wavePhase + 11) % 78
  const heartValid = !!vitals.heart_valid
  const breathValid = !!vitals.breath_valid
  els.heartWave.setAttribute('points', wavePoints('heart', heartValid))
  els.breathWave.setAttribute('points', wavePoints('breath', breathValid))
  els.heartWave.classList.toggle('inactive', !heartValid)
  els.breathWave.classList.toggle('inactive', !breathValid)
}

function setLoading(loading) {
  els.refreshBtn.classList.toggle('loading', loading)
}

function setError(message = '') {
  els.errorBox.textContent = message
  els.errorBox.classList.toggle('hidden', !message)
}

function setConnected(connected, message) {
  els.connectionDot.classList.toggle('online', connected)
  els.connectionDot.classList.toggle('offline', !connected)
  els.connectionText.textContent = message
}

function renderStatus(data) {
  lastStatusAt = Date.now()
  const vitals = data.vitals || {}
  const nodes = data.nodes || {}
  const displayNodes = nodesForDisplay(nodes)
  const fusion = data.fusion || { score: 0, threshold: 65, fall_alarm: false }
  const history = data.history || {}
  const alarm = !!fusion.fall_alarm
  latestStatus = Object.assign({}, data, {
    controller_fusion: data.fusion || {},
    fusion,
  })

  setConnected(true, `${data.network_ready ? TEXT.masterOnline : TEXT.masterOffline} - ${timeText()}`)
  setError('')

  els.heartValue.textContent = formatBpm(vitals.heart_valid, Number(vitals.heart_bpm))
  els.breathValue.textContent = formatBpm(vitals.breath_valid, Number(vitals.breath_bpm))
  els.humanText.textContent = vitals.human_present ? TEXT.human : TEXT.noHuman
  els.radarText.textContent = vitals.radar_online ? TEXT.radarOnline : TEXT.radarOffline
  els.analysisHeart.textContent = formatBpm(vitals.heart_valid, Number(vitals.heart_bpm))
  els.analysisBreath.textContent = formatBpm(vitals.breath_valid, Number(vitals.breath_bpm))
  els.analysisRisk.textContent = String(fusion.score ?? 0)

  const onlineNodeCount = Object.values(displayNodes).filter((node) => node.online).length
  els.onlineNodeCount.textContent = `${onlineNodeCount} / 3`
  els.vitalQuality.textContent = vitals.heart_valid && vitals.breath_valid
    ? '数据完整'
    : (vitals.heart_valid || vitals.breath_valid ? '部分有效' : '暂无数据')
  els.safetyText.textContent = alarm ? '发现跌倒风险' : '持续监护中'
  els.safetyText.classList.toggle('danger-text', alarm)
  renderWaves(vitals)

  els.fusionCard.classList.toggle('danger', alarm)
  els.fusionText.textContent = alarm ? TEXT.fusionAlarm : TEXT.fusionNormal
  els.fusionScore.textContent = String(fusion.score ?? 0)
  els.fusionThreshold.textContent = String(fusion.threshold ?? 65)
  renderFusionSources(displayNodes)
  renderHistory(history)

  renderNodes(displayNodes)
}

function renderFusionSources(nodes = {}) {
  const shortNames = {
    k230_vision: '视觉',
    ld6002c: '雷达',
    csi: 'CSI',
  }
  els.fusionSources.innerHTML = Object.keys(nodeMeta).map((key) => {
    const node = nodes[key] || {}
    const online = !!node.online
    const fall = !!node.fall
    const tone = !online ? 'offline' : fall ? 'danger' : 'ok'
    return `<span class="source-chip ${tone}"><i></i><b>${shortNames[key]}</b><small>${!online ? '离线' : (fall ? '风险' : '正常')}</small></span>`
  }).join('')
}

function renderNodes(nodes = {}) {
  els.nodeList.innerHTML = Object.entries(nodeMeta).map(([key, [title, subtitle]]) => {
    const node = nodes[key] || {}
    const online = !!node.online
    const fall = !!node.fall
    const p = Math.max(0, Math.min(1, Number(node.probability || 0)))
    const tone = !online ? 'offline' : fall ? 'danger' : p > 0.45 ? 'warn' : 'ok'

    return `
      <article class="node-card ${tone}">
        <div class="node-top">
          <div>
            <span class="node-title">${title}</span>
            <span class="node-subtitle">${subtitle}</span>
          </div>
          <span class="node-badge">${online ? TEXT.online : TEXT.offline}</span>
        </div>
        <div class="node-bottom">
          <span>${fall ? TEXT.fall : TEXT.normal}</span>
          <span>${Math.round(p * 100)}%</span>
        </div>
        <div class="bar">
          <span class="bar-fill" style="width:${Math.round(p * 100)}%"></span>
        </div>
      </article>
    `
  }).join('')
}

function buildHealthPrompt(data) {
  const vitals = data.vitals || {}
  const fusion = data.fusion || {}
  const nodes = nodesForDisplay(data.nodes || {})
  const history = data.history || {}
  const days = Array.isArray(history.days) ? history.days : []
  const heart = vitals.heart_valid ? `${Math.round(Number(vitals.heart_bpm))} 次/分` : '暂无有效数据'
  const breath = vitals.breath_valid ? `${Math.round(Number(vitals.breath_bpm))} 次/分` : '暂无有效数据'
  const historyText = days.length
    ? days.map((day) => `${day.day || '--'}：心率${day.heart_valid ? Math.round(Number(day.heart_bpm)) : '--'}，呼吸${day.breath_valid ? Math.round(Number(day.breath_bpm)) : '--'}`).join('；')
    : '暂无五天历史数据'
  const nodeText = Object.entries(nodeMeta).map(([key, [title]]) => {
    const node = nodes[key] || {}
    return `${title}${node.online ? '在线' : '离线'}${node.fall ? '，检测到跌倒风险' : ''}`
  }).join('；')

  return [
    '请根据以下居家老人健康守护终端数据进行一次简短、谨慎的健康分析：',
    `当前心率：${heart}`,
    `当前呼吸率：${breath}`,
    `人体状态：${vitals.human_present ? '检测到人体' : '未检测到人体'}`,
    `生命体征雷达：${vitals.radar_online ? '在线' : '离线'}`,
    `多模态融合结果：${fusion.fall_alarm ? '已报警' : '未确认跌倒'}，评分 ${fusion.score || 0}/${fusion.threshold || 65}`,
    `节点状态：${nodeText}`,
    `最近五天平均：${historyText}`,
    '请用简体中文回答，先给出总体状态，再给出3条可执行建议；若数据异常或不足，请明确提醒复测、联系家属或及时就医。不要做确定性医学诊断，控制在220字以内。',
  ].join('\n')
}

async function analyzeHealth() {
  const apiKey = els.deepseekKeyInput.value.trim()
  if (!apiKey) {
    els.analysisStatus.textContent = '缺少密钥'
    els.analysisReply.textContent = TEXT.analysisNoKey
    els.deepseekKeyInput.focus()
    return
  }

  localStorage.setItem(STORAGE_DEEPSEEK_KEY, apiKey)
  if (!latestStatus) {
    await refreshStatus()
  }
  if (!latestStatus) {
    els.analysisStatus.textContent = '暂无数据'
    els.analysisReply.textContent = TEXT.analysisNoData
    return
  }

  els.analyzeBtn.disabled = true
  els.analyzeBtn.textContent = '分析中...'
  els.analysisStatus.textContent = TEXT.analysisRunning
  els.analysisReply.textContent = '正在综合当前生命体征、跌倒判断与历史趋势...'
  setError('')

  try {
    const response = await fetch(DEEPSEEK_API_URL, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${apiKey}`,
      },
      body: JSON.stringify({
        model: DEEPSEEK_MODEL,
        messages: [
          { role: 'system', content: '你是居家养老健康监测助手。回答要客观、易懂、可执行，并说明设备数据不能替代医生诊断。' },
          { role: 'user', content: buildHealthPrompt(latestStatus) },
        ],
        temperature: 0.3,
        max_tokens: 500,
        stream: false,
      }),
    })
    const data = await response.json().catch(() => ({}))
    if (!response.ok) {
      const apiMessage = data.error && data.error.message ? data.error.message : `HTTP ${response.status}`
      throw new Error(apiMessage)
    }
    const choice = data.choices && data.choices[0]
    const content = choice && choice.message && choice.message.content
    if (!content) throw new Error('DeepSeek 未返回分析文本')
    els.analysisStatus.textContent = `${TEXT.analysisDone} - ${timeText()}`
    els.analysisReply.textContent = content.trim()
  } catch (error) {
    els.analysisStatus.textContent = '分析失败'
    els.analysisReply.textContent = `${TEXT.analysisFailed}：${error.message || '未知错误'}`
  } finally {
    els.analyzeBtn.disabled = false
    els.analyzeBtn.textContent = '一键分析'
  }
}

function shortDay(day) {
  if (!day) return '--'
  return day.length >= 10 ? day.slice(5) : day
}

function renderHistory(history) {
  const days = Array.isArray(history.days) ? history.days : []
  els.historyStatus.textContent = days.length
    ? `${TEXT.historyTitle} - ${history.sd_ready ? 'SD OK' : (history.status || '--')}`
    : TEXT.noHistory

  if (!days.length) {
    els.historyList.innerHTML = `<div class="history-empty">${TEXT.noHistory}</div>`
    return
  }

  els.historyList.innerHTML = days.map((day) => {
    const heart = day.heart_valid ? Math.round(Number(day.heart_bpm || 0)) : 0
    const breath = day.breath_valid ? Math.round(Number(day.breath_bpm || 0)) : 0
    const heartWidth = day.heart_valid ? Math.max(8, Math.min(100, Math.round((heart / 140) * 100))) : 0
    const breathWidth = day.breath_valid ? Math.max(8, Math.min(100, Math.round((breath / 30) * 100))) : 0

    return `
      <article class="history-row">
        <span class="history-day">${shortDay(day.day)}</span>
        <div class="history-bars">
          <div class="history-line">
            <span class="history-label">${TEXT.heart}</span>
            <div class="history-track"><span class="history-fill heart-fill" style="width:${heartWidth}%"></span></div>
            <span class="history-value">${day.heart_valid ? heart : '--'}</span>
          </div>
          <div class="history-line">
            <span class="history-label">${TEXT.breath}</span>
            <div class="history-track"><span class="history-fill breath-fill" style="width:${breathWidth}%"></span></div>
            <span class="history-value">${day.breath_valid ? breath : '--'}</span>
          </div>
        </div>
      </article>
    `
  }).join('')
}

async function refreshStatus() {
  const baseUrl = normalizeBaseUrl(els.baseUrlInput.value)
  localStorage.setItem(STORAGE_BASE_URL, baseUrl)
  els.baseUrlInput.value = baseUrl
  setLoading(true)
  try {
    const response = await fetch(`${baseUrl}/api/device/status`, { cache: 'no-store' })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const data = await response.json()
    renderStatus(data)
    return data
  } catch (error) {
    setConnected(false, `${TEXT.connectedFail} - --`)
    setError(error.message === 'Failed to fetch' ? TEXT.cannotConnect : (error.message || TEXT.cannotConnect))
    els.safetyText.textContent = '主控连接中断'
    els.safetyText.classList.add('danger-text')
    els.onlineNodeCount.textContent = '3 / 3'
    els.vitalQuality.textContent = '数据暂停'
    renderNodes(nodesForDisplay())
    renderFusionSources(nodesForDisplay())
  } finally {
    setLoading(false)
  }
  return null
}

function setMode(mode) {
  els.body.classList.toggle('desktop-mode', mode === 'desktop')
  els.body.classList.toggle('phone-mode', mode === 'phone')
  els.desktopModeBtn.classList.toggle('active', mode === 'desktop')
  els.phoneModeBtn.classList.toggle('active', mode === 'phone')
  localStorage.setItem('guardian-web-mode', mode)
}

els.refreshBtn.addEventListener('click', refreshStatus)
els.connectBtn.addEventListener('click', refreshStatus)
els.analyzeBtn.addEventListener('click', analyzeHealth)
els.deepseekKeyInput.addEventListener('change', () => {
  localStorage.setItem(STORAGE_DEEPSEEK_KEY, els.deepseekKeyInput.value.trim())
})
els.desktopModeBtn.addEventListener('click', () => setMode('desktop'))
els.phoneModeBtn.addEventListener('click', () => setMode('phone'))

const savedBaseUrl = localStorage.getItem(STORAGE_BASE_URL)
  els.baseUrlInput.value = !savedBaseUrl || LEGACY_BASE_URLS.indexOf(savedBaseUrl) >= 0 ? DEFAULT_BASE_URL : savedBaseUrl
els.deepseekKeyInput.value = localStorage.getItem(STORAGE_DEEPSEEK_KEY) || ''
renderNodes()
renderFusionSources()
const requestedMode = new URLSearchParams(window.location.search).get('mode')
setMode(requestedMode === 'phone' ? 'phone' : (localStorage.getItem('guardian-web-mode') || 'desktop'))
updateClock()
updateAge()
refreshStatus()
setInterval(refreshStatus, 5000)
setInterval(() => {
  updateClock()
  updateAge()
  if (latestStatus) renderWaves(latestStatus.vitals || {})
}, 1000)
