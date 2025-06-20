#!/bin/bash

# TON 節點性能實時監控儀表板
# Real-time Node Performance Dashboard for TON

set -euo pipefail

# 顏色定義
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
WHITE='\033[1;37m'
GRAY='\033[0;37m'
NC='\033[0m'

# 全局變量
declare -A NODE_DATA
declare -A NODE_STATUS
declare -A NODE_LAST_SEEN
declare -A NODE_TOTAL_SUCCESS
declare -A NODE_TOTAL_FAILURES
declare -A NODE_SCORE
declare -A NODE_SUCCESS_RATE
declare -A NODE_ATTEMPTS

LOG_DIR="/var/ton-work"
TEMP_DIR="/tmp/ton_monitor"
NODE_FILE="$TEMP_DIR/nodes.dat"
STATS_FILE="$TEMP_DIR/stats.dat"

# 創建臨時目錄
mkdir -p "$TEMP_DIR"

# 統計變量
TOTAL_DOWNLOADS=0
TOTAL_SUCCESS=0
TOTAL_FAILURES=0
BLACKLISTED_NODES=0
ACTIVE_NODES=0
NEW_NODES=0
START_TIME=$(date +%s)

# 清理函數
cleanup() {
    rm -rf "$TEMP_DIR"
    tput cnorm  # 恢復游標
    clear
}
trap cleanup EXIT

# 隱藏游標
tput civis

# 節點ID截短函數
shorten_node_id() {
    local node_id="$1"
    echo "${node_id:0:8}...${node_id: -8}"
}

# 解析節點狀態信息
parse_node_info() {
    local line="$1"
    local timestamp="$2"
    
    # 提取節點ID
    if [[ "$line" =~ Node[[:space:]]+([A-Za-z0-9+/=]+) ]]; then
        local node_id="${BASH_REMATCH[1]}"
        NODE_LAST_SEEN["$node_id"]="$timestamp"
        
        # 解析不同類型的節點信息
        if [[ "$line" =~ "is BLACKLISTED" ]]; then
            NODE_STATUS["$node_id"]="BLACKLISTED"
            ((BLACKLISTED_NODES++))
            
            # 提取評分和統計信息
            if [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]]; then
                NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]]; then
                NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Attempts:[[:space:]]+([0-9]+)" ]]; then
                NODE_ATTEMPTS["$node_id"]="${BASH_REMATCH[1]}"
            fi
            
        elif [[ "$line" =~ "SUCCESS" ]]; then
            NODE_STATUS["$node_id"]="SUCCESS"
            ((NODE_TOTAL_SUCCESS["$node_id"]++))
            ((TOTAL_SUCCESS++))
            
            # 提取成功時的統計信息
            if [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]]; then
                NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]]; then
                NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Attempts:[[:space:]]+([0-9]+)" ]]; then
                NODE_ATTEMPTS["$node_id"]="${BASH_REMATCH[1]}"
            fi
            
        elif [[ "$line" =~ "ARCHIVE NOT FOUND" ]] || [[ "$line" =~ "FAILED" ]]; then
            NODE_STATUS["$node_id"]="FAILED"
            ((NODE_TOTAL_FAILURES["$node_id"]++))
            ((TOTAL_FAILURES++))
            
            # 提取失敗時的統計信息
            if [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]]; then
                NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]]; then
                NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Attempts:[[:space:]]+([0-9]+)" ]]; then
                NODE_ATTEMPTS["$node_id"]="${BASH_REMATCH[1]}"
            fi
            
        elif [[ "$line" =~ "Using completely unknown node" ]] || [[ "$line" =~ "🆕" ]]; then
            NODE_STATUS["$node_id"]="NEW"
            NODE_SCORE["$node_id"]="0.2"
            NODE_SUCCESS_RATE["$node_id"]="0"
            NODE_ATTEMPTS["$node_id"]="0"
            ((NEW_NODES++))
            
        elif [[ "$line" =~ "REUSING EXCELLENT NODE" ]]; then
            NODE_STATUS["$node_id"]="EXCELLENT"
            
            # 提取優秀節點統計信息
            if [[ "$line" =~ "Success Count:[[:space:]]+([0-9]+)" ]]; then
                NODE_TOTAL_SUCCESS["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]]; then
                NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            
        elif [[ "$line" =~ "REUSING GOOD NODE" ]]; then
            NODE_STATUS["$node_id"]="GOOD"
            
            # 提取好節點統計信息
            if [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]]; then
                NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            fi
            if [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]]; then
                NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            fi
        fi
        
        # 初始化節點數據（如果尚未存在）
        if [[ -z "${NODE_TOTAL_SUCCESS[$node_id]:-}" ]]; then
            NODE_TOTAL_SUCCESS["$node_id"]=0
        fi
        if [[ -z "${NODE_TOTAL_FAILURES[$node_id]:-}" ]]; then
            NODE_TOTAL_FAILURES["$node_id"]=0
        fi
    fi
}

