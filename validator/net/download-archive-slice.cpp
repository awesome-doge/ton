/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
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

// Node quality tracking structure
struct NodeQuality {
  td::uint32 success_count = 0;
  td::uint32 failure_count = 0;
  td::uint32 archive_not_found_count = 0;
  td::Timestamp last_success;
  td::Timestamp last_failure;
  td::Timestamp first_seen;
  double avg_speed = 0.0;
  double total_download_time = 0.0;
  
  // **NEW: Advanced metrics for explore-exploit strategy**
  td::uint32 total_attempts() const { return success_count + failure_count; }
  
  double success_rate() const {
    if (total_attempts() == 0) return 0.0;
    return double(success_count) / total_attempts();
  }
  
  double confidence_interval() const {
    if (total_attempts() == 0) return 1.0;  // High uncertainty for new nodes
    // Upper Confidence Bound calculation
    double exploration_factor = std::sqrt(2.0 * std::log(100.0) / total_attempts());
    return std::min(1.0, success_rate() + exploration_factor);
  }
  
  bool is_new_node() const {
    return total_attempts() < 3;  // Node with less than 3 attempts is considered "new"
  }
  
  double get_score() const {
    if (total_attempts() == 0) return 0.3;  // **REDUCED: Lower score for completely unknown nodes**
    
    double base_score = success_rate();
    
    // **REDUCED EXPLORATION BONUS: Less aggressive exploration**
    double exploration_bonus = 0.0;
    if (is_new_node()) {
      exploration_bonus = 0.1;  // **REDUCED: Smaller bonus for new nodes**
    } else if (total_attempts() < 10) {
      exploration_bonus = 0.05;  // **REDUCED: Smaller bonus for lightly tested nodes**
    }
    
    // **ENHANCED TIME PENALTY: Stronger penalty for recent failures**
    double time_penalty = 0.0;
    if (failure_count > 0 && (td::Timestamp::now().at() - last_failure.at()) < 600.0) {
      time_penalty = 0.4;  // **INCREASED: Stronger penalty for recent failures**
      // **ENHANCED: Still penalize "archive not found" but less**
      if (archive_not_found_count > failure_count * 0.8) {
        time_penalty *= 0.7;  // **INCREASED: Less reduction for archive not found**
      }
    }
    
    // **SPEED BONUS: Faster nodes get slight preference**
    double speed_bonus = 0.0;
    if (success_count > 0 && avg_speed > 0) {
      speed_bonus = std::min(0.1, avg_speed / 10000000.0);  // Up to 0.1 bonus for 10MB/s+
    }
    
    return std::max(0.0, std::min(1.0, base_score + exploration_bonus - time_penalty + speed_bonus));
  }
  
  bool is_blacklisted() const {
    // **MORE AGGRESSIVE: Blacklist failing nodes faster**
    if (failure_count < 3) return false;  // **REDUCED: Need only 3 failures instead of 5**
    if (success_count * 2 > failure_count) return false;  // **STRICTER: Don't blacklist if success rate > 50% (was 25%)**
    
    double blacklist_time = 1800.0;  // **INCREASED: Default 30 minutes (was 15)** 
    if (archive_not_found_count > failure_count * 0.7) {
      blacklist_time = 900.0;  // **INCREASED: 15 minutes for data availability issues (was 5)**
    }
    
    return (td::Timestamp::now().at() - last_failure.at()) < blacklist_time;
  }
};

// Static node quality tracking (shared across instances)
static std::map<adnl::AdnlNodeIdShort, NodeQuality> node_qualities_;
static std::set<adnl::AdnlNodeIdShort> active_attempts_;
static td::uint32 strategy_attempt_ = 0;

// **ENHANCED: Block-level data availability tracking**
struct BlockAvailability {
  td::uint32 not_found_count = 0;
  td::uint32 total_attempts = 0;
  td::Timestamp first_attempt;
  td::Timestamp last_not_found;
  
  bool is_likely_unavailable() const {
    return false;  // **DISABLED: Never consider blocks unavailable to avoid delays**
  }
  
  td::uint32 recommended_delay() const {
    return 0;  // **DISABLED: Never delay download attempts**
  }
};

static std::map<BlockSeqno, BlockAvailability> block_availability_;

