#!/bin/bash

# TON 節點性能日誌分析器
# Analyze existing TON node logs for performance insights

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
declare -A NODE_STATUS
declare -A NODE_SCORE
declare -A NODE_SUCCESS_RATE
declare -A NODE_ATTEMPTS
declare -A NODE_SUCCESS_COUNT
declare -A NODE_FAILURE_COUNT
declare -A NODE_LAST_SEEN

# 統計變量
TOTAL_DOWNLOADS=0
TOTAL_SUCCESS=0
TOTAL_FAILURES=0
BLACKLISTED_NODES=0
EXCELLENT_NODES=0
GOOD_NODES=0
NEW_NODES=0

# 日誌目錄
LOG_DIR="/var/ton-work"

# 如果提供了參數，則使用參數作為日誌目錄
if [[ $# -gt 0 ]]; then
    LOG_DIR="$1"
fi

# 節點ID截短函數
shorten_node_id() {
    local node_id="$1"
    echo "${node_id:0:12}...${node_id: -8}"
}

# 解析節點信息
parse_node_info() {
    local line="$1"
    
    # 提取節點ID
    if [[ "$line" =~ Node[[:space:]]+([A-Za-z0-9+/=]+) ]]; then
        local node_id="${BASH_REMATCH[1]}"
        
        # 解析不同類型的節點信息
        if [[ "$line" =~ "is BLACKLISTED" ]]; then
            NODE_STATUS["$node_id"]="BLACKLISTED"
            
            # 提取統計信息
            [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]] && NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]] && NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Attempts:[[:space:]]+([0-9]+)" ]] && NODE_ATTEMPTS["$node_id"]="${BASH_REMATCH[1]}"
            
        elif [[ "$line" =~ "SUCCESS" ]]; then
            NODE_STATUS["$node_id"]="SUCCESS"
            ((NODE_SUCCESS_COUNT["$node_id"]++))
            
            [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]] && NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]] && NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Attempts:[[:space:]]+([0-9]+)" ]] && NODE_ATTEMPTS["$node_id"]="${BASH_REMATCH[1]}"
            
        elif [[ "$line" =~ "ARCHIVE NOT FOUND" ]] || [[ "$line" =~ "FAILED" ]]; then
            NODE_STATUS["$node_id"]="FAILED"
            ((NODE_FAILURE_COUNT["$node_id"]++))
            
            [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]] && NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]] && NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Attempts:[[:space:]]+([0-9]+)" ]] && NODE_ATTEMPTS["$node_id"]="${BASH_REMATCH[1]}"
            
        elif [[ "$line" =~ "Using completely unknown node" ]] || [[ "$line" =~ "🆕" ]]; then
            NODE_STATUS["$node_id"]="NEW"
            NODE_SCORE["$node_id"]="0.2"
            NODE_SUCCESS_RATE["$node_id"]="0"
            NODE_ATTEMPTS["$node_id"]="0"
            
        elif [[ "$line" =~ "REUSING EXCELLENT NODE" ]]; then
            NODE_STATUS["$node_id"]="EXCELLENT"
            [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]] && NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
            
        elif [[ "$line" =~ "REUSING GOOD NODE" ]]; then
            NODE_STATUS["$node_id"]="GOOD"
            [[ "$line" =~ Score:[[:space:]]+([0-9.]+) ]] && NODE_SCORE["$node_id"]="${BASH_REMATCH[1]}"
            [[ "$line" =~ "Success Rate:[[:space:]]+([0-9.]+)%" ]] && NODE_SUCCESS_RATE["$node_id"]="${BASH_REMATCH[1]}"
        fi
        
        # 初始化計數器
        [[ -z "${NODE_SUCCESS_COUNT[$node_id]:-}" ]] && NODE_SUCCESS_COUNT["$node_id"]=0
        [[ -z "${NODE_FAILURE_COUNT[$node_id]:-}" ]] && NODE_FAILURE_COUNT["$node_id"]=0
        
        # 記錄最後看到時間
        NODE_LAST_SEEN["$node_id"]=$(date '+%H:%M:%S')
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

