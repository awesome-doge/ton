# 🚀 TON 節點性能監控工具

這個工具包包含兩個腳本，用於監控和分析 TON 驗證器的節點下載性能：

## 📁 工具概覽

### 1. `analyze-node-logs.sh` - 日誌分析器 (靜態分析)
- 📊 分析現有日誌文件，生成節點性能報告
- 🔍 識別優秀節點、封鎖節點和新節點
- 📈 顯示下載成功率和統計信息
- 💡 提供優化建議

### 2. `node-performance-dashboard.sh` - 實時監控儀表板
- 📱 實時監控節點狀態變化
- 📊 動態更新統計信息
- 🔄 顯示最近事件日誌
- ⚡ 實時顯示節點評分和成功率

## 🛠️ 安裝與設置

### 步驟1：複製腳本到服務器
```bash
# 上傳腳本到您的 TON 服務器
scp analyze-node-logs.sh node-performance-dashboard.sh user@your-server:/home/user/
```

### 步驟2：設置權限
```bash
chmod +x analyze-node-logs.sh
chmod +x node-performance-dashboard.sh
```

### 步驟3：確保日誌路徑正確
默認日誌路徑是 `/var/ton-work/log.t*.log`，如果您的日誌在其他位置，請相應調整。

## 📖 使用方法

### 🔍 靜態日誌分析 (推薦先使用)

#### 基本用法：
```bash
# 分析默認日誌目錄 /var/ton-work
./analyze-node-logs.sh

# 分析自定義目錄
./analyze-node-logs.sh /path/to/your/logs

# 顯示幫助
./analyze-node-logs.sh --help
```

#### 示例輸出：
```
🔍 TON 節點性能日誌分析器
================================

正在分析日誌文件...
找到 16 個日誌文件
分析: log.thread1.log
分析: log.thread2.log
...
日誌分析完成！

╔═══════════════════════════════════════════════════════════════════════════════════╗
║                        📊 TON 節點性能分析報告                                 ║
╠═══════════════════════════════════════════════════════════════════════════════════╣
║ 分析時間: 2025-06-20 17:15:30                                                    ║
║ 日誌路徑: /var/ton-work                                                         ║
╚═══════════════════════════════════════════════════════════════════════════════════╝

╔══════════════════════════════════ 📊 統計摘要 ═══════════════════════════════════╗
║ 下載總數: 234    │ 成功: 156    │ 失敗: 78     │ 成功率:  67% ║
║ 節點總數: 45     │ 優秀: 8      │ 良好: 12     │ 封鎖: 15   ║
╚═══════════════════════════════════════════════════════════════════════════════════╝
```

### 📱 實時監控儀表板

#### 基本用法：
```bash
# 實時監控 (需要 root 權限讀取日誌)
sudo ./node-performance-dashboard.sh

# 或使用 sudo -E 保持環境變量
sudo -E ./node-performance-dashboard.sh
```

#### 儀表板功能：
- **實時更新**：每2秒更新一次顯示
- **顏色編碼**：
  - 🟢 綠色：優秀節點 (EXCELLENT)
  - 🔵 藍色：良好節點 (GOOD)
  - 🟡 黃色：新節點 (NEW)
  - 🔴 紅色：失敗節點 (FAILED)
  - 🟣 紫色：封鎖節點 (BLACKLISTED)

## 📊 理解輸出結果

### 節點狀態說明

| 狀態 | 描述 | 顏色 |
|------|------|------|
| EXCELLENT | 最近1小時內有成功下載的優秀節點 | 🟢 綠色 |
| GOOD | 成功率 ≥ 50% 且有良好記錄的節點 | 🔵 藍色 |
| SUCCESS | 最近成功的節點 | 🔻 青色 |
| NEW | 完全未知的新節點 | 🟡 黃色 |
| FAILED | 最近失敗的節點 | 🔴 紅色 |
| BLACKLISTED | 被系統封鎖的節點 | 🟣 紫色 |

### 關鍵指標解釋

- **評分 (Score)**：節點質量評分 (0.0-1.0)，越高越好
- **成功率 (%)**：成功下載的百分比
- **嘗試數**：總共嘗試連接該節點的次數
- **最後更新**：最後一次看到該節點活動的時間

## 🎯 優化建議

### 根據分析結果優化：

#### ✅ 成功率高 (>80%)
- 系統運行良好，優化策略生效
- 繼續監控，保持當前配置

#### ⚠️ 成功率中等 (50-80%)
- 可能需要等待更多優秀節點
- 檢查網絡連接穩定性

#### 🚫 成功率低 (<50%)
- 檢查網絡連接
- 確認 TON 網絡狀態
- 考慮重啟驗證器服務

#### 🔍 節點分析建議：
- **優秀節點多**：系統會自動重用這些節點
- **封鎖節點過多**：可能需要調整節點選擇策略
- **新節點頻繁**：系統正在探索，等待穩定

## 🛠️ 故障排除

### 常見問題：

#### 1. 權限問題
```bash
# 錯誤：無法讀取日誌目錄
sudo ./analyze-node-logs.sh
```

#### 2. 日誌文件不存在
```bash
# 檢查日誌文件位置
ls -la /var/ton-work/log.t*.log

# 使用自定義路徑
./analyze-node-logs.sh /actual/log/path
```

#### 3. 實時監控無數據
```bash
# 確保驗證器服務正在運行
systemctl status validator

# 檢查日誌是否在更新
tail -f /var/ton-work/log.thread1.log
```

#### 4. 腳本執行權限
```bash
# 設置執行權限
chmod +x *.sh

# 檢查腳本語法
bash -n analyze-node-logs.sh
```

## 📈 預期改進效果

使用我們的優化後的 validator-engine，您應該看到：

1. **🏆 更多優秀節點重用**：
   ```
   🏆 REUSING EXCELLENT NODE: I2Qd6WT1+Ekpo5/tJF...
   ```

2. **🔻 減少失敗嘗試**：
   ```
   🎯 Using only 3 high-quality nodes, ignoring 12 new nodes
   ```

3. **📈 成功率提升**：
   - 從 20-30% 提升到 60-80%+
   - 減少對失敗節點的重複嘗試

4. **⚡ 更快的下載速度**：
   - 優先使用高速節點
   - 減少無效嘗試

## 🔄 持續監控建議

### 每日檢查：
```bash
# 生成每日報告
./analyze-node-logs.sh > daily-report-$(date +%Y%m%d).txt
```

### 實時監控（在問題期間）：
```bash
# 啟動實時監控
sudo ./node-performance-dashboard.sh
```

### 週期性分析：
- 每天運行靜態分析查看趨勢
- 在遇到下載問題時啟動實時監控
- 根據報告調整配置

## 💡 進階技巧

### 1. 創建監控別名
```bash
# 添加到 ~/.bashrc
alias ton-analyze="cd /path/to/scripts && ./analyze-node-logs.sh"
alias ton-monitor="cd /path/to/scripts && sudo ./node-performance-dashboard.sh"
```

### 2. 設置定時分析
```bash
# 添加到 crontab
0 */6 * * * /path/to/analyze-node-logs.sh > /tmp/ton-analysis.log
```

### 3. 結合日誌過濾
```bash
# 只查看最近的存檔相關日誌
tail -f /var/ton-work/log.t*.log | grep -E "(📦|✅|❌|archive)"
```

---

## 📞 技術支持

如果您遇到問題或需要定制功能，請提供：
1. 錯誤訊息和完整輸出
2. 您的 TON 設置詳情
3. 日誌文件示例

這些工具將幫助您更好地理解和優化 TON 節點的下載性能！ 