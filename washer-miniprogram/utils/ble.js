// utils/ble.js —— 蓝牙 BLE 封装
// 注意：不能在模块顶层调用 getApp()，因为本模块被 require 时 App() 可能尚未注册
// 真机时序比开发者工具更严，会导致 "Page has not been registered" 错误
function _app() { return getApp() }
const SERVICE_UUID = '0000FFE0-0000-1000-8000-00805F9B34FB'
const CHAR_UUID    = '0000FFE1-0000-1000-8000-00805F9B34FB'

const listeners = new Set()
function onState(fn) { listeners.add(fn); return () => listeners.delete(fn) }
function emit(state) { listeners.forEach(fn => fn(state)) }

// M1: 用 flag 避免重复注册监听器，防止累积泄漏
let initDone = false
let scanning = false

function init() {
  return new Promise((resolve, reject) => {
    wx.openBluetoothAdapter({
      success: () => {
        // M1: 只注册一次连接状态监听
        if (!initDone) {
          wx.onBLEConnectionStateChange(res => {
            if (!res.connected) {
              _app().globalData.connected = false
              _app().globalData.deviceId = ''
              emit({ state: 'disconnected' })
            }
          })
          initDone = true
        }
        resolve()
      },
      fail: reject
    })
  })
}

function scan(prefix = 'Rinse') {
  return new Promise((resolve, reject) => {
    // M1: 并发保护
    if (scanning) return reject(new Error('scanning'))
    scanning = true
    let found = []
    const onFound = res => {
      res.devices.forEach(d => {
        if (d.name && d.name.indexOf(prefix) >= 0) found.push(d)
      })
    }
    wx.startBluetoothDevicesDiscovery({
      services: [SERVICE_UUID],
      allowDuplicatesKey: false,
      success: () => {
        // M1: 先 off 再 on，避免重复累积
        wx.offBluetoothDeviceFound()
        wx.onBluetoothDeviceFound(onFound)
        setTimeout(() => {
          wx.stopBluetoothDevicesDiscovery()
          wx.offBluetoothDeviceFound(onFound)
          scanning = false
          resolve(found)
        }, 5000)
      },
      fail: (e) => {
        scanning = false
        reject(e)
      }
    })
  })
}

// M1: 字节级累积，解决跨包 UTF-8 中文字符损坏
let rxBytes = []
function onCharChange(item) {
  const u8 = new Uint8Array(item.value)
  for (let i = 0; i < u8.length; i++) rxBytes.push(u8[i])
  let idx
  while ((idx = rxBytes.indexOf(0x0A)) >= 0) {  // '\n'
    const lineBytes = rxBytes.splice(0, idx + 1)
    const line = utf8Decode(lineBytes)
    try { emit(JSON.parse(line)) } catch (e) {}
  }
}
// 暴露给 app.js 自动连接成功后复用
function _onCharChange(item) { onCharChange(item) }
function _resetRx() { rxBytes = [] }
function utf8Decode(bytes) {
  let s = ''
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i])
  try { return decodeURIComponent(escape(s)) } catch (e) { return s }
}

function connect(deviceId) {
  return new Promise((resolve, reject) => {
    wx.createBLEConnection({
      deviceId,
      success: () => {
        wx.getBLEDeviceServices({
          deviceId,
          success: () => {
            wx.getBLEDeviceCharacteristics({
              deviceId,
              serviceId: SERVICE_UUID,
              success: () => {
                wx.notifyBLECharacteristicValueChange({
                  deviceId,
                  serviceId: SERVICE_UUID,
                  characteristicId: CHAR_UUID,
                  state: true,
                  success: () => {
                    // M1: 先 off 再 on，避免重连后监听器累积
                    wx.offBLECharacteristicValueChange()
                    rxBytes = []
                    wx.onBLECharacteristicValueChange(onCharChange)
                    _app().globalData.deviceId = deviceId
                    _app().globalData.connected = true
                    resolve()
                  },
                  fail: reject
                })
              },
              fail: reject
            })
          },
          fail: reject
        })
      },
      fail: reject
    })
  })
}

function send(obj) {
  if (!_app().globalData.connected) return Promise.reject('未连接')
  const data = JSON.stringify(obj) + '\n'
  return new Promise((resolve, reject) => {
    wx.writeBLECharacteristicValue({
      deviceId: _app().globalData.deviceId,
      serviceId: SERVICE_UUID,
      characteristicId: CHAR_UUID,
      value: stringToArrayBuffer(data),
      success: resolve,
      fail: reject
    })
  })
}

function disconnect() {
  if (!_app().globalData.deviceId) return Promise.resolve()
  return new Promise(resolve => {
    wx.closeBLEConnection({
      deviceId: _app().globalData.deviceId,
      complete: () => {
        _app().globalData.connected = false
        _app().globalData.deviceId = ''
        resolve()
      }
    })
  })
}

function stringToArrayBuffer(str) {
  const bytes = []
  for (let i = 0; i < str.length; i++) {
    const c = str.charCodeAt(i)
    if (c < 0x80) bytes.push(c)
    else if (c < 0x800) {
      bytes.push(0xc0 | (c >> 6))
      bytes.push(0x80 | (c & 0x3f))
    } else {
      bytes.push(0xe0 | (c >> 12))
      bytes.push(0x80 | ((c >> 6) & 0x3f))
      bytes.push(0x80 | (c & 0x3f))
    }
  }
  return new Uint8Array(bytes).buffer
}
function arrayBufferToString(buf) {
  const u8 = new Uint8Array(buf)
  let str = ''
  for (let i = 0; i < u8.length; i++) str += String.fromCharCode(u8[i])
  try { return decodeURIComponent(escape(str)) } catch (e) { return str }
}

module.exports = { init, scan, connect, send, disconnect, onState, _onCharChange, _resetRx }