# 解析下載事件
parse_download_events() {
    local line="$1"
    
    if [[ "$line" =~ "Successfully downloaded archive slice" ]]; then
        ((TOTAL_SUCCESS++))
    elif [[ "$line" =~ "Failed to download archive slice" ]]; then
        ((TOTAL_FAILURES++))
    fi
    
    if [[ "$line" =~ "📦 Starting optimized download" ]]; then
        ((TOTAL_DOWNLOADS++))
    fi
}

# 顯示儀表板標題
show_header() {
    local current_time=$(date '+%Y-%m-%d %H:%M:%S')
    local runtime=$(($(date +%s) - START_TIME))
    local hours=$((runtime / 3600))
    local minutes=$(((runtime % 3600) / 60))
    local seconds=$((runtime % 60))
    
    printf "${WHITE}╔═══════════════════════════════════════════════════════════════════════════════════╗${NC}\n"
    printf "${WHITE}║${CYAN}                    🚀 TON 節點性能實時監控儀表板                               ${WHITE}║${NC}\n"
    printf "${WHITE}╠═══════════════════════════════════════════════════════════════════════════════════╣${NC}\n"
    printf "${WHITE}║${NC} 當前時間: ${BLUE}%s${NC} │ 運行時間: ${YELLOW}%02d:%02d:%02d${WHITE} ║${NC}\n" "$current_time" "$hours" "$minutes" "$seconds"
    printf "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}\n"
}

# 顯示統計摘要
show_statistics() {
    local success_rate=0
    if [[ $((TOTAL_SUCCESS + TOTAL_FAILURES)) -gt 0 ]]; then
        success_rate=$((TOTAL_SUCCESS * 100 / (TOTAL_SUCCESS + TOTAL_FAILURES)))
    fi
    
    local total_nodes=${#NODE_DATA[@]}
    local excellent_nodes=$(printf '%s\n' "${NODE_STATUS[@]}" | grep -c "EXCELLENT" || true)
    local good_nodes=$(printf '%s\n' "${NODE_STATUS[@]}" | grep -c "GOOD" || true)
    
    printf "${WHITE}╔══════════════════════════════════ 📊 統計摘要 ═══════════════════════════════════╗${NC}\n"
    printf "${WHITE}║${NC} 下載總數: ${CYAN}%-6d${NC} │ 成功: ${GREEN}%-6d${NC} │ 失敗: ${RED}%-6d${NC} │ 成功率: ${YELLOW}%3d%%${WHITE} ║${NC}\n" \
        "$TOTAL_DOWNLOADS" "$TOTAL_SUCCESS" "$TOTAL_FAILURES" "$success_rate"
    printf "${WHITE}║${NC} 節點總數: ${CYAN}%-6d${NC} │ 優秀: ${GREEN}%-6d${NC} │ 良好: ${BLUE}%-6d${NC} │ 封鎖: ${RED}%3d${WHITE}  ║${NC}\n" \
        "$total_nodes" "$excellent_nodes" "$good_nodes" "$BLACKLISTED_NODES"
    printf "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}\n"
}

# 顯示節點表格
show_node_table() {
    printf "${WHITE}╔══════════════════════════════════ 🔗 節點狀態表 ═══════════════════════════════════╗${NC}\n"
    printf "${WHITE}║${NC} %-20s │ %-12s │ %-8s │ %-8s │ %-8s │ %-12s ${WHITE}║${NC}\n" \
        "節點ID" "狀態" "評分" "成功率%" "嘗試數" "最後更新"
    printf "${WHITE}╠══════════════════════════════════════════════════════════════════════════════════╣${NC}\n"
    
    # 按狀態和評分排序節點
    local sorted_nodes=()
    while IFS= read -r node_id; do
        sorted_nodes+=("$node_id")
    done < <(
        for node_id in "${!NODE_STATUS[@]}"; do
            local status="${NODE_STATUS[$node_id]}"
            local score="${NODE_SCORE[$node_id]:-0}"
            printf "%s:%s:%s\n" "$status" "$score" "$node_id"
        done | sort -t: -k1,1 -k2,2nr | cut -d: -f3 | head -20
    )
    
    for node_id in "${sorted_nodes[@]}"; do
        local short_id=$(shorten_node_id "$node_id")
        local status="${NODE_STATUS[$node_id]}"
        local score="${NODE_SCORE[$node_id]:-N/A}"
        local success_rate="${NODE_SUCCESS_RATE[$node_id]:-N/A}"
        local attempts="${NODE_ATTEMPTS[$node_id]:-N/A}"
        local last_seen="${NODE_LAST_SEEN[$node_id]:-N/A}"
        
        # 根據狀態選擇顏色
        local status_color=""
        case "$status" in
            "EXCELLENT") status_color="$GREEN" ;;
            "GOOD") status_color="$BLUE" ;;
            "SUCCESS") status_color="$CYAN" ;;
            "NEW") status_color="$YELLOW" ;;
            "FAILED") status_color="$RED" ;;
            "BLACKLISTED") status_color="$MAGENTA" ;;
            *) status_color="$GRAY" ;;
        esac
        
        # 格式化最後更新時間
        local time_display="N/A"
        if [[ "$last_seen" != "N/A" ]]; then
            time_display=$(date -d "@$last_seen" '+%H:%M:%S' 2>/dev/null || echo "N/A")
        fi
        
        printf "${WHITE}║${NC} %-20s │ ${status_color}%-12s${NC} │ %-8s │ %-8s │ %-8s │ %-12s ${WHITE}║${NC}\n" \
            "$short_id" "$status" "$score" "$success_rate" "$attempts" "$time_display"
    done
    
    printf "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}\n"
}

