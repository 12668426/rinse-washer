// utils/ota.js — 云端固件升级
// 版本检查 + 下载 .bin + BLE 分包发送

const VERSION_URL = 'https://raw.githubusercontent.com/12668426/rinse-washer/main/ota/version.json'
// 本地小程序内置版本（每次发新版 .bin 时同步改这里）
const APP_FW_VERSION = '1.0.0'

// 简单语义版本比较：a > b 返回 1, a < b 返回 -1, 相等 0
function cmpVer(a, b) {
  const pa = a.split('.').map(Number)
  const pb = b.split('.').map(Number)
  for (let i = 0; i < 3; i++) {
    const va = pa[i] || 0
    const vb = pb[i] || 0
    if (va > vb) return 1
    if (va < vb) return -1
  }
  return 0
}

// 检查云端是否有新版本
// 返回 {hasUpdate, version, url, size, notes} 或抛异常
function checkUpdate(currentVer) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: VERSION_URL,
      method: 'GET',
      timeout: 8000,
      success: (res) => {
        if (res.statusCode !== 200 || !res.data) {
          reject(new Error('版本接口异常'))
          return
        }
        const data = res.data
        const remoteVer = data.version || '0.0.0'
        const hasUpdate = cmpVer(remoteVer, currentVer) > 0
        resolve({
          hasUpdate,
          version: remoteVer,
          url: data.url,
          size: data.size || 0,
          notes: data.notes || ''
        })
      },
      fail: (err) => {
        reject(new Error('网络请求失败: ' + err.errMsg))
      }
    })
  })
}

// 下载固件 .bin 到本地临时文件
// onProgress(percent) 回调下载进度
function downloadFw(url, onProgress) {
  return new Promise((resolve, reject) => {
    const task = wx.downloadFile({
      url,
      success: (res) => {
        if (res.statusCode === 200) {
          resolve(res.tempFilePath)
        } else {
          reject(new Error('下载失败: HTTP ' + res.statusCode))
        }
      },
      fail: (err) => reject(new Error('下载失败: ' + err.errMsg))
    })
    if (task && onProgress) {
      task.onProgressUpdate((p) => onProgress(p.progress))
    }
  })
}

// 读取文件并分包通过 BLE 发送
// bleSend 是 ble.js 的发送函数
// onOtaProgress(percent) 回调发送进度
async function pushToFw(filePath, bleSend, onOtaProgress) {
  const fs = wx.getFileSystemManager()
  let fileData
  try {
    fileData = fs.readFileSync(filePath)
  } catch (e) {
    throw new Error('读取固件文件失败')
  }

  const fileSize = fileData.length
  if (fileSize <= 0) throw new Error('固件文件为空')

  // 1. OTA begin
  await bleSend({ cmd: 'ota_begin', size: fileSize })

  // 2. 分包发送（4KB/包）
  const CHUNK = 4096
  const total = Math.ceil(fileSize / CHUNK)
  for (let i = 0; i < total; i++) {
    const start = i * CHUNK
    const end = Math.min(start + CHUNK, fileSize)
    const chunk = fileData.slice(start, end)
    const b64 = wx.arrayBufferToBase64(chunk)
    await bleSend({ cmd: 'ota_data', seq: i, data: b64 })
    if (onOtaProgress) onOtaProgress(Math.floor(((i + 1) / total) * 100))
    await new Promise(r => setTimeout(r, 30))
  }

  // 3. OTA end
  await bleSend({ cmd: 'ota_end' })
}

module.exports = {
  VERSION_URL,
  APP_FW_VERSION,
  cmpVer,
  checkUpdate,
  downloadFw,
  pushToFw
}
