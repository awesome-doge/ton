/*
    Enhanced Archive Download Strategy for TON Blockchain
    This file implements more aggressive optimization to reduce download failures
*/

#include "download-archive-slice.hpp"
#include "td/utils/port/path.h"
#include "td/utils/overloaded.h"
#include "td/utils/Random.h"
#include <algorithm>
#include <map>
#include <set>

#include <ton/ton-tl.hpp>

namespace ton {
namespace validator {
namespace fullnode {

// Enhanced Node Quality with more aggressive filtering
struct EnhancedNodeQuality {
  td::uint32 success_count = 0;
  td::uint32 failure_count = 0;
  td::uint32 archive_not_found_count = 0;
  td::uint32 timeout_count = 0;
  td::Timestamp last_success;
  td::Timestamp last_failure;
  td::Timestamp first_seen;
  double avg_speed = 0.0;
  double best_speed = 0.0;
  td::uint32 consecutive_failures = 0;
  
  td::uint32 total_attempts() const { return success_count + failure_count; }
  
  double success_rate() const {
    if (total_attempts() == 0) return 0.0;
    return double(success_count) / total_attempts();
  }
  
  bool is_reliable() const {
    // Stricter criteria for reliable nodes
    return success_count >= 3 && success_rate() >= 0.85 && consecutive_failures <= 1;
  }
  
  bool is_promising() const {
    // For newer nodes that show potential
    return total_attempts() <= 5 && success_rate() >= 0.6 && consecutive_failures <= 2;
  }
  
  double get_enhanced_score() const {
    if (total_attempts() == 0) return 0.1;  // Very low for unknown nodes
    
    // Base success rate
    double base_score = success_rate();
    
    // Heavy penalty for consecutive failures
    double failure_penalty = consecutive_failures * 0.2;
    
    // Bonus for reliable nodes
    double reliability_bonus = is_reliable() ? 0.3 : 0.0;
    
    // Speed bonus (prioritize faster nodes more aggressively)
    double speed_bonus = 0.0;
    if (best_speed > 1000000.0) {  // 1MB/s+
      speed_bonus = std::min(0.2, best_speed / 5000000.0);  // Up to 0.2 for 5MB/s+
    }
    
    // Recency bonus (recently successful nodes get preference)
    double recency_bonus = 0.0;
    if (last_success.is_in_past(300.0)) {  // Success within 5 minutes
      recency_bonus = 0.1;
    }
    
    // Archive availability penalty
    double availability_penalty = 0.0;
    if (archive_not_found_count > success_count) {
      availability_penalty = 0.3;  // Strong penalty for nodes with poor data availability
    }
    
    return std::max(0.0, std::min(1.0, 
      base_score + reliability_bonus + speed_bonus + recency_bonus - failure_penalty - availability_penalty));
  }
  
  bool should_blacklist() const {
    // More aggressive blacklisting
    if (consecutive_failures >= 2) return true;  // Blacklist after 2 consecutive failures
    if (failure_count >= 3 && success_rate() < 0.3) return true;  // Low overall success rate
    if (timeout_count >= 2 && total_attempts() <= 5) return true;  // Too many timeouts for new nodes
    if (archive_not_found_count >= 3 && success_count == 0) return true;  // No data available
    return false;
  }
  
  td::uint32 blacklist_duration() const {
    // Dynamic blacklist duration based on failure type
    if (consecutive_failures >= 3) return 3600;  // 1 hour for severe failures
    if (timeout_count > failure_count / 2) return 1800;  // 30 min for timeout issues
    if (archive_not_found_count > failure_count * 0.8) return 900;  // 15 min for data availability
    return 600;  // 10 min default
  }
};

// Enhanced Download Strategy Implementation
class EnhancedArchiveDownloader {
public:
  static constexpr td::uint32 MAX_PARALLEL_ATTEMPTS = 5;
  static constexpr td::uint32 MAX_STRATEGY_RETRIES = 6;
  static constexpr double INITIAL_TIMEOUT = 3.0;
  static constexpr double DATA_TIMEOUT = 30.0;
  