# 顯示最近事件
show_recent_events() {
    printf "${WHITE}╔═══════════════════════════════ 📝 最近事件 (最新10條) ══════════════════════════════╗${NC}\n"
    
    # 讀取最近的日誌條目
    tail -n 10 "$TEMP_DIR/recent_events.log" 2>/dev/null | while read -r event; do
        if [[ -n "$event" ]]; then
            printf "${WHITE}║${NC} %-83s ${WHITE}║${NC}\n" "$event"
        fi
    done
    
    printf "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}\n"
}

# 更新顯示
update_display() {
    clear
    show_header
    echo
    show_statistics
    echo
    show_node_table
    echo
    show_recent_events
    echo
    printf "${GRAY}按 Ctrl+C 退出監控...${NC}\n"
}

# 處理日誌行
process_log_line() {
    local file="$1"
    local line="$2"
    
    # 解析時間戳
    local timestamp=""
    if [[ "$line" =~ \[([0-9]{4}-[0-9]{2}-[0-9]{2}[[:space:]]+[0-9]{2}:[0-9]{2}:[0-9]{2}) ]]; then
        timestamp=$(date -d "${BASH_REMATCH[1]}" +%s 2>/dev/null || echo "$(date +%s)")
    else
        timestamp=$(date +%s)
    fi
    
    # 記錄最近事件
    local event_time=$(date -d "@$timestamp" '+%H:%M:%S' 2>/dev/null || date '+%H:%M:%S')
    if [[ "$line" =~ (📦|✅|❌|🚫|🔍|🆕|🏆|⭐) ]]; then
        local event_desc=$(echo "$line" | sed -E 's/.*\](.*)/\1/' | cut -c1-70)
        echo "[$event_time] $event_desc" >> "$TEMP_DIR/recent_events.log"
        
        # 保持最近事件文件不超過50行
        tail -n 50 "$TEMP_DIR/recent_events.log" > "$TEMP_DIR/recent_events.tmp" && \
        mv "$TEMP_DIR/recent_events.tmp" "$TEMP_DIR/recent_events.log"
    fi
    
    # 解析節點信息和下載事件
    parse_node_info "$line" "$timestamp"
    parse_download_events "$line"
}

# 主監控循環
main_monitor() {
    echo "正在初始化監控系統..."
    
    # 檢查日誌目錄
    if [[ ! -d "$LOG_DIR" ]]; then
        echo "錯誤：日誌目錄 $LOG_DIR 不存在！"
        exit 1
    fi
    
    # 初始化最近事件文件
    touch "$TEMP_DIR/recent_events.log"
    
    echo "開始監控日誌文件..."
    
    # 使用 tail -F 監控所有日誌文件
    tail -F "$LOG_DIR"/log.t*.log 2>/dev/null | while read -r line; do
        if [[ -n "$line" ]]; then
            process_log_line "" "$line"
            
            # 每處理10行更新一次顯示
            if [[ $((RANDOM % 10)) -eq 0 ]]; then
                update_display
            fi
        fi
    done &
    
    # 定期更新顯示
    while true; do
        update_display
        sleep 2
    done
}

# 檢查權限
if [[ ! -r "$LOG_DIR" ]]; then
    echo "錯誤：無法讀取日誌目錄 $LOG_DIR"
    echo "請確保您有適當的權限，或使用 sudo 運行此腳本"
    exit 1
fi

# 啟動監控
main_monitor 