#!/bin/bash

# TON Validator Archive Download Optimization Script
# 歸檔下載優化腳本

set -e

echo "🚀 TON 歸檔下載優化部署腳本"
echo "=================================="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

# Check if we're in the TON directory
if [ ! -f "build/validator-engine/validator-engine" ]; then
    echo "❌ 錯誤：找不到編譯好的 validator-engine"
    echo "請確保："
    echo "1. 當前目錄是 TON 源碼根目錄"
    echo "2. 已經成功編譯了 validator-engine"
    echo "3. build/validator-engine/validator-engine 文件存在"
    exit 1
fi

# Get current user input for deployment path
echo "請輸入當前 validator-engine 的安裝路徑（默認：/usr/bin/ton/validator-engine/validator-engine）："
read -r VALIDATOR_PATH
if [ -z "$VALIDATOR_PATH" ]; then
    VALIDATOR_PATH="/usr/bin/ton/validator-engine/validator-engine"
fi

if [ ! -f "$VALIDATOR_PATH" ]; then
    echo "❌ 錯誤：找不到現有的 validator-engine 在 $VALIDATOR_PATH"
    echo "請確認路徑是否正確"
    exit 1
fi

echo "📋 部署信息："
echo "  - 源文件：build/validator-engine/validator-engine"
echo "  - 目標路徑：$VALIDATOR_PATH"
echo "  - 備份路徑：${VALIDATOR_PATH}.backup"

echo ""
echo "⚠️  警告：此操作將："
echo "  1. 停止 validator 服務"
echo "  2. 備份當前 validator-engine"
echo "  3. 部署優化版本"
echo "  4. 重啟 validator 服務"
echo ""
read -p "確認繼續？(y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "❌ 取消部署"
    exit 1
fi

echo ""
echo "🔄 開始部署..."

# Check if we need sudo
SUDO=""
if [ "$EUID" -ne 0 ]; then
    echo "需要 sudo 權限來部署和重啟服務..."
    SUDO="sudo"
fi

# Step 1: Stop validator service
echo "1️⃣ 停止 validator 服務..."
if $SUDO systemctl is-active --quiet validator; then
    $SUDO systemctl stop validator
    echo "✅ Validator 服務已停止"
else
    echo "ℹ️  Validator 服務未運行"
fi

# Step 2: Backup current version
echo "2️⃣ 備份當前版本..."
$SUDO cp "$VALIDATOR_PATH" "${VALIDATOR_PATH}.backup"
echo "✅ 備份完成：${VALIDATOR_PATH}.backup"

# Step 3: Deploy new version
echo "3️⃣ 部署優化版本..."
$SUDO cp "build/validator-engine/validator-engine" "$VALIDATOR_PATH"
$SUDO chmod +x "$VALIDATOR_PATH"
echo "✅ 優化版本部署完成"

# Step 4: Restart validator service
echo "4️⃣ 重啟 validator 服務..."
$SUDO systemctl start validator
sleep 3

if $SUDO systemctl is-active --quiet validator; then
    echo "✅ Validator 服務已成功重啟"
else
    echo "❌ Validator 服務啟動失敗，正在回滾..."
    $SUDO cp "${VALIDATOR_PATH}.backup" "$VALIDATOR_PATH"
    $SUDO systemctl start validator
    echo "🔄 已回滾到原版本"
    exit 1
fi

echo ""
echo "🎉 部署完成！"
echo ""
echo "📊 監控命令："
echo "  查看實時日誌：sudo journalctl -u validator -f"
echo "  查看優化日誌：sudo journalctl -u validator -f | grep -E '(📦|✅|❌|🔍|⬇️)'"
echo ""
echo "🔍 預期改善："
echo "  - 智能節點選擇（🔍 Selected best node）"
echo "  - 節點質量跟踪（✅ Node success, score=）"
echo "  - 自動黑名單（❌ Node blacklisted）"
echo "  - 下載進度優化（⬇️ Downloading archive slice）"
echo ""
echo "⚡ 如遇問題，回滾命令："
echo "  sudo systemctl stop validator"
echo "  sudo cp ${VALIDATOR_PATH}.backup $VALIDATOR_PATH"
echo "  sudo systemctl start validator"
echo ""
echo "✨ 祝您的 TON 驗證器運行順利！" 