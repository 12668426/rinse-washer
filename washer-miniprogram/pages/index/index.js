// pages/index/index.js
const ble = require('../../utils/ble.js')
// 不在顶层调 getApp()，避免真机时序问题；改在生命周期里懒获取
let app = null
function _app() { if (!app) app = getApp(); return app }

const STAGE_TEXT = {
  idle: '待机', fill: '进水中', wash: '洗涤中', rinse: '漂洗中',
  drain: '排水中', done: '已完成', alarm: '报警',
  disconnected: '未连接'
}

const STATUS_TEXT = {
  idle: '待机', fill: '进水', wash: '洗涤', rinse: '漂洗',
  drain: '排水', done: '完成', alarm: '报警', disconnected: '未连接'
}

const MODE_LABEL = {
  standard: '标准', quick: '快洗', gentle: '轻柔', rinse: '漂洗'
}

const MODE_DURATIONS = {  // 用于进度条百分比估算（秒）
  standard: 30*60, quick: 15*60, gentle: 25*60, rinse: 10*60
}

// 运行态（不是 idle/done/alarm/disconnected）
const RUNNING_STATES = ['fill', 'wash', 'rinse', 'drain']

// 根据状态计算主按钮的形态
function computePrimary(state, paused, total) {
  if (state === 'done')   return { action: 'done',   label: '完成' }
  if (state === 'alarm')  return { action: 'start',   label: '启动' }
  // 手动排水（total=0）：显示"结束排水"
  if (state === 'drain' && (!total || total === 0)) {
    return { action: 'stopDrain', label: '结束排水' }
  }
  if (RUNNING_STATES.includes(state)) {
    return paused
      ? { action: 'resume', label: '继续' }
      : { action: 'pause',  label: '暂停' }
  }
  // idle / disconnected / unknown
  return { action: 'start', label: '启动' }
}

