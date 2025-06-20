/*
    This file is part of TON Blockchain Library - Optimized Version
    
    Enhanced archive slice download with smart node selection and parallel downloading
*/
#include "download-archive-slice-optimized.hpp"
#include "td/utils/port/path.h"
#include "td/utils/overloaded.h"
#include "td/utils/Random.h"
#include <algorithm>

#include <ton/ton-tl.hpp>

namespace ton {

namespace validator {

namespace fullnode {

// Static member initialization
std::map<adnl::AdnlNodeIdShort, NodeQuality> DownloadArchiveSliceOptimized::node_qualities_;

DownloadArchiveSliceOptimized::DownloadArchiveSliceOptimized(
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
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise)) {
  
  // If specific node provided, add it to candidates
  if (!download_from.is_zero()) {
    candidate_nodes_.push_back(download_from);
  }
}

void DownloadArchiveSliceOptimized::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "🚫 Failed to download archive slice #" << masterchain_seqno_ 
                 << " for shard " << shard_prefix_.to_str() << ": " << reason;
    promise_.set_error(std::move(reason));
    if (!fd_.empty()) {
      td::unlink(tmp_name_).ensure();
      fd_.close();
    }
  }
  cleanup_active_attempts();
  stop();
}

void DownloadArchiveSliceOptimized::alarm() {
  if (active_attempts_.empty()) {
    abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
  } else {
    // Try next strategy if current one times out
    cleanup_active_attempts();
    try_next_strategy();
  }
}

void DownloadArchiveSliceOptimized::finish_query() {
  if (promise_) {
    auto elapsed = download_start_timer_.elapsed();
    auto speed = offset_ > 0 ? (double(offset_) / elapsed) : 0.0;
    
    LOG(INFO) << "✅ Successfully downloaded archive slice #" << masterchain_seqno_ 
              << " " << shard_prefix_.to_str() << ": " << td::format::as_size(offset_)
              << " in " << elapsed << "s (" << td::format::as_size(td::uint64(speed)) << "/s)";
              
    if (!successful_node_.is_zero()) {
      handle_node_success(successful_node_, speed);
    }
    
    promise_.set_value(std::move(tmp_name_));
    fd_.close();
  }
  cleanup_active_attempts();
  stop();
}

void DownloadArchiveSliceOptimized::start_up() {
  alarm_timestamp() = timeout_;
  download_start_timer_ = td::Timer();

  auto R = td::mkstemp(tmp_dir_);
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to open temp file: "));
    return;
  }
  auto r = R.move_as_ok();
  fd_ = std::move(r.first);
  tmp_name_ = std::move(r.second);

  LOG(INFO) << "📦 Starting optimized download of archive slice #" << masterchain_seqno_ 
            << " " << shard_prefix_.to_str();

  // Get multiple candidate nodes for better success rate
  if (candidate_nodes_.empty() && client_.empty()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadArchiveSliceOptimized::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &DownloadArchiveSliceOptimized::got_candidate_nodes, R.move_as_ok());
      }
    });

    // Request more nodes for better selection
    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 8,
                            std::move(P));
  } else {
    // Start with known nodes or client
    got_candidate_nodes(candidate_nodes_);
  }
}

void DownloadArchiveSliceOptimized::got_candidate_nodes(std::vector<adnl::AdnlNodeIdShort> nodes) {
  if (!client_.empty()) {
    // When using client, proceed directly
    candidate_nodes_ = nodes;
    try_next_strategy();
    return;
  }

  if (nodes.empty()) {
    abort_query(td::Status::Error(ErrorCode::notready, "no nodes available"));
    return;
  }

  candidate_nodes_ = select_best_nodes(nodes, std::min<td::uint32>(nodes.size(), 6));
  
  LOG(INFO) << "🔍 Selected " << candidate_nodes_.size() << " candidate nodes from " << nodes.size() 
            << " available nodes";
  
  try_next_strategy();
}