# 分析日誌文件
analyze_logs() {
    echo -e "${CYAN}正在分析日誌文件...${NC}"
    
    # 檢查日誌目錄
    if [[ ! -d "$LOG_DIR" ]]; then
        echo -e "${RED}錯誤：日誌目錄 $LOG_DIR 不存在！${NC}"
        exit 1
    fi
    
    # 讀取最近的日誌內容 (最後1000行)
    local log_files=("$LOG_DIR"/log.t*.log)
    
    if [[ ${#log_files[@]} -eq 0 ]] || [[ ! -f "${log_files[0]}" ]]; then
        echo -e "${RED}錯誤：在 $LOG_DIR 中找不到日誌文件！${NC}"
        exit 1
    fi
    
    echo -e "${YELLOW}找到 ${#log_files[@]} 個日誌文件${NC}"
    
    # 分析每個日誌文件
    for log_file in "${log_files[@]}"; do
        if [[ -f "$log_file" ]]; then
            echo -e "${GRAY}分析: $(basename "$log_file")${NC}"
            
            # 讀取最後1000行進行分析
            tail -n 1000 "$log_file" | while read -r line; do
                parse_node_info "$line"
                parse_download_events "$line"
            done
        fi
    done
    
    echo -e "${GREEN}日誌分析完成！${NC}"
}

# 統計節點類型
count_node_types() {
    BLACKLISTED_NODES=0
    EXCELLENT_NODES=0
    GOOD_NODES=0
    NEW_NODES=0
    
    for status in "${NODE_STATUS[@]}"; do
        case "$status" in
            "BLACKLISTED") ((BLACKLISTED_NODES++)) ;;
            "EXCELLENT") ((EXCELLENT_NODES++)) ;;
            "GOOD") ((GOOD_NODES++)) ;;
            "NEW") ((NEW_NODES++)) ;;
        esac
    done
}