  enum class DownloadStrategy {
    RELIABLE_NODES_ONLY = 1,      // Try only proven reliable nodes
    PARALLEL_BEST_NODES = 2,      // Parallel from top nodes
    MIXED_APPROACH = 3,           // Mix of reliable + promising
    PROMISING_NODES = 4,          // Try newer promising nodes
    BROAD_SEARCH = 5,             // Cast wider net
    DESPERATE_ATTEMPT = 6         // Try anything available
  };
  
  static std::vector<adnl::AdnlNodeIdShort> select_nodes_for_strategy(
      DownloadStrategy strategy,
      const std::vector<adnl::AdnlNodeIdShort>& available_nodes,
      const std::map<adnl::AdnlNodeIdShort, EnhancedNodeQuality>& qualities) {
    
    std::vector<std::pair<double, adnl::AdnlNodeIdShort>> scored_nodes;
    std::vector<adnl::AdnlNodeIdShort> reliable_nodes;
    std::vector<adnl::AdnlNodeIdShort> promising_nodes;
    std::vector<adnl::AdnlNodeIdShort> unknown_nodes;
    
    // Categorize nodes
    for (const auto& node : available_nodes) {
      auto it = qualities.find(node);
      
      if (it == qualities.end()) {
        unknown_nodes.push_back(node);
        continue;
      }
      
      const auto& quality = it->second;
      
      // Skip blacklisted nodes
      if (quality.should_blacklist() && 
          !quality.last_failure.is_in_past(quality.blacklist_duration())) {
        continue;
      }
      
      double score = quality.get_enhanced_score();
      scored_nodes.emplace_back(score, node);
      
      if (quality.is_reliable()) {
        reliable_nodes.push_back(node);
      } else if (quality.is_promising()) {
        promising_nodes.push_back(node);
      }
    }
    
    // Sort by score
    std::sort(scored_nodes.begin(), scored_nodes.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    std::vector<adnl::AdnlNodeIdShort> result;
    
    switch (strategy) {
      case DownloadStrategy::RELIABLE_NODES_ONLY:
        // Only use proven reliable nodes
        result = reliable_nodes;
        if (result.size() > 3) result.resize(3);
        LOG(INFO) << "🎯 Strategy 1: Using " << result.size() << " reliable nodes only";
        break;
        
      case DownloadStrategy::PARALLEL_BEST_NODES:
        // Top 4 nodes in parallel
        for (size_t i = 0; i < std::min(size_t(4), scored_nodes.size()); i++) {
          result.push_back(scored_nodes[i].second);
        }
        LOG(INFO) << "🎯 Strategy 2: Parallel download from " << result.size() << " best nodes";
        break;
        
      case DownloadStrategy::MIXED_APPROACH:
        // Mix reliable + promising
        result = reliable_nodes;
        if (result.size() < 3) {
          for (const auto& node : promising_nodes) {
            if (result.size() >= 3) break;
            result.push_back(node);
          }
        }
        if (result.size() > 3) result.resize(3);
        LOG(INFO) << "🎯 Strategy 3: Mixed approach with " << result.size() << " nodes";
        break;
        
      case DownloadStrategy::PROMISING_NODES:
        // Focus on promising new nodes
        result = promising_nodes;
        if (result.size() > 3) result.resize(3);
        LOG(INFO) << "🎯 Strategy 4: Trying " << result.size() << " promising nodes";
        break;
        
      case DownloadStrategy::BROAD_SEARCH:
        // Top 6 nodes regardless of category
        for (size_t i = 0; i < std::min(size_t(6), scored_nodes.size()); i++) {
          result.push_back(scored_nodes[i].second);
        }
        // Add some unknown nodes for exploration
        for (size_t i = 0; i < std::min(size_t(2), unknown_nodes.size()); i++) {
          result.push_back(unknown_nodes[i]);
        }
        LOG(INFO) << "🎯 Strategy 5: Broad search with " << result.size() << " nodes";
        break;
        
      case DownloadStrategy::DESPERATE_ATTEMPT:
        // Try everything that's not severely blacklisted
        for (const auto& [score, node] : scored_nodes) {
          if (score > 0.05) {  // Very low threshold
            result.push_back(node);
          }
        }
        // Even try some unknown nodes
        for (size_t i = 0; i < std::min(size_t(3), unknown_nodes.size()); i++) {
          result.push_back(unknown_nodes[i]);
        }
        LOG(WARNING) << "🎯 Strategy 6: Desperate attempt with " << result.size() << " nodes";
        break;
    }
    
    return result;
  }
  
  static double get_timeout_for_strategy(DownloadStrategy strategy, td::uint32 retry_count) {
    double base_timeout;
    
    switch (strategy) {
      case DownloadStrategy::RELIABLE_NODES_ONLY:
        base_timeout = 10.0;  // Short timeout for reliable nodes
        break;
      case DownloadStrategy::PARALLEL_BEST_NODES:
        base_timeout = 8.0;   // Very short for parallel
        break;
      case DownloadStrategy::MIXED_APPROACH:
        base_timeout = 15.0;
        break;
      case DownloadStrategy::PROMISING_NODES:
        base_timeout = 12.0;
        break;
      case DownloadStrategy::BROAD_SEARCH:
        base_timeout = 20.0;
        break;
      case DownloadStrategy::DESPERATE_ATTEMPT:
        base_timeout = 25.0;
        break;
    }
    
    // Increase timeout with retries
    return base_timeout * (1.0 + retry_count * 0.3);
  }
  
  static bool should_attempt_parallel(DownloadStrategy strategy) {
    return strategy == DownloadStrategy::PARALLEL_BEST_NODES || 
           strategy == DownloadStrategy::BROAD_SEARCH;
  }
};

// Enhanced failure tracking with block-level statistics
struct BlockDownloadStats {
  td::uint32 total_attempts = 0;
  td::uint32 not_found_count = 0;
  td::uint32 timeout_count = 0;
  td::uint32 success_count = 0;
  td::Timestamp first_attempt;
  td::Timestamp last_attempt;
  std::set<adnl::AdnlNodeIdShort> failed_nodes;
  
