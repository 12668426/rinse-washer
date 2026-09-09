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
    progressText: '',
    scanning: false,
    scanList: [],
    scanMsg: ''
  },

  onLoad() {
    this.unsub = ble.onState(state => this.handleState(state))
    if (_app().globalData.connected) {
      ble.send({ cmd: 'wifi_status' }).catch(() => {})
    }
  },

  onUnload() {
    if (this.unsub) this.unsub()
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null }
    if (this.reconnectDelay) { clearTimeout(this.reconnectDelay); this.reconnectDelay = null }
  },

  onShow() {
    if (_app().globalData.connected) {
      ble.send({ cmd: 'wifi_status' }).catch(() => {})
    }
  },

  handleState(s) {
    if (s.wifi_scan !== undefined) {
      const list = (s.wifi_scan || []).map(item => ({
        ssid: item.ssid,
        rssi: item.rssi,
        enc: item.enc,
        signalLevel: item.rssi > -50 ? 4 : item.rssi > -65 ? 3 : item.rssi > -75 ? 2 : 1
      }))
      this.setData({
        scanning: false,
        scanList: list,
        scanMsg: list.length > 0 ? `发现 ${list.length} 个 WiFi` : '未发现 WiFi'
      })
      return
    }

    if (!s.wifi && s.wifi !== 'disconnected') return
    if (s.wifi === undefined) return

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
      this.startReconnect()
    } else if (status === 'timeout') {
      data.progressText = '连接超时，请检查 WiFi 信息是否正确'
    }

    this.setData(data)
  },

  async onScanWifi() {
    if (!_app().globalData.connected) {
      wx.showToast({ title: '请先连接设备', icon: 'none' })
      return
    }
    this.setData({ scanning: true, scanMsg: '扫描中...', scanList: [] })
    try {
      await ble.send({ cmd: 'wifi_scan' })
    } catch (e) {
      this.setData({ scanning: false, scanMsg: '扫描失败' })
      wx.showToast({ title: '扫描失败', icon: 'none' })
    }
  },

  onSelectWifi(e) {
    const ssid = e.currentTarget.dataset.ssid
    this.setData({ inputSsid: ssid })
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

  startReconnect() {
    if (this.reconnectTimer) clearInterval(this.reconnectTimer)
    if (this.reconnectDelay) clearTimeout(this.reconnectDelay)
    let attempts = 0
    this.setData({ wifiStatus: 'connecting', wifiStatusText: '设备重启中...', progressText: '等待设备重启...' })
    const retry = async () => {
      attempts++
      if (attempts > 30) {
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
            return
          }
        } catch (e) {}
      } else {
        this.reconnectTimer = null
        return
      }
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