DownloadArchiveSlice::DownloadArchiveSlice(
    BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir, adnl::AdnlNodeIdShort local_id,
    overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from, td::Timestamp timeout,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager, td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<std::string> promise)
    : masterchain_seqno_(masterchain_seqno)
    , shard_prefix_(shard_prefix)
    , tmp_dir_(std::move(tmp_dir))
    , local_id_(local_id)
    , overlay_id_(overlay_id)
    , download_from_(download_from)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise)) {
}

void DownloadArchiveSlice::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "🚫 Failed to download archive slice #" << masterchain_seqno_ 
                 << " for shard " << shard_prefix_.to_str() << ": " << reason;
    promise_.set_error(std::move(reason));
    if (!fd_.empty()) {
      td::unlink(tmp_name_).ensure();
      fd_.close();
    }
  }
  active_attempts_.erase(download_from_);
  stop();
}

void DownloadArchiveSlice::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadArchiveSlice::finish_query() {
  if (promise_) {
    LOG(INFO) << "✅ Successfully downloaded archive slice #" << masterchain_seqno_ 
              << " " << shard_prefix_.to_str() << ": " << td::format::as_size(offset_);
    
    // **ENHANCED: Update node quality with detailed statistics**
    if (!download_from_.is_zero()) {
      auto& quality = node_qualities_[download_from_];
      quality.success_count++;
      quality.last_success = td::Timestamp::now();
      
      // **NEW: Calculate and update download speed**
      double download_time = prev_logged_timer_.elapsed() > 0 ? prev_logged_timer_.elapsed() : 1.0;
      double current_speed = static_cast<double>(offset_) / download_time;  // bytes per second
      
      if (quality.success_count == 1) {
        quality.avg_speed = current_speed;
        quality.total_download_time = download_time;
      } else {
        // Update running average speed
        quality.total_download_time += download_time;
        quality.avg_speed = (quality.avg_speed * (quality.success_count - 1) + current_speed) / quality.success_count;
      }
      
      LOG(INFO) << "✅ Node " << download_from_ << " SUCCESS"
                << " | Score: " << quality.get_score()
                << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                << " | Attempts: " << quality.total_attempts()
                << " | Speed: " << td::format::as_size(static_cast<td::uint64>(current_speed)) << "/s"
                << " | Avg Speed: " << td::format::as_size(static_cast<td::uint64>(quality.avg_speed)) << "/s";
    }
    
    promise_.set_value(std::move(tmp_name_));
    fd_.close();
  }
  active_attempts_.erase(download_from_);
  stop();
}