  double success_probability() const {
    if (total_attempts == 0) return 0.5;
    return double(success_count) / total_attempts;
  }
  
  bool seems_unavailable() const {
    // Block seems unavailable if most attempts result in "not found"
    return total_attempts >= 5 && 
           not_found_count > total_attempts * 0.8 &&
           success_count == 0;
  }
  
  bool has_connectivity_issues() const {
    // High timeout rate suggests connectivity problems
    return total_attempts >= 3 && 
           timeout_count > total_attempts * 0.6;
  }
  
  td::uint32 recommended_delay() const {
    if (seems_unavailable()) return 300;  // 5 min delay for unavailable blocks
    if (has_connectivity_issues()) return 60;  // 1 min for connectivity issues
    return 0;  // No delay
  }
};

// Global tracking maps
static std::map<adnl::AdnlNodeIdShort, EnhancedNodeQuality> enhanced_node_qualities_;
static std::map<BlockSeqno, BlockDownloadStats> block_download_stats_;

// Enhanced logging and monitoring
class DownloadMonitor {
public:
  static void log_strategy_start(EnhancedArchiveDownloader::DownloadStrategy strategy, 
                                 BlockSeqno seqno, 
                                 const std::vector<adnl::AdnlNodeIdShort>& nodes) {
    LOG(INFO) << "🚀 ENHANCED STRATEGY " << static_cast<int>(strategy) 
              << " for block #" << seqno 
              << " with " << nodes.size() << " nodes";
  }
  