Page({
  data: {
    connected: false,
    state: 'idle',
    stateClass: 'idle',
    statusText: '待机',
    stageText: '待机',
    remain: 0,
    remainText: '0:00',
    temp: '--',
    progressPct: 0,
    progressTransition: 0,  // 进度条匀速过渡时长（秒）= 当前阶段总时长
    alarmMsg: '',
    selectedMode: 'standard',
    modeLabel: '标准',
    paused: false,
    primaryAction: 'start',
    primaryLabel: '启动',
    modes: [
      { key: 'standard', name: '标准',   desc: '30 分钟' },
      { key: 'quick',    name: '快洗',   desc: '15 分钟' },
      { key: 'gentle',   name: '轻柔',   desc: '25 分钟' },
      { key: 'rinse',    name: '仅漂洗', desc: '10 分钟' }
    ]
  },

  // 本地倒数计时状态（非 data，不触发渲染）
  _bleRemain: 0,        // 固件最近一次广播的 remain（秒）
  _bleRemainTs: 0,      // 收到该广播的时间戳（ms）
  _tickTimer: null,     // 每秒 tick 定时器
  _stageTotal: 0,       // 当前环节总时长（秒，用于进度条）

  onLoad() {
    this.unsub = ble.onState(state => this.handleState(state))
    // 监听 app 后台自动连接成功/断开的通知
    this._appUnsub = _app().onAppState(state => {
      if (state.connected) {
        // 后台自动连上了，刷新 UI
        this.setData({ connected: true })
        ble.send({ cmd: 'status' }).catch(() => {})
      } else if (state.state === 'disconnected') {
        this.handleState({ state: 'disconnected' })
      }
    })
    // 每秒本地倒数：基于固件广播的 remain + 时间差，平滑显示
    this._tickTimer = setInterval(() => this.tickRemain(), 1000)
  },

  onShow() {
    if (_app().globalData.connected) {
      // 已连接：请求一次最新状态
      this.setData({ connected: true })
      ble.send({ cmd: 'status' }).catch(() => {})
    } else if (_app().globalData.autoConnecting) {
      // 正在后台自动连接，显示"连接中"
      this.setData({
        connected: false,
        state: 'disconnected',
        stateClass: 'disconnected',
        statusText: '连接中',
        stageText: '正在连接设备...',
        remain: 0,
        remainText: '--:--',
        progressPct: 0,
        primaryAction: 'start',
        primaryLabel: '启动'
      })
    } else {
      // 未连接：界面显示"未连接"，而不是默认的"待机"
      this.setData({
        connected: false,
        state: 'disconnected',
        stateClass: 'disconnected',
        statusText: '未连接',
        stageText: '请先连接设备',
        remain: 0,
        remainText: '--:--',
        progressPct: 0,
        primaryAction: 'start',
        primaryLabel: '启动'
      })
    }
  },

  onUnload() {
    if (this.unsub) this.unsub()
    if (this._appUnsub) this._appUnsub()
    if (this._tickTimer) { clearInterval(this._tickTimer); this._tickTimer = null }
  },

  // 格式化秒为 m:ss
  _fmtRemain(sec) {
    const s = Math.max(0, Math.floor(sec))
    const mm = Math.floor(s / 60)
    const ss = s % 60
    return `${mm}:${ss < 10 ? '0' + ss : ss}`
  },

  // 每秒本地倒数：只更新倒计时数字，进度条靠 CSS transition 匀速动
  tickRemain() {
    const { state, paused, connected } = this.data
    // 只在运行态且未暂停时本地倒数
    if (!connected || !RUNNING_STATES.includes(state) || paused) return
    // 手动排水（total=0）：不倒数，保持 --:--
    if (state === 'drain' && this._stageTotal === 0) return
    if (!this._bleRemain || !this._bleRemainTs) return

    // 本地估算剩余 = 广播remain - (距上次广播的秒数)
    const elapsedSec = Math.floor((Date.now() - this._bleRemainTs) / 1000)
    let localRemain = this._bleRemain - elapsedSec
    if (localRemain < 0) localRemain = 0

    // 只更新倒计时数字，不碰 progressPct（进度条由 CSS 匀速过渡）
    this.setData({
      remain: localRemain,
      remainText: this._fmtRemain(localRemain)
    })
  },

  handleState(s) {
    // WiFi 事件单独处理，不污染运行状态
    if (s.wifi !== undefined) {
      this.setData({ wifiStatus: s.wifi })
      return
    }

    if (s.state === 'disconnected') {
      this._bleRemain = 0
      this._bleRemainTs = 0
      this._stageTotal = 0
      this.setData({
        connected: false,
        state: 'disconnected',
        stateClass: 'disconnected',
        statusText: '未连接',
        stageText: '请先连接设备',
        remain: 0,
        remainText: '--:--',
        progressPct: 0,
        temp: '--',
        alarmMsg: '',
        paused: false,
        primaryAction: 'start',
        primaryLabel: '启动'
      })
      return
    }

    if (s.state === undefined) return

    const state = s.state || 'idle'
    const paused = !!s.paused
    const remain = s.remain || 0
    const safeRemain = (remain > 21600 || remain < 0) ? 0 : remain
    // 手动排水（drain + total=0）保留 0，不回退到默认值
    let stageTotal
    if (s.total !== undefined && s.total !== null) {
      stageTotal = s.total  // 固件显式发了 total，信任它（包括 0 = 手动排水）
    } else {
      stageTotal = MODE_DURATIONS[this.data.selectedMode] || 1800
    }

    // 总是静默更新校准基准（本地 tick 会基于此 + 时间差计算）
    this._bleRemain = safeRemain
    this._bleRemainTs = Date.now()
    this._stageTotal = stageTotal

    // 状态变化（state 变了 / paused 变了 / done / alarm）才 setData 渲染
    // 相同运行态下的周期广播：只更新基准，不 setData，避免和本地 tick 打架导致跳
    const stateChanged = (state !== this.data.state)
    const pausedChanged = (paused !== this.data.paused)
    const isTerminal = (state === 'done' || state === 'alarm' || state === 'idle')

    if (stateChanged || pausedChanged || isTerminal) {
      // 当前实际进度（用于暂停冻结）
      const currentPct = stageTotal > 0 ? Math.max(0, Math.min(100, (1 - safeRemain / stageTotal) * 100)) : 0
      const primary = computePrimary(state, paused, stageTotal)
      // 暂停态只显示"已暂停"，不带阶段名
      let stageText = ''
      if (paused) {
        stageText = '已暂停'
      } else {
        stageText = STAGE_TEXT[state] || ''
      }

      // 基础字段（不含进度条）
      const baseData = {
        connected: true,
        state,
        stateClass: state,
        stageText,
        statusText: STATUS_TEXT[state] || state,
        remain: safeRemain,
        remainText: this._fmtRemain(safeRemain),
        temp: (s.temp === undefined || s.temp < 0) ? '--' : s.temp,
        alarmMsg: s.msg || '',
        paused,
        primaryAction: primary.action,
        primaryLabel: primary.label
      }

      if (state === 'done' || state === 'alarm') {
        // 完成/报警：进度条定格
        baseData.progressPct = state === 'done' ? 100 : currentPct
        baseData.progressTransition = 0
        this.setData(baseData)
      } else if (state === 'drain' && stageTotal === 0) {
        // 手动排水：不计时，进度条空着不动，倒计时显示 --
        baseData.progressPct = 0
        baseData.progressTransition = 0
        baseData.remainText = '--:--'
        this.setData(baseData)
      } else if (RUNNING_STATES.includes(state)) {
        if (paused) {
          // 暂停：冻结进度条在当前位置
          baseData.progressPct = currentPct
          baseData.progressTransition = 0
          this.setData(baseData)
        } else if (stateChanged) {
          // 新阶段开始：先瞬间归零，下一帧匀速涨到 100%
          baseData.progressPct = 0
          baseData.progressTransition = 0
          this.setData(baseData, () => {
            // 渲染完成后，设目标 100%，匀速过渡 = 剩余秒数
            this.setData({ progressPct: 100, progressTransition: safeRemain })
          })
        } else {
          // 暂停→恢复：从当前进度匀速涨到 100%
          baseData.progressPct = 100
          baseData.progressTransition = safeRemain
          this.setData(baseData)
        }
      } else {
        // idle / disconnected
        baseData.progressPct = 0
        baseData.progressTransition = 0
        this.setData(baseData)
      }
    }
    // 相同运行态 + 相同暂停态：周期广播只更新 _bleRemain/_bleRemainTs，
    // 本地 tickRemain 会每秒基于新基准平滑减1，不会有跳变
  },

  selectMode(e) {
    // M3: 运行中（pause 态）和暂停态（resume 态）都禁止切换模式
    if (this.data.primaryAction === 'pause' || this.data.primaryAction === 'resume') {
      wx.showToast({ title: '运行中不可切换', icon: 'none', duration: 1200 })
      return
    }
    const key = e.currentTarget.dataset.key
    this.setData({
      selectedMode: key,
      modeLabel: MODE_LABEL[key] || key
    })
  },

  async ensureConnected() {
    if (!_app().globalData.connected) {
      wx.showToast({ title: '请先连接设备', icon: 'none' })
      return false
    }
    return true
  },

  // 主按钮：根据当前形态分发到 start/pause/resume/done
  async onPrimary() {
    if (!await this.ensureConnected()) return
    const action = this.data.primaryAction
    if (action === 'start' || action === 'resume') {
      try {
        await ble.send({ cmd: action, mode: this.data.selectedMode })
      } catch {
        wx.showToast({ title: '发送失败', icon: 'none' })
      }
    } else if (action === 'pause') {
      await ble.send({ cmd: 'pause' }).catch(() => {})
    } else if (action === 'stopDrain') {
      // 手动排水结束：重置回 idle
      await ble.send({ cmd: 'stop' }).catch(() => {})
      wx.showToast({ title: '排水已结束', icon: 'none' })
    } else if (action === 'done') {
      // 完成态：点一下重置回 idle（待机），可重新启动
      await ble.send({ cmd: 'stop' }).catch(() => {})
    }
  },

  async onStop() {
    if (!await this.ensureConnected()) return
    const res = await wx.showModal({ title: '确认结束？', content: '将结束本次运行并排水' })
    if (res.confirm) {
      await ble.send({ cmd: 'stop' }).catch(() => {})
      wx.showToast({ title: '已结束', icon: 'none' })
    }
  },

  goDevice() {
    wx.navigateTo({ url: '/pages/device/device' })
  },

  goWifi() {
    if (!_app().globalData.connected) {
      wx.showToast({ title: '请先连接设备', icon: 'none' })
      return
    }
    wx.navigateTo({ url: '/pages/wifi/wifi' })
  }
})
