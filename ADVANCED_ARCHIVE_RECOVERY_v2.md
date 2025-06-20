# 🚀 TON 高級歸檔恢復機制 v2.0

## 📋 針對您日誌問題的專門解決方案

基於您提供的實際日誌分析，我創建了一個**高級歸檔恢復系統**，專門解決網絡級數據可用性問題。

### 🎯 問題分析

您的日誌顯示：
- **特定歸檔** `#48484800` 在多個分片中大規模不可用
- **網絡級數據缺失**：大量節點返回 "archive not found"
- **我們的優化正常工作**：黑名單、評分系統都在正確運行

## 🔧 新增高級功能

### 1. **智能歸檔可用性跟踪**
```cpp
struct ArchiveAvailability {
  td::uint32 attempt_count = 0;       // 總嘗試次數
  td::uint32 not_found_count = 0;     // "not found" 次數
  td::Timestamp first_attempt;         // 首次嘗試時間
  td::Timestamp last_attempt;          // 最後嘗試時間
}
```

### 2. **指數延遲重試機制**
- **檢測條件**：5次嘗試中80%失敗 → 標記為"疑似不可用"
- **延遲策略**：30秒 → 1分鐘 → 2分鐘 → 5分鐘 → 10分鐘（最大）
- **自動恢復**：成功下載後立即重置統計

### 3. **增強日誌監控**
**新日誌標識：**
- 🕒 `"appears unavailable (attempts: X, not found: Y). Delaying retry for Zs"`
- 🔄 `"Retry delay expired for archive slice, attempting download"`
- 🔄 `"Archive slice confirmed available, resetting statistics"`

## 🚀 立即升級到 v2.0

### 快速部署命令：
```bash
# 停止驗證器
sudo systemctl stop validator

# 備份 v1 版本
sudo cp /usr/bin/ton/validator-engine/validator-engine /usr/bin/ton/validator-engine/validator-engine.v1

# 部署 v2.0 高級版本
sudo cp build/validator-engine/validator-engine /usr/bin/ton/validator-engine/validator-engine

# 重啟驗證器
sudo systemctl start validator
```

## 📊 v2.0 專門解決的問題

| 問題情況 | v1.0 行為 | v2.0 智能行為 |
|----------|-----------|---------------|
| 大量"archive not found" | 持續快速重試 | **智能延遲**：30s→10min |
| 網絡級數據缺失 | 浪費帶寬和CPU | **自適應退避**：減少無效嘗試 |
| 偶發可用性 | 錯過時機 | **即時恢復**：成功後重置統計 |
| 資源消耗 | 高頻無效請求 | **高效管理**：指數延遲減負 |

## 🔍 監控 v2.0 效果

### 實時監控新功能：
```bash
# 監控延遲機制
sudo journalctl -u validator -f | grep -E "(🕒|appears unavailable|Delaying retry)"

# 監控恢復機制
sudo journalctl -u validator -f | grep -E "(🔄|Retry delay expired|confirmed available)"

# 全面監控
sudo journalctl -u validator -f | grep -E "(📦|✅|❌|🔍|⬇️|🕒|🔄)"
```

## 🎉 預期改善效果

### 針對當前 `#48484800` 問題：

**立即效果（10分鐘內）：**
- 🕒 自動檢測並延遲重試不可用歸檔
- 📉 顯著減少無效網絡請求
- ⚡ CPU和帶寬使用率降低

**中期效果（1小時內）：**
- 🔄 其他可用歸檔正常下載，不受影響
- 📊 智能資源分配，優先處理可用數據
- 🎯 系統整體穩定性提升

**長期效果（24小時內）：**
- 🔄 數據可用性恢復時自動快速響應
- 📈 整體同步效率提升
- 🛡️ 更強的網絡波動抵抗能力

## 🛠️ 技術細節

### 延遲算法：
```
delay = 30s * (2^retry_factor)
retry_factor = min(attempt_count / 5, 10)
max_delay = 10 minutes
```

### 觸發條件：
```
is_likely_unavailable = 
  attempt_count >= 5 && 
  not_found_count > attempt_count * 0.8
```

## 🎯 特別優勢

**v2.0 專門為您的使用場景優化：**

1. **🔄 保持其他下載正常**：只影響問題歸檔，其他歸檔正常下載
2. **⚡ 智能資源管理**：避免在不可用數據上浪費資源  
3. **📈 自動適應網絡狀況**：數據恢復可用時立即響應
4. **🛡️ 增強系統穩定性**：減少頻繁失敗對系統的影響

---

**🎊 立即體驗 v2.0 的智能歸檔恢復能力！**

部署後，您會看到針對 `#48484800` 的智能延遲日誌，而其他可用歸檔繼續正常同步。

*編譯完成時間：2024年6月20日* 