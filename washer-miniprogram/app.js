// app.js
const ble = require('./utils/ble.js')

App({
  globalData: {
    serviceUUID: '0000FFE0-0000-1000-8000-00805F9B34FB',
    charUUID:    '0000FFE1-0000-1000-8000-00805F9B34FB',
    deviceId: '',
    connected: false,
    autoConnecting: false  // 正在后台自动连接
  },

  onLaunch() {
    // 0. 导航栏颜色跟随系统主题
    this.applyNavColor()
    wx.onThemeChange(() => this.applyNavColor())

    // 1. 初始化蓝牙适配器
    wx.openBluetoothAdapter({
      success: () => {
        console.log('[BLE] 适配器就绪')
        // 2. 监听连接状态变化
        wx.onBLEConnectionStateChange(res => {
          if (!res.connected) {
            this.globalData.connected = false
            this.globalData.deviceId = ''
            // 通知首页
            this._notifyDisconnect()
          }
        })
        // 3. 后台自动连接已配对设备
        this.autoConnect()
      },
      fail: () => {
        // 蓝牙没开，等用户去开
        console.log('[BLE] 适配器打开失败')
      }
    })
  },

  // 导航栏颜色跟随系统主题
  applyNavColor() {
    try {
      const info = wx.getSystemInfoSync()
      const isDark = info.theme === 'dark'
      wx.setNavigationBarColor({
        frontColor: isDark ? '#ffffff' : '#000000',
        backgroundColor: isDark ? '#0c0a09' : '#ffffff',
        animation: { duration: 200 }
      })
    } catch (e) {}
  },

  // 后台自动连接：读本地已配对列表，逐个尝试
  autoConnect() {
    if (this.globalData.autoConnecting || this.globalData.connected) return
    let list = []
    try { list = wx.getStorageSync('paired_devices') || [] } catch (e) {}
    if (list.length === 0) return

    this.globalData.autoConnecting = true
    this._autoConnectList(list, 0)
  },

  _autoConnectList(list, idx) {
    if (idx >= list.length) {
      // 全部失败
      this.globalData.autoConnecting = false
      return
    }
    const dev = list[idx]
    wx.createBLEConnection({
      deviceId: dev.deviceId,
      success: () => {
        // 连上了，注册特征值通知
        wx.getBLEDeviceServices({
          deviceId: dev.deviceId,
          success: () => {
            wx.getBLEDeviceCharacteristics({
              deviceId: dev.deviceId,
              serviceId: this.globalData.serviceUUID,
              success: () => {
                wx.notifyBLECharacteristicValueChange({
                  deviceId: dev.deviceId,
                  serviceId: this.globalData.serviceUUID,
                  characteristicId: this.globalData.charUUID,
                  state: true,
                  success: () => {
                    // 通知 ble.js 清空接收缓冲
                    if (ble._resetRx) ble._resetRx()
                    wx.offBLECharacteristicValueChange()
                    wx.onBLECharacteristicValueChange(ble._onCharChange)
                    this.globalData.deviceId = dev.deviceId
                    this.globalData.connected = true
                    this.globalData.autoConnecting = false
                    console.log('[BLE] 自动连接成功:', dev.name)
                    this._notifyConnect()
                  },
                  fail: () => this._autoConnectList(list, idx + 1)
                })
              },
              fail: () => this._autoConnectList(list, idx + 1)
            })
          },
          fail: () => this._autoConnectList(list, idx + 1)
        })
      },
      fail: () => this._autoConnectList(list, idx + 1)
    })
  },

  // 通知首页连接成功
  _notifyConnect() {
    // 用事件通道，ble.js 的 onState 机制
    // 直接发一个 disconnected=false 的状态
    if (this._listeners) {
      this._listeners.forEach(fn => fn({ connected: true }))
    }
  },

  // 通知首页断开
  _notifyDisconnect() {
    if (this._listeners) {
      this._listeners.forEach(fn => fn({ state: 'disconnected' }))
    }
  },

  // 注册/注销首页监听（供 index.js 调用）
  onAppState(fn) {
    if (!this._listeners) this._listeners = new Set()
    this._listeners.add(fn)
    return () => this._listeners.delete(fn)
  }
})
