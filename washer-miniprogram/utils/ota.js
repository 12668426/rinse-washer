// utils/ota.js — 云端固件升级
// 版本检查 + 下载 .bin + BLE 分包发送

const VERSION_URL = 'https://raw.githubusercontent.com/12668426/rinse-washer/main/ota/version.json'
const ble = require('./ble.js')

// 语义版本比较：a > b 返回 1，a < b 返回 -1，相等返回 0
function cmpVer(a, b) {
  const pa = a.split('.').map(Number)
  const pb = b.split('.').map(Number)
  for (let i = 0; i < 3; i++) {
    const da = pa[i] || 0
    const db = pb[i] || 0
    if (da > db) return 1
    if (da < db) return -1
  }
  return 0
}

// 检查云端版本，返回 {version, url, size, notes, hasUpdate}
function checkUpdate(currentVer) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: VERSION_URL,
      method: 'GET',
      timeout: 10000,
      success: (res) => {
        if (res.statusCode !== 200) {
          reject(new Error('version.json HTTP ' + res.statusCode))
          return
        }
        const info = res.data
        if (!info || !info.version || !info.url) {
          reject(new Error('version.json 格式错误'))
          return
        }
        const hasUpdate = cmpVer(info.version, currentVer) > 0
        resolve({
          version: info.version,
          url: info.url,
          size: info.size || 0,
          notes: info.notes || '',
          hasUpdate
        })
      },
      fail: (err) => reject(new Error('网络请求失败：' + (err.errMsg || err)))
    })
  })
}

// 从 URL 下载 .bin 到本地临时文件，返回 {tempFilePath, size}
function downloadFirmware(url, onProgress) {
  return new Promise((resolve, reject) => {
    wx.downloadFile({
      url,
      success: (res) => {
        if (res.statusCode !== 200) {
          reject(new Error('下载 HTTP ' + res.statusCode))
          return
        }
        resolve({ tempFilePath: res.tempFilePath })
      },
      fail: (err) => reject(new Error('下载失败：' + (err.errMsg || err)))
    })
  })
}

// 读取本地文件为 ArrayBuffer
function readFileAsArrayBuffer(filePath) {
  return new Promise((resolve, reject) => {
    wx.getFileSystemManager().readFile({
      filePath,
      success: (res) => resolve(res.data),
      fail: (err) => reject(new Error('读取文件失败：' + (err.errMsg || err)))
    })
  })
}

// base64 编码
function base64Encode(arrayBuffer) {
  const bytes = new Uint8Array(arrayBuffer)
  let binary = ''
  for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i])
  return wx.arrayBufferToBase64 ? wx.arrayBufferToBase64(arrayBuffer) : btoa(binary)
}

// 分包推送固件到 ESP32（通过 BLE）
// onProgress: (phase, percent) => void  phase: 'begin'|'data'|'end'
async function pushFirmware(arrayBuffer, onProgress) {
  const CHUNK_SIZE = 4096  // 每包 4KB
  const totalSize = arrayBuffer.byteLength
  const totalChunks = Math.ceil(totalSize / CHUNK_SIZE)

  // 1. 开始 OTA
  onProgress && onProgress('begin', 0)
  await ble.send({ cmd: 'ota_begin', size: totalSize })

  // 2. 分包发送
  for (let i = 0; i < totalChunks; i++) {
    const offset = i * CHUNK_SIZE
    const end = Math.min(offset + CHUNK_SIZE, totalSize)
    const chunk = arrayBuffer.slice(offset, end)
    const b64 = base64Encode(chunk)
    await ble.send({ cmd: 'ota_data', seq: i, data: b64 })
    onProgress && onProgress('data', Math.round((i + 1) / totalChunks * 100))
  }

  // 3. 结束
  onProgress && onProgress('end', 100)
  await ble.send({ cmd: 'ota_end' })
}

module.exports = { checkUpdate, downloadFirmware, readFileAsArrayBuffer, pushFirmware, cmpVer }
