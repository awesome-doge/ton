#!/bin/bash

echo "📊 TON Archive Download Performance Monitor"
echo "============================================="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Performance counters
SUCCESS_COUNT=0
FAILURE_COUNT=0
NODE_SELECTIONS=0
START_TIME=$(date +%s)

print_header() {
    clear
    echo -e "${CYAN}📊 TON Archive Download Performance Monitor${NC}"
    echo "============================================="
    current_time=$(date '+%Y-%m-%d %H:%M:%S')
    runtime=$(($(date +%s) - START_TIME))
    echo -e "${BLUE}當前時間: ${current_time} | 運行時間: ${runtime}s${NC}"
    
    if [ $((SUCCESS_COUNT + FAILURE_COUNT)) -gt 0 ]; then
        success_rate=$((SUCCESS_COUNT * 100 / (SUCCESS_COUNT + FAILURE_COUNT)))
        echo -e "${GREEN}成功: ${SUCCESS_COUNT}${NC} | ${RED}失敗: ${FAILURE_COUNT}${NC} | ${YELLOW}成功率: ${success_rate}%${NC} | ${CYAN}節點選擇: ${NODE_SELECTIONS}${NC}"
    else
        echo -e "${YELLOW}等待下載事件...${NC}"
    fi
    echo "============================================="
}

# Function to parse and display log entries
parse_log_entry() {
    local line="$1"
    local timestamp=$(echo "$line" | cut -d'│' -f1 | xargs)
    local thread=$(echo "$line" | cut -d'│' -f2 | xargs)
    local time=$(echo "$line" | cut -d'│' -f3 | xargs)
    local message=$(echo "$line" | cut -d'│' -f4- | xargs)
    
    # Count events
    if [[ "$message" == *"Selected best node"* ]]; then
        NODE_SELECTIONS=$((NODE_SELECTIONS + 1))
    elif [[ "$message" == *"Successfully downloaded archive slice"* ]]; then
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
    elif [[ "$message" == *"Failed to download archive slice"* ]]; then
        FAILURE_COUNT=$((FAILURE_COUNT + 1))
    fi
    
    # Color code the output
    if [[ "$message" == *"📦"* ]]; then
        echo -e "${BLUE}[${time}] ${thread} 📦 ${message}${NC}"
    elif [[ "$message" == *"✅"* ]]; then
        echo -e "${GREEN}[${time}] ${thread} ✅ ${message}${NC}"
    elif [[ "$message" == *"❌"* ]] || [[ "$message" == *"🚫"* ]]; then
        echo -e "${RED}[${time}] ${thread} ❌ ${message}${NC}"
    elif [[ "$message" == *"⬇️"* ]]; then
        echo -e "${CYAN}[${time}] ${thread} ⬇️ ${message}${NC}"
    elif [[ "$message" == *"🔍"* ]]; then
        echo -e "${YELLOW}[${time}] ${thread} 🔍 ${message}${NC}"
    else
        echo -e "[${time}] ${thread} ${message}"
    fi
}

echo -e "${YELLOW}正在監控 TON 驗證器日誌中的下載事件...${NC}"
echo -e "${BLUE}按 Ctrl+C 退出監控${NC}"
echo ""

# Check if validator service is running
if ! systemctl is-active --quiet validator; then
    echo -e "${RED}❌ 驗證器服務未運行！${NC}"
    echo -e "${YELLOW}請先啟動服務: systemctl start validator${NC}"
    exit 1
fi

# Monitor the logs
print_header

# Use journalctl to follow the logs and filter for download-related messages
journalctl -u validator -f --since "now" | grep --line-buffered -E "(📦|✅|❌|⬇️|🔍|archive|download)" | while read -r line; do
    # Update header every 20 lines
    if [ $(($(date +%s) % 10)) -eq 0 ]; then
        print_header
    fi
    
    parse_log_entry "$line"
done 