// Helper function to select best nodes with explore-exploit strategy
std::vector<adnl::AdnlNodeIdShort> select_best_nodes(const std::vector<adnl::AdnlNodeIdShort>& nodes, td::uint32 count) {
  if (nodes.empty()) return {};
  
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> all_nodes;
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> high_quality_nodes;  // **NEW: Track high-quality nodes**
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> medium_nodes;
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> new_nodes;
  
  td::uint32 new_count = 0;
  td::uint32 experienced_count = 0;
  td::uint32 blacklisted_count = 0;
  td::uint32 high_quality_count = 0;
  
  for (auto& node : nodes) {
    auto it = node_qualities_.find(node);
    
    if (it == node_qualities_.end()) {
      // **REDUCED EXPLORATION: Lower score for new unknown nodes**
      double new_node_score = 0.2;  // **REDUCED: Much lower score for new nodes**
      all_nodes.emplace_back(new_node_score, node);
      new_nodes.emplace_back(new_node_score, node);
      new_count++;
      
      // Initialize tracking for new node
      auto& quality = node_qualities_[node];
      if (quality.first_seen.at() == 0.0) {
        quality.first_seen = td::Timestamp::now();
        LOG(INFO) << "🆕 Discovered new node " << node;
      }
      
    } else {
      if (it->second.is_blacklisted()) {
        blacklisted_count++;
        LOG(INFO) << "🚫 Skipping blacklisted node " << node 
                  << " (failures: " << it->second.failure_count << ")";
        continue;  // Skip blacklisted nodes
      }
      
      double score = it->second.get_score();
      
      // **STRICTER FILTERING: Skip more low quality nodes**
      if (score < 0.2 && it->second.total_attempts() >= 2) {  // **INCREASED: Stricter threshold**
        blacklisted_count++;
        LOG(WARNING) << "🚫 Filtering low-quality node " << node 
                     << " | Score: " << score
                     << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                     << " | Attempts: " << it->second.total_attempts();
        continue;  // Skip very low quality nodes
      }
      
      all_nodes.emplace_back(score, node);
      
      // **ENHANCED: More strict categorization for better quality control**
      if (it->second.success_rate() >= 0.8 && it->second.total_attempts() >= 3) {  // **STRICTER: Higher requirements**
        high_quality_nodes.emplace_back(score, node);
        high_quality_count++;
        LOG(INFO) << "⭐ High-quality node found: " << node 
                  << " (score=" << score << ", success_rate=" << (it->second.success_rate() * 100) << "%)";
      } else if (it->second.is_new_node() || (score >= 0.4 && it->second.success_rate() >= 0.5)) {  // **STRICTER: Higher thresholds**
        // Only medium nodes if they have decent success OR are new
        medium_nodes.emplace_back(score, node);
        LOG(INFO) << "🔶 Medium-quality node: " << node 
                  << " (score=" << score << ", success_rate=" << (it->second.success_rate() * 100) << "%)";
      } else {
        // **NEW: Log nodes that are being filtered out**
        LOG(INFO) << "🔻 Low-quality node available but deprioritized: " << node 
                  << " (score=" << score << ", success_rate=" << (it->second.success_rate() * 100) << "%)";
      }
      
      if (it->second.is_new_node()) {
        new_count++;
      } else {
        experienced_count++;
      }
    }
  }
  
  // **PRIORITIZED SELECTION STRATEGY**
  std::vector<adnl::AdnlNodeIdShort> result;
  td::uint32 selected_count = std::min(count, static_cast<td::uint32>(all_nodes.size()));
  
  if (all_nodes.empty()) {
    LOG(WARNING) << "❌ No available nodes (blacklisted: " << blacklisted_count << ")";
    return result;
  }
  
  LOG(INFO) << "🎯 SELECTION ANALYSIS - Total: " << nodes.size() 
            << " | High-Quality: " << high_quality_count 
            << " | Medium: " << medium_nodes.size()
            << " | New: " << new_count 
            << " | Blacklisted: " << blacklisted_count;
  
  // **STRATEGY 1: PRIORITIZE HIGH-QUALITY NODES**
  if (!high_quality_nodes.empty()) {
    // Sort high-quality nodes by score
    std::sort(high_quality_nodes.begin(), high_quality_nodes.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // **INCREASED: Select at least 80% from high-quality nodes (was 60%)**
    td::uint32 high_quality_slots = std::max(1u, static_cast<td::uint32>(selected_count * 0.8));
    high_quality_slots = std::min(high_quality_slots, static_cast<td::uint32>(high_quality_nodes.size()));
    
    for (td::uint32 i = 0; i < high_quality_slots; i++) {
      result.push_back(high_quality_nodes[i].second);
      auto it = node_qualities_.find(high_quality_nodes[i].second);
      LOG(INFO) << "✅ PRIORITY SELECT: " << high_quality_nodes[i].second 
                << " | Score: " << high_quality_nodes[i].first
                << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                << " | Attempts: " << it->second.total_attempts();
    }
    
    selected_count -= high_quality_slots;
  }
  
  // **STRATEGY 2: PRIORITIZE MEDIUM NODES OVER NEW NODES**
  if (selected_count > 0) {
    std::vector<std::pair<double, adnl::AdnlNodeIdShort>> remaining_candidates;
    
    // **CHANGED: Prioritize medium nodes first, then new nodes**
    remaining_candidates.insert(remaining_candidates.end(), medium_nodes.begin(), medium_nodes.end());
    
    // **LIMIT NEW NODE EXPLORATION: Only add a few new nodes**
    td::uint32 max_new_nodes = std::min(static_cast<td::uint32>(2), selected_count);  // **LIMIT: Max 2 new nodes**
    td::uint32 added_new_nodes = 0;
    for (auto& new_node : new_nodes) {
      if (added_new_nodes >= max_new_nodes) break;
      remaining_candidates.push_back(new_node);
      added_new_nodes++;
    }
    
    // Sort remaining candidates by score
    std::sort(remaining_candidates.begin(), remaining_candidates.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    for (td::uint32 i = 0; i < std::min(selected_count, static_cast<td::uint32>(remaining_candidates.size())); i++) {
      result.push_back(remaining_candidates[i].second);
      
      auto it = node_qualities_.find(remaining_candidates[i].second);
      if (it != node_qualities_.end()) {
        LOG(INFO) << "🔍 MEDIUM SELECT: " << remaining_candidates[i].second 
                  << " | Score: " << remaining_candidates[i].first
                  << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                  << " | Attempts: " << it->second.total_attempts();
      } else {
        LOG(INFO) << "🆕 LIMITED NEW NODE SELECT: " << remaining_candidates[i].second 
                  << " | Score: " << remaining_candidates[i].first;
      }
    }
  }
  
  // **FALLBACK: If no nodes selected, pick the best available**
  if (result.empty() && !all_nodes.empty()) {
    std::sort(all_nodes.begin(), all_nodes.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // **STRICTER: Higher minimum score requirement**
    std::vector<std::pair<double, adnl::AdnlNodeIdShort>> acceptable_fallback;
    for (auto& node_pair : all_nodes) {
      if (node_pair.first >= 0.3) {  // **INCREASED: At least 0.3 minimal score (was 0.1)**
        acceptable_fallback.push_back(node_pair);
      }
    }
    
    if (!acceptable_fallback.empty()) {
      result.push_back(acceptable_fallback[0].second);
      LOG(WARNING) << "⚠️ FALLBACK SELECT (acceptable): " << acceptable_fallback[0].second 
                   << " | Score: " << acceptable_fallback[0].first;
    } else {
      // **REMOVED: No more "last resort" selection of worst nodes**
      LOG(ERROR) << "🚫 NO ACCEPTABLE NODES FOUND! All nodes have score < 0.3"
                 << " | Best available score: " << (all_nodes.empty() ? 0.0 : all_nodes[0].first)
                 << " | Refusing to use low-quality nodes";
      // Don't select any node - let the system request new nodes instead
    }
  }
  
  if (result.empty()) {
    LOG(ERROR) << "💥 NO NODES SELECTED! This should not happen!";
  }
  
  return result;
}

void DownloadArchiveSlice::start_up() {
  alarm_timestamp() = timeout_;

  auto R = td::mkstemp(tmp_dir_);
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to open temp file: "));
    return;
  }
  auto r = R.move_as_ok();
  fd_ = std::move(r.first);
  tmp_name_ = std::move(r.second);

  // **NEW: Check block-level data availability**
  auto& block_avail = block_availability_[masterchain_seqno_];
  if (block_avail.first_attempt.at() == 0.0) {
    block_avail.first_attempt = td::Timestamp::now();
  }
  
  // **DISABLED: Skip delay logic completely**
  // if (block_avail.is_likely_unavailable()) {
  //   td::uint32 delay = block_avail.recommended_delay();
  //   LOG(WARNING) << "⏳ Block #" << masterchain_seqno_ << " likely unavailable"
  //                << " | NotFound: " << block_avail.not_found_count 
  //                << "/" << block_avail.total_attempts 
  //                << " | Delaying " << delay << "s";
  //   
  //   // Delay download attempt
  //   alarm_timestamp() = td::Timestamp::in(static_cast<double>(delay));
  //   return;
  // }

  LOG(INFO) << "📦 Starting optimized download of archive slice #" << masterchain_seqno_ 
            << " " << shard_prefix_.to_str();

  if (download_from_.is_zero() && client_.empty()) {
    // **NEW: First try to use known high-quality nodes**
    std::vector<adnl::AdnlNodeIdShort> known_good_nodes;
    for (auto& pair : node_qualities_) {
      if (!pair.second.is_blacklisted() && 
          pair.second.success_rate() >= 0.7 && 
          pair.second.total_attempts() >= 2) {
        known_good_nodes.push_back(pair.first);
      }
    }
    
    if (!known_good_nodes.empty()) {
      // **CHANGED: 90% use known good nodes, 10% explore new nodes (was 80%/20%)**
      bool use_known_node = (td::Random::fast(1, 100) <= 90);  // **INCREASED: 90% probability**
      
      if (use_known_node) {
        // Sort by score and pick the best
        std::sort(known_good_nodes.begin(), known_good_nodes.end(), 
                  [](const adnl::AdnlNodeIdShort& a, const adnl::AdnlNodeIdShort& b) {
                    auto it_a = node_qualities_.find(a);
                    auto it_b = node_qualities_.find(b);
                    return it_a->second.get_score() > it_b->second.get_score();
                  });
        
        // **ENHANCED: Add some randomness among top nodes to avoid overusing single node**
        td::uint32 top_nodes_count = std::min(3u, static_cast<td::uint32>(known_good_nodes.size()));
        td::uint32 selected_idx = td::Random::fast(0, static_cast<td::int32>(top_nodes_count - 1));
        
        auto best_known = known_good_nodes[selected_idx];
        auto it = node_qualities_.find(best_known);
        LOG(INFO) << "🏆 PRIORITIZING known high-quality node: " << best_known
                  << " | Score: " << it->second.get_score()
                  << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                  << " | Attempts: " << it->second.total_attempts()
                  << " | Rank: " << (selected_idx + 1) << "/" << known_good_nodes.size();
        
        got_node_to_download(best_known);
        return;
      } else {
        LOG(INFO) << "🎲 EXPLORATION MODE: Skipping " << known_good_nodes.size() 
                  << " known good nodes to explore new options (10% chance)";
      }
    } else {
      LOG(INFO) << "🔍 No known high-quality nodes available, requesting from overlay...";
    }

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          // **ENHANCED: Smart explore-exploit node selection**
          LOG(INFO) << "🔍 Starting node selection from " << vec.size() << " candidates";
          auto best_nodes = select_best_nodes(vec, std::min(static_cast<td::uint32>(vec.size()), static_cast<td::uint32>(3)));
          
          if (!best_nodes.empty()) {
            LOG(INFO) << "🎯 Smart selection completed from " << vec.size() << " candidates, chose: " << best_nodes[0];
            td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, best_nodes[0]);
          } else {
            // **NEW: If all nodes are blacklisted, request more nodes**
            LOG(WARNING) << "⚠️ All initial nodes blacklisted or filtered, requesting more candidates...";
            // Request more nodes when initial selection fails
            auto P2 = td::PromiseCreator::lambda([SelfId](td::Result<std::vector<adnl::AdnlNodeIdShort>> R2) {
              if (R2.is_error()) {
                td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R2.move_as_error());
              } else {
                auto vec2 = R2.move_as_ok();
                if (!vec2.empty()) {
                  LOG(INFO) << "🔄 Fallback to any available node from " << vec2.size() << " candidates";
                  td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, vec2[0]);
                } else {
                  td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query,
                                          td::Status::Error(ErrorCode::notready, "no fallback nodes"));
                }
              }
            });
            // Request more nodes as fallback - call directly, not via actor
            request_more_nodes(std::move(P2));
            return;
          }
        }
      }
    });

    // **OPTIMIZATION: Request more nodes for better selection**
    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 6,
                            std::move(P));
  } else {
    got_node_to_download(download_from_);
  }
}

void DownloadArchiveSlice::got_node_to_download(adnl::AdnlNodeIdShort download_from) {
  download_from_ = download_from;
  active_attempts_.insert(download_from);

  // **ENHANCED: Check if node is blacklisted with detailed info**
  auto it = node_qualities_.find(download_from);
  if (it != node_qualities_.end() && it->second.is_blacklisted()) {
    LOG(WARNING) << "❌ Node " << download_from << " is BLACKLISTED"
                 << " | Score: " << it->second.get_score()
                 << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                 << " | Attempts: " << it->second.total_attempts()
                 << " | Recent Failures: " << it->second.failure_count;
    abort_query(td::Status::Error(ErrorCode::notready, "node blacklisted"));
    return;
  }

  // **NEW: Log node selection details**
  if (it != node_qualities_.end()) {
    LOG(INFO) << "🚀 Using node " << download_from 
              << " | Score: " << it->second.get_score()
              << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
              << " | Attempts: " << it->second.total_attempts()
              << " | Type: " << (it->second.is_new_node() ? "NEW" : "EXPERIENCED");
  } else {
    LOG(INFO) << "🆕 Using completely unknown node " << download_from;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_info, R.move_as_ok());
    }
  });

  td::BufferSlice q;
  if (shard_prefix_.is_masterchain()) {
    q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
  } else {
    q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                         create_tl_shard_id(shard_prefix_));
  }
  if (client_.empty()) {
    // **OPTIMIZATION: Shorter timeout for faster failure detection**
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_archive_info", std::move(P), td::Timestamp::in(2.0), std::move(q));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_info",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadArchiveSlice::got_archive_info(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_ArchiveInfo>(std::move(data), true);
  if (F.is_error()) {
    // **ENHANCED: Track node failure with detailed statistics**
    auto& quality = node_qualities_[download_from_];
    quality.failure_count++;
    quality.last_failure = td::Timestamp::now();
    
    LOG(WARNING) << "❌ Node " << download_from_ << " FAILED to parse ArchiveInfo"
                 << " | Score: " << quality.get_score()
                 << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                 << " | Attempts: " << quality.total_attempts();
    
    abort_query(F.move_as_error_prefix("failed to parse ArchiveInfo answer"));
    return;
  }
  auto f = F.move_as_ok();

  bool fail = false;
  ton_api::downcast_call(*f.get(), td::overloaded(
                                       [&](const ton_api::tonNode_archiveNotFound &obj) {
                                         // **ENHANCED: Track specific failure type**
                                         auto& quality = node_qualities_[download_from_];
                                         quality.failure_count++;
                                         quality.last_failure = td::Timestamp::now();
                                         quality.archive_not_found_count++;
                                         
                                         // **NEW: Update block-level availability tracking**
                                         auto& block_avail = block_availability_[masterchain_seqno_];
                                         block_avail.total_attempts++;
                                         block_avail.not_found_count++;
                                         block_avail.last_not_found = td::Timestamp::now();
                                         
                                         LOG(WARNING) << "❌ Node " << download_from_ << " ARCHIVE NOT FOUND"
                                                      << " | Score: " << quality.get_score()
                                                      << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                                                      << " | Attempts: " << quality.total_attempts()
                                                      << " | NotFound: " << quality.archive_not_found_count
                                                      << " | Block NotFound Rate: " << (double(block_avail.not_found_count) / block_avail.total_attempts * 100) << "%";
                                         
                                         abort_query(td::Status::Error(ErrorCode::notready, "remote db not found"));
                                         fail = true;
                                       },
                                       [&](const ton_api::tonNode_archiveInfo &obj) { 
                                         archive_id_ = obj.id_;
                                         
                                         // **NEW: Update block-level availability tracking for success**
                                         auto& block_avail = block_availability_[masterchain_seqno_];
                                         block_avail.total_attempts++;
                                       }));
  if (fail) {
    return;
  }

  // **NEW: Record download start time for speed calculation**
  prev_logged_timer_ = td::Timer();  // Reset timer at start of actual download
  LOG(INFO) << "📦 Found archive info from " << download_from_ << ", starting download";
  get_archive_slice();
}

void DownloadArchiveSlice::get_archive_slice() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_slice, R.move_as_ok());
    }
  });

  auto q = create_serialize_tl_object<ton_api::tonNode_getArchiveSlice>(archive_id_, offset_, slice_size());
  if (client_.empty()) {
    // **OPTIMIZATION: Longer timeout for actual data transfer**
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_, overlay_id_,
                            "get_archive_slice", std::move(P), td::Timestamp::in(25.0), std::move(q),
                            slice_size() + 1024, rldp_);
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_slice",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(20.0), std::move(P));
  }
}