# 顯示分析結果
show_results() {
    count_node_types
    
    local success_rate=0
    if [[ $((TOTAL_SUCCESS + TOTAL_FAILURES)) -gt 0 ]]; then
        success_rate=$((TOTAL_SUCCESS * 100 / (TOTAL_SUCCESS + TOTAL_FAILURES)))
    fi
    
    local total_nodes=${#NODE_STATUS[@]}
    
    echo
    echo -e "${WHITE}╔═══════════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${WHITE}║${CYAN}                        📊 TON 節點性能分析報告                                 ${WHITE}║${NC}"
    echo -e "${WHITE}╠═══════════════════════════════════════════════════════════════════════════════════╣${NC}"
    echo -e "${WHITE}║${NC} 分析時間: ${BLUE}$(date '+%Y-%m-%d %H:%M:%S')${NC}                                           ${WHITE}║${NC}"
    echo -e "${WHITE}║${NC} 日誌路徑: ${GRAY}$LOG_DIR${NC}                                   ${WHITE}║${NC}"
    echo -e "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo
    
    # 統計摘要
    echo -e "${WHITE}╔══════════════════════════════════ 📊 統計摘要 ═══════════════════════════════════╗${NC}"
    printf "${WHITE}║${NC} 下載總數: ${CYAN}%-6d${NC} │ 成功: ${GREEN}%-6d${NC} │ 失敗: ${RED}%-6d${NC} │ 成功率: ${YELLOW}%3d%%${WHITE} ║${NC}\n" \
        "$TOTAL_DOWNLOADS" "$TOTAL_SUCCESS" "$TOTAL_FAILURES" "$success_rate"
    printf "${WHITE}║${NC} 節點總數: ${CYAN}%-6d${NC} │ 優秀: ${GREEN}%-6d${NC} │ 良好: ${BLUE}%-6d${NC} │ 封鎖: ${RED}%3d${WHITE}  ║${NC}\n" \
        "$total_nodes" "$EXCELLENT_NODES" "$GOOD_NODES" "$BLACKLISTED_NODES"
    echo -e "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo
    
    # 節點詳細信息
    echo -e "${WHITE}╔══════════════════════════════════ 🔗 節點詳細信息 ═══════════════════════════════════╗${NC}"
    printf "${WHITE}║${NC} %-24s │ %-12s │ %-8s │ %-8s │ %-8s ${WHITE}║${NC}\n" \
        "節點ID" "狀態" "評分" "成功率%" "嘗試數"
    echo -e "${WHITE}╠══════════════════════════════════════════════════════════════════════════════════╣${NC}"
    
    # 按狀態排序顯示節點
    for status in "EXCELLENT" "GOOD" "SUCCESS" "NEW" "FAILED" "BLACKLISTED"; do
        for node_id in "${!NODE_STATUS[@]}"; do
            if [[ "${NODE_STATUS[$node_id]}" == "$status" ]]; then
                local short_id=$(shorten_node_id "$node_id")
                local score="${NODE_SCORE[$node_id]:-N/A}"
                local success_rate="${NODE_SUCCESS_RATE[$node_id]:-N/A}"
                local attempts="${NODE_ATTEMPTS[$node_id]:-N/A}"
                
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
                
                printf "${WHITE}║${NC} %-24s │ ${status_color}%-12s${NC} │ %-8s │ %-8s │ %-8s ${WHITE}║${NC}\n" \
                    "$short_id" "$status" "$score" "$success_rate" "$attempts"
            fi
        done
    done
    
    echo -e "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo
    
    # 建議
    echo -e "${WHITE}╔══════════════════════════════════ 💡 優化建議 ═══════════════════════════════════╗${NC}"
    
    if [[ $EXCELLENT_NODES -gt 0 ]]; then
        echo -e "${WHITE}║${GREEN} ✅ 發現 $EXCELLENT_NODES 個優秀節點！系統應該優先使用這些節點             ${WHITE}║${NC}"
    else
        echo -e "${WHITE}║${YELLOW} ⚠️  沒有發現優秀節點，建議等待系統發現穩定的高性能節點         ${WHITE}║${NC}"
    fi
    
    if [[ $BLACKLISTED_NODES -gt $((total_nodes / 2)) ]]; then
        echo -e "${WHITE}║${RED} 🚫 封鎖節點過多 ($BLACKLISTED_NODES/$total_nodes)，可能需要調整節點選擇策略      ${WHITE}║${NC}"
    fi
    
    if [[ $success_rate -lt 50 ]]; then
        echo -e "${WHITE}║${RED} 📉 成功率過低 ($success_rate%)，建議檢查網絡連接和節點質量             ${WHITE}║${NC}"
    elif [[ $success_rate -gt 80 ]]; then
        echo -e "${WHITE}║${GREEN} 📈 成功率良好 ($success_rate%)，優化策略正在生效！                   ${WHITE}║${NC}"
    fi
    
    echo -e "${WHITE}╚═══════════════════════════════════════════════════════════════════════════════════╝${NC}"
}

# 主函數
main() {
    echo -e "${CYAN}🔍 TON 節點性能日誌分析器${NC}"
    echo -e "${GRAY}=================================${NC}"
    echo
    
    if [[ $# -gt 0 ]]; then
        echo -e "${YELLOW}使用自定義日誌目錄: $1${NC}"
    else
        echo -e "${YELLOW}使用默認日誌目錄: $LOG_DIR${NC}"
    fi
    
    # 檢查權限
    if [[ ! -r "$LOG_DIR" ]]; then
        echo -e "${RED}錯誤：無法讀取日誌目錄 $LOG_DIR${NC}"
        echo -e "${YELLOW}請確保您有適當的權限，或使用 sudo 運行此腳本${NC}"
        exit 1
    fi
    
    analyze_logs
    show_results
    
    echo
    echo -e "${CYAN}分析完成！如需實時監控，請使用: ./node-performance-dashboard.sh${NC}"
}

# 使用說明
if [[ "${1:-}" == "--help" ]] || [[ "${1:-}" == "-h" ]]; then
    echo "用法: $0 [日誌目錄]"
    echo
    echo "參數:"
    echo "  日誌目錄    可選，指定TON日誌文件所在目錄 (默認: /var/ton-work)"
    echo
    echo "示例:"
    echo "  $0                     # 分析默認目錄"
    echo "  $0 /path/to/logs      # 分析指定目錄"
    echo "  $0 --help             # 顯示此幫助信息"
    exit 0
fi

# 運行主函數
main "$@" 