# OTA 固件上传指南

每次改完固件后，手动把 .bin 和 version.json 推到 GitHub。

## 步骤

```bash
# 1. 推源码
git add -A
git commit -m "vX.X.X: 描述"
git push origin main

# 2. 安装 gh CLI（只需一次）
brew install gh && gh auth login

# 3. 创建 Release + 上传 bin
gh release create vX.X.X ota/esp32_dual_washer.ino.bin --title "vX.X.X" --notes "更改内容"
```

## 目录结构

```
ota/
├── esp32_dual_washer.ino.bin   ← 编译好的固件
├── version.json                ← 小程序读的版本元数据
└── OTA_UPLOAD.md               ← 本文件
```

## 注意

- Release tag 必须和 version.json 里的 version 一致
- bin 文件名必须是 esp32_dual_washer.ino.bin
- 改固件后先 git push，再 gh release create