void DownloadArchiveSlice::got_archive_slice(td::BufferSlice data) {
  auto R = fd_.write(data.as_slice());
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to write temp file: "));
    return;
  }
  if (R.move_as_ok() != data.size()) {
    abort_query(td::Status::Error(ErrorCode::error, "short write to temp file"));
    return;
  }

  offset_ += data.size();

  // **OPTIMIZATION: Enhanced progress logging**
  double elapsed = prev_logged_timer_.elapsed();
  if (elapsed > 3.0) {  // Log every 3 seconds
    prev_logged_timer_ = td::Timer();
    auto speed = static_cast<double>(offset_ - prev_logged_sum_) / elapsed;
    LOG(INFO) << "⬇️  Downloading archive slice #" << masterchain_seqno_ 
              << " " << shard_prefix_.to_str() << ": " << td::format::as_size(offset_)
              << " (" << td::format::as_size(static_cast<td::uint64>(speed)) << "/s)";
    prev_logged_sum_ = offset_;
  }

  if (data.size() < slice_size()) {
    finish_query();
  } else {
    get_archive_slice();
  }
}

// **NEW: Method to request more nodes when initial selection fails**
void DownloadArchiveSlice::request_more_nodes(td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {
  LOG(INFO) << "🔄 Requesting additional nodes due to blacklist situation";
  
  // Request double the usual amount for better chances
  td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 12,
                          std::move(promise));
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