void DownloadArchiveSliceOptimized::try_next_strategy() {
  cleanup_active_attempts();
  
  if (strategy_attempt_ >= 4) {  // Max 4 strategies
    abort_query(td::Status::Error(ErrorCode::timeout, "exhausted all download strategies"));
    return;
  }
  
  strategy_attempt_++;
  retry_count_ = 0;
  
  // Set strategy deadline
  strategy_deadline_ = get_dynamic_timeout(strategy_attempt_);
  alarm_timestamp() = strategy_deadline_;
  
  switch (strategy_attempt_) {
    case 1:
      LOG(INFO) << "🎯 Strategy 1: Parallel download from best 3 nodes";
      attempt_parallel_download();
      break;
    case 2:
      LOG(INFO) << "🎯 Strategy 2: Sequential high-quality nodes";
      if (!candidate_nodes_.empty()) {
        attempt_single_download(candidate_nodes_[0]);
      } else {
        try_next_strategy();
      }
      break;
    case 3:
      LOG(INFO) << "🎯 Strategy 3: Try remaining nodes";
      for (auto& node : candidate_nodes_) {
        if (failed_nodes_.find(node) == failed_nodes_.end()) {
          attempt_single_download(node);
          return;
        }
      }
      try_next_strategy();
      break;
    case 4:
      LOG(INFO) << "🎯 Strategy 4: Desperate attempt - try all nodes";
      if (!candidate_nodes_.empty()) {
        auto node = candidate_nodes_[td::Random::fast(0, candidate_nodes_.size() - 1)];
        attempt_single_download(node);
      } else {
        try_next_strategy();
      }
      break;
  }
}

void DownloadArchiveSliceOptimized::attempt_parallel_download() {
  auto best_nodes = select_best_nodes(candidate_nodes_, 3);
  
  if (best_nodes.empty()) {
    try_next_strategy();
    return;
  }
  
  for (auto& node : best_nodes) {
    if (failed_nodes_.find(node) == failed_nodes_.end()) {
      active_attempts_.insert(node);
      
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), node](td::Result<td::BufferSlice> R) {
        td::actor::send_closure(SelfId, &DownloadArchiveSliceOptimized::got_archive_info, node, std::move(R));
      });

      td::BufferSlice q;
      if (shard_prefix_.is_masterchain()) {
        q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
      } else {
        q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                             create_tl_shard_id(shard_prefix_));
      }
      
      // Shorter timeout for parallel attempts
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query, node, local_id_, overlay_id_,
                              "get_archive_info", std::move(P), td::Timestamp::in(2.0), std::move(q));
    }
  }
  
  if (active_attempts_.empty()) {
    try_next_strategy();
  }
}

void DownloadArchiveSliceOptimized::attempt_single_download(adnl::AdnlNodeIdShort node) {
  if (failed_nodes_.find(node) != failed_nodes_.end()) {
    try_next_strategy();
    return;
  }
  
  active_attempts_.insert(node);
  
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), node](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DownloadArchiveSliceOptimized::got_archive_info, node, std::move(R));
  });

  td::BufferSlice q;
  if (shard_prefix_.is_masterchain()) {
    q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
  } else {
    q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                         create_tl_shard_id(shard_prefix_));
  }
  
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, node, local_id_, overlay_id_,
                            "get_archive_info", std::move(P), td::Timestamp::in(5.0), std::move(q));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_info",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(2.0), std::move(P));
  }
}

void DownloadArchiveSliceOptimized::got_archive_info(adnl::AdnlNodeIdShort node, td::Result<td::BufferSlice> result) {
  active_attempts_.erase(node);
  
  if (result.is_error()) {
    handle_node_failure(node, result.move_as_error());
    
    if (active_attempts_.empty()) {
      try_next_strategy();
    }
    return;
  }
  
  auto data = result.move_as_ok();
  auto F = fetch_tl_object<ton_api::tonNode_ArchiveInfo>(std::move(data), true);
  if (F.is_error()) {
    handle_node_failure(node, F.move_as_error_prefix("failed to parse ArchiveInfo"));
    
    if (active_attempts_.empty()) {
      try_next_strategy();
    }
    return;
  }
  
  auto f = F.move_as_ok();
  bool fail = false;
  
  ton_api::downcast_call(*f.get(), td::overloaded(
                                       [&](const ton_api::tonNode_archiveNotFound &obj) {
                                         handle_node_failure(node, td::Status::Error(ErrorCode::notready, "remote db not found"));
                                         fail = true;
                                       },
                                       [&](const ton_api::tonNode_archiveInfo &obj) { 
                                         archive_id_ = obj.id_;
                                         archive_id_found_ = true;
                                       }));
  
  if (fail) {
    if (active_attempts_.empty()) {
      try_next_strategy();
    }
    return;
  }
  
  // Success! Cancel other attempts and start downloading
  successful_node_ = node;
  cleanup_active_attempts();
  
  LOG(INFO) << "📦 Found archive info from " << node << ", starting download";
  prev_logged_timer_ = td::Timer();
  get_archive_slice_from_node(node);
}