  static void log_node_attempt(adnl::AdnlNodeIdShort node, BlockSeqno seqno) {
    auto it = enhanced_node_qualities_.find(node);
    if (it != enhanced_node_qualities_.end()) {
      const auto& quality = it->second;
      LOG(INFO) << "🔍 Trying node " << node 
                << " | Score: " << quality.get_enhanced_score()
                << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                << " | Attempts: " << quality.total_attempts()
                << " | Type: " << (quality.is_reliable() ? "RELIABLE" : 
                                   quality.is_promising() ? "PROMISING" : "NORMAL");
    } else {
      LOG(INFO) << "🆕 Trying unknown node " << node << " for block #" << seqno;
    }
  }
  
  static void log_node_success(adnl::AdnlNodeIdShort node, double speed, BlockSeqno seqno) {
    auto& quality = enhanced_node_qualities_[node];
    quality.success_count++;
    quality.consecutive_failures = 0;
    quality.last_success = td::Timestamp::now();
    
    if (speed > 0) {
      quality.avg_speed = quality.success_count == 1 ? speed : 
                         (quality.avg_speed * 0.7 + speed * 0.3);
      quality.best_speed = std::max(quality.best_speed, speed);
    }
    
    auto& block_stats = block_download_stats_[seqno];
    block_stats.success_count++;
    
    LOG(INFO) << "✅ SUCCESS " << node 
              << " | Score: " << quality.get_enhanced_score()
              << " | Success Rate: " << (quality.success_rate() * 100) << "%"
              << " | Speed: " << td::format::as_size(static_cast<td::uint64>(speed)) << "/s"
              << " | Best: " << td::format::as_size(static_cast<td::uint64>(quality.best_speed)) << "/s";
  }
  
  static void log_node_failure(adnl::AdnlNodeIdShort node, const td::Status& error, BlockSeqno seqno) {
    auto& quality = enhanced_node_qualities_[node];
    quality.failure_count++;
    quality.consecutive_failures++;
    quality.last_failure = td::Timestamp::now();
    
    // Track specific failure types
    std::string error_str = error.message().str();
    if (error_str.find("not found") != std::string::npos) {
      quality.archive_not_found_count++;
    } else if (error_str.find("timeout") != std::string::npos) {
      quality.timeout_count++;
    }
    
    auto& block_stats = block_download_stats_[seqno];
    block_stats.total_attempts++;
    block_stats.failed_nodes.insert(node);
    
    if (error_str.find("not found") != std::string::npos) {
      block_stats.not_found_count++;
    } else if (error_str.find("timeout") != std::string::npos) {
      block_stats.timeout_count++;
    }
    
    bool should_blacklist = quality.should_blacklist();
    
    LOG(WARNING) << "❌ FAILURE " << node 
                 << " | Error: " << error_str
                 << " | Score: " << quality.get_enhanced_score()
                 << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                 << " | Consecutive Failures: " << quality.consecutive_failures
                 << (should_blacklist ? " | BLACKLISTED" : "");
  }
  
  static void log_block_stats(BlockSeqno seqno) {
    auto& stats = block_download_stats_[seqno];
    if (stats.total_attempts > 0) {
      LOG(INFO) << "📊 Block #" << seqno << " stats"
                << " | Attempts: " << stats.total_attempts
                << " | Success Rate: " << (stats.success_probability() * 100) << "%"
                << " | NotFound: " << stats.not_found_count
                << " | Timeouts: " << stats.timeout_count
                << " | Failed Nodes: " << stats.failed_nodes.size()
                << (stats.seems_unavailable() ? " | SEEMS UNAVAILABLE" : "")
                << (stats.has_connectivity_issues() ? " | CONNECTIVITY ISSUES" : "");
    }
  }
};

}  // namespace fullnode
}  // namespace validator
}  // namespace ton
 