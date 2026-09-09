// pages/wifi/wifi.js
const ble = require('../../utils/ble.js')
// 真机时序：不在模块顶层调 getApp()
let app = null
function _app() { if (!app) app = getApp(); return app }

const STATUS_TEXT = {
  unprovisioned: '设备未配网',
  saving: '正在保存...',
  reboot: '设备重启中...',
  connecting: '正在连接 WiFi...',
  connected: '配网成功',
  disconnected: '未连接 WiFi',
  timeout: '连接超时',
  error: '配网失败'
}

Page({
  data: {
    wifiStatus: 'unprovisioned',
    wifiStatusText: '设备未配网',
    wifiStatusClass: 'unprovisioned',
    ssid: '',
    ip: '',
    rssi: 0,
    inputSsid: '',
    inputPwd: '',
    progressText: ''
  },

  onLoad() {
    this.unsub = ble.onState(state => this.handleState(state))
    // 查询当前 WiFi 状态
    if (_app().globalData.connected) {
      ble.send({ cmd: 'wifi_status' }).catch(() => {})
    }
  },

  onUnload() {
    if (this.unsub) this.unsub()
    // 清理所有定时器
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null }
    if (this.reconnectDelay) { clearTimeout(this.reconnectDelay); this.reconnectDelay = null }
  },

  onShow() {
    if (_app().globalData.connected) {
      ble.send({ cmd: 'wifi_status' }).catch(() => {})
    }
  },

  handleState(s) {
    // 处理 WiFi 相关消息
    if (!s.wifi) return

    const status = s.wifi
    const statusText = STATUS_TEXT[status] || status
    const statusClass = status === 'connected' ? 'connected'
      : status === 'connecting' ? 'connecting' : 'unprovisioned'

    const data = {
      wifiStatus: status,
      wifiStatusText: statusText,
      wifiStatusClass: statusClass
    }

    if (status === 'connected') {
      data.ssid = s.ssid || ''
      data.ip = s.ip || ''
      data.rssi = s.rssi || 0
    } else if (status === 'saving') {
      data.progressText = '正在把 WiFi 信息发给设备...'
    } else if (status === 'reboot') {
      data.progressText = '设备即将重启，蓝牙会断开，请稍候...'
      // 蓝牙会断开，开始等待重连
      this.startReconnect()
    } else if (status === 'timeout') {
      data.progressText = '连接超时，请检查 WiFi 信息是否正确'
    } else if (status === 'unprovisioned') {
      // 设备从未配网，正常
    }

    this.setData(data)
  },

  onSsidInput(e) {
    this.setData({ inputSsid: e.detail.value })
  },

  onPwdInput(e) {
    this.setData({ inputPwd: e.detail.value })
  },

  async onSubmit() {
    if (!_app().globalData.connected) {
      wx.showToast({ title: '请先连接设备', icon: 'none' })
      return
    }
    if (!this.data.inputSsid) {
      wx.showToast({ title: '请输入 WiFi 名', icon: 'none' })
      return
    }

    this.setData({ wifiStatus: 'saving', wifiStatusText: '正在保存...', progressText: '正在发送...' })

    try {
      await ble.send({ cmd: 'wifi', ssid: this.data.inputSsid, pwd: this.data.inputPwd })
    } catch (e) {
      wx.showToast({ title: '发送失败', icon: 'none' })
      this.setData({ wifiStatus: 'error', wifiStatusText: '发送失败' })
    }
  },

  // 设备重启后蓝牙断开，尝试重连
  startReconnect() {
    if (this.reconnectTimer) clearInterval(this.reconnectTimer)
    if (this.reconnectDelay) clearTimeout(this.reconnectDelay)
    let attempts = 0
    this.setData({ wifiStatus: 'connecting', wifiStatusText: '设备重启中...', progressText: '等待设备重启...' })

    // M2: 等 3s 让设备重启，然后递归 setTimeout（避免 2s interval 与 5s scan 叠加）
    const retry = async () => {
      attempts++
      if (attempts > 30) {  // 30 次 ≈ 60s
        this.setData({ progressText: '重连超时，请手动重连蓝牙' })
        this.reconnectTimer = null
        return
      }
      if (!_app().globalData.connected) {
        try {
          await ble.init()
          const devices = await ble.scan('Rinse')
          if (devices.length > 0) {
            await ble.connect(devices[0].deviceId)
            this.setData({ progressText: '已重连，等待 WiFi 状态...' })
            ble.send({ cmd: 'wifi_status' }).catch(() => {})
            this.reconnectTimer = null
            return  // M2: 成功后不再递归调用
          }
        } catch (e) {}
      } else {
        // 已经连接，停止重试
        this.reconnectTimer = null
        return
      }
      // M2: 上一次 scan 完成后再调度下一次（避免叠加）
      this.reconnectTimer = setTimeout(retry, 1000)
    }

    this.reconnectDelay = setTimeout(() => {
      this.reconnectDelay = null
      this.reconnectTimer = setTimeout(retry, 1000)
    }, 3000)
  },

  async onReset() {
    const res = await wx.showModal({ title: '重新配网？', content: '将清除当前 WiFi 配置' })
    if (res.confirm) {
      this.setData({
        wifiStatus: 'unprovisioned',
        wifiStatusText: '设备未配网',
        wifiStatusClass: 'unprovisioned',
        inputSsid: '',
        inputPwd: ''
      })
    }
  }
})