void DownloadArchiveSliceOptimized::get_archive_slice_from_node(adnl::AdnlNodeIdShort node) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), node](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DownloadArchiveSliceOptimized::got_archive_slice, node, std::move(R));
  });

  auto q = create_serialize_tl_object<ton_api::tonNode_getArchiveSlice>(archive_id_, offset_, slice_size());
  
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, node, local_id_, overlay_id_,
                            "get_archive_slice", std::move(P), td::Timestamp::in(20.0), std::move(q),
                            slice_size() + 1024, rldp_);
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_slice",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(20.0), std::move(P));
  }
}

void DownloadArchiveSliceOptimized::got_archive_slice(adnl::AdnlNodeIdShort node, td::Result<td::BufferSlice> result) {
  if (result.is_error()) {
    handle_node_failure(node, result.move_as_error());
    try_next_strategy();
    return;
  }
  
  auto data = result.move_as_ok();
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
  log_download_progress();

  if (data.size() < slice_size()) {
    finish_query();
  } else {
    // Continue downloading from the same successful node
    get_archive_slice_from_node(node);
  }
}

void DownloadArchiveSliceOptimized::handle_node_success(adnl::AdnlNodeIdShort node, double speed) {
  auto& quality = node_qualities_[node];
  quality.success_count++;
  quality.last_success = td::Timestamp::now();
  
  // Update average speed with exponential moving average
  if (quality.avg_speed == 0.0) {
    quality.avg_speed = speed;
  } else {
    quality.avg_speed = 0.7 * quality.avg_speed + 0.3 * speed;
  }
  
  LOG(INFO) << "✅ Node " << node << " success: speed=" << td::format::as_size(td::uint64(speed)) 
            << "/s, score=" << quality.get_score();
}

void DownloadArchiveSliceOptimized::handle_node_failure(adnl::AdnlNodeIdShort node, td::Status error) {
  failed_nodes_.insert(node);
  
  auto& quality = node_qualities_[node];
  quality.failure_count++;
  quality.last_failure = td::Timestamp::now();
  
  LOG(WARNING) << "❌ Node " << node << " failed: " << error 
               << ", score=" << quality.get_score() 
               << ", blacklisted=" << quality.is_blacklisted();
}

std::vector<adnl::AdnlNodeIdShort> DownloadArchiveSliceOptimized::select_best_nodes(
    const std::vector<adnl::AdnlNodeIdShort>& nodes, td::uint32 count) {
  
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> scored_nodes;
  
  for (auto& node : nodes) {
    auto it = node_qualities_.find(node);
    double score;
    
    if (it == node_qualities_.end()) {
      score = 0.5;  // Neutral score for unknown nodes
    } else {
      if (it->second.is_blacklisted()) {
        continue;  // Skip blacklisted nodes
      }
      score = it->second.get_score();
    }
    
    scored_nodes.emplace_back(score, node);
  }
  
  // Sort by score (descending)
  std::sort(scored_nodes.begin(), scored_nodes.end(), 
            [](const auto& a, const auto& b) { return a.first > b.first; });
  
  std::vector<adnl::AdnlNodeIdShort> result;
  for (td::uint32 i = 0; i < std::min(count, td::uint32(scored_nodes.size())); i++) {
    result.push_back(scored_nodes[i].second);
  }
  
  return result;
}

td::Timestamp DownloadArchiveSliceOptimized::get_dynamic_timeout(td::uint32 strategy) {
  double base_timeout;
  
  switch (strategy) {
    case 1: base_timeout = 15.0; break;  // Parallel attempts
    case 2: base_timeout = 25.0; break;  // Sequential high-quality
    case 3: base_timeout = 35.0; break;  // Remaining nodes
    case 4: base_timeout = 45.0; break;  // Desperate attempt
    default: base_timeout = 60.0; break;
  }
  
  // Adjust based on retry count
  double adjusted_timeout = base_timeout * (1.0 + retry_count_ * 0.5);
  
  return td::Timestamp::in(adjusted_timeout);
}

void DownloadArchiveSliceOptimized::cleanup_active_attempts() {
  active_attempts_.clear();
}

void DownloadArchiveSliceOptimized::log_download_progress() {
  double elapsed = prev_logged_timer_.elapsed();
  if (elapsed > 5.0 || offset_ == 0) {  // Log every 5 seconds
    prev_logged_timer_ = td::Timer();
    auto speed = (offset_ - prev_logged_sum_) / elapsed;
    
    LOG(INFO) << "⬇️  Downloading archive slice #" << masterchain_seqno_ 
              << " " << shard_prefix_.to_str() << ": " << td::format::as_size(offset_)
              << " (" << td::format::as_size(td::uint64(speed)) << "/s)";
    
    prev_logged_sum_ = offset_;
  }
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton 