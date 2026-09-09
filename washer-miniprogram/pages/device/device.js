// pages/device/device.js
const ble = require('../../utils/ble.js')
const ota = require('../../utils/ota.js')
// 真机时序：不在模块顶层调 getApp()
let app = null
function _app() { if (!app) app = getApp(); return app }

const PAIR_KEY = 'paired_devices'  // 本地存储的已配对设备列表

Page({
  data: {
    scanning: false,
    scanned: false,
    connected: false,
    devices: [],          // 本次扫描发现的设备
    paired: [],           // 已配对（历史连接过）的设备
    autoConnecting: false, // 正在自动连接
    otaUploading: false,
    otaProgress: 0,
    otaStage: '',          // idle / checking / downloading / pushing / done / error
    otaMsg: '',
    curVer: ''             // 设备当前固件版本
  },

  onShow() {
    this.setData({
      connected: _app().globalData.connected,
      autoConnecting: _app().globalData.autoConnecting
    })
    this.loadPaired()
    // 已连接时查设备当前固件版本
    if (_app().globalData.connected) {
      ble.send({ cmd: 'version' }).catch(() => {})
    }
  },

  onLoad() {
    this.unsub = ble.onState((s) => {
      // 接收设备返回的版本号
      if (s.ver !== undefined) {
        this.setData({ curVer: s.ver })
      }
    })
  },

  onUnload() {
    if (this.unsub) this.unsub()
  },

  // 读取本地已配对设备
  loadPaired() {
    try {
      const list = wx.getStorageSync(PAIR_KEY) || []
      this.setData({ paired: list })
    } catch (e) {
      this.setData({ paired: [] })
    }
  },

  // 保存到本地
  savePaired(device) {
    if (!device || !device.deviceId) return
    let list = []
    try { list = wx.getStorageSync(PAIR_KEY) || [] } catch (e) {}
    // 去重：同 deviceId 替换，保留最新
    list = list.filter(d => d.deviceId !== device.deviceId)
    list.unshift({ deviceId: device.deviceId, name: device.name || 'Rinse-设备', savedAt: Date.now() })
    // 最多保留 5 个
    if (list.length > 5) list = list.slice(0, 5)
    try { wx.setStorageSync(PAIR_KEY, list) } catch (e) {}
    this.setData({ paired: list })
  },

  // 删除已配对设备
  removePaired(e) {
    const deviceId = e.currentTarget.dataset.id
    let list = []
    try { list = wx.getStorageSync(PAIR_KEY) || [] } catch (e) {}
    list = list.filter(d => d.deviceId !== deviceId)
    try { wx.setStorageSync(PAIR_KEY, list) } catch (e) {}
    this.setData({ paired: list })
    wx.showToast({ title: '已移除', icon: 'none' })
  },

  // 自动连接：已配对列表里第一个设备
  async autoConnect() {
    if (this.data.autoConnecting || this.data.connected) return
    const list = this.data.paired
    if (!list || list.length === 0) return
    this.setData({ autoConnecting: true })
    // 先尝试用系统已记录的连接（iOS/Android 配对缓存）
    for (let i = 0; i < list.length; i++) {
      const deviceId = list[i].deviceId
      try {
        // 初始化蓝牙适配器
        await ble.init()
        // 直接尝试连接（系统若记得该设备会更快）
        await ble.connect(deviceId)
        this.setData({ connected: true, autoConnecting: false })
        wx.showToast({ title: '已自动连接', icon: 'success' })
        return
      } catch (e) {
        // 该设备连不上，尝试下一个
        continue
      }
    }
    // 全部失败：静默退出，等用户手动扫描
    this.setData({ autoConnecting: false })
  },

  async handleScan() {
    this.setData({ scanning: true, scanned: false, devices: [] })
    try {
      await ble.init()
      const devices = await ble.scan('Rinse-设备')
      const map = new Map()
      devices.forEach(d => map.set(d.deviceId, d))
      this.setData({ devices: [...map.values()], scanned: true })
    } catch (e) {
      wx.showToast({ title: '扫描失败', icon: 'none' })
    } finally {
      this.setData({ scanning: false })
    }
  },

  async handleConnect(e) {
    const deviceId = e.currentTarget.dataset.id
    const devName = e.currentTarget.dataset.name
    wx.showLoading({ title: '连接中...' })
    try {
      await ble.connect(deviceId)
      // 连接成功：保存为已配对设备
      this.savePaired({ deviceId: deviceId, name: devName || 'Rinse-设备' })
      this.setData({ connected: true })
      wx.showToast({ title: '连接成功', icon: 'success' })
      setTimeout(() => wx.navigateBack(), 600)
    } catch (err) {
      wx.showToast({ title: '连接失败', icon: 'none' })
    } finally {
      wx.hideLoading()
    }
  },

  async handleDisconnect() {
    await ble.disconnect()
    this.setData({ connected: false })
    wx.showToast({ title: '已断开', icon: 'none' })
  },

  // ===== 云端固件升级 =====
  async handleOta() {
    // 已完成：点"重启设备"
    if (this.data.otaStage === 'done') {
      try {
        await ble.send({ cmd: 'reboot' })
        wx.showToast({ title: '设备重启中', icon: 'none' })
      } catch (e) {
        wx.showToast({ title: '发送失败', icon: 'none' })
      }
      this.setData({ otaStage: '', otaProgress: 0 })
      return
    }

    // 正在升级中：忽略重复点击
    if (this.data.otaUploading) return

    const curVer = this.data.curVer || '0.0.0'
    this.setData({ otaUploading: true, otaProgress: 0, otaStage: 'checking', otaMsg: '检查更新...' })

    // 1. 检查云端版本
    let info
    try {
      info = await ota.checkUpdate(curVer)
    } catch (e) {
      this.setData({ otaUploading: false, otaStage: 'error', otaMsg: e.message })
      wx.showToast({ title: e.message, icon: 'none' })
      return
    }

    if (!info.hasUpdate) {
      this.setData({ otaUploading: false, otaStage: 'idle', otaMsg: '已是最新版本 ' + curVer })
      wx.showToast({ title: '已是最新版本', icon: 'none' })
      return
    }

    // 2. 确认升级
    const modal = await wx.showModal({
      title: '发现新版本 ' + info.version,
      content: (info.notes || '固件更新') + (info.size ? '\n大小: ' + (info.size / 1024).toFixed(1) + ' KB' : ''),
      confirmText: '升级',
      cancelText: '取消'
    })
    if (!modal.confirm) {
      this.setData({ otaUploading: false, otaStage: 'idle' })
      return
    }

    // 3. 下载固件
    this.setData({ otaStage: 'downloading', otaMsg: '下载固件 0%', otaProgress: 0 })
    let filePath
    try {
      filePath = await ota.downloadFw(info.url, (p) => {
        this.setData({ otaProgress: p, otaMsg: '下载固件 ' + p + '%' })
      })
    } catch (e) {
      this.setData({ otaUploading: false, otaStage: 'error', otaMsg: e.message })
      wx.showToast({ title: e.message, icon: 'none' })
      return
    }

    // 4. BLE 推送固件
    this.setData({ otaStage: 'pushing', otaMsg: '推送固件 0%', otaProgress: 0 })
    try {
      await ota.pushToFw(filePath, (cmd) => ble.send(cmd), (p) => {
        this.setData({ otaProgress: p, otaMsg: '推送固件 ' + p + '%' })
      })
    } catch (e) {
      this.setData({ otaUploading: false, otaStage: 'error', otaMsg: e.message })
      wx.showToast({ title: e.message, icon: 'none' })
      return
    }

    // 5. 完成
    this.setData({ otaUploading: false, otaStage: 'done', otaMsg: '升级完成', otaProgress: 100 })
    wx.showModal({
      title: '升级完成',
      content: '固件已写入，点击"重启设备"应用新固件',
      showCancel: false,
      confirmText: '我知道了'
    })
  }
})
