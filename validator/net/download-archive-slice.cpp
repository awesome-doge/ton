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
  td::Timestamp last_success;
  td::Timestamp last_failure;
  double avg_speed = 0.0;
  
  double get_score() const {
    if (success_count + failure_count == 0) return 0.5;
    double success_rate = double(success_count) / (success_count + failure_count);
    double time_penalty = (td::Timestamp::now().at() - last_failure.at()) < 600.0 ? 0.3 : 0.0;
    return std::max(0.0, success_rate - time_penalty);
  }
  
  bool is_blacklisted() const {
    return failure_count >= 3 && (td::Timestamp::now().at() - last_failure.at()) < 900.0;  // 15min blacklist
  }
};

// Static node quality tracking (shared across instances)
static std::map<adnl::AdnlNodeIdShort, NodeQuality> node_qualities_;
static std::set<adnl::AdnlNodeIdShort> active_attempts_;
static td::uint32 strategy_attempt_ = 0;

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
    
    // Update node quality on success
    if (!download_from_.is_zero()) {
      auto& quality = node_qualities_[download_from_];
      quality.success_count++;
      quality.last_success = td::Timestamp::now();
      LOG(INFO) << "✅ Node " << download_from_ << " success, score=" << quality.get_score();
    }
    
    promise_.set_value(std::move(tmp_name_));
    fd_.close();
  }
  active_attempts_.erase(download_from_);
  stop();
}

// Helper function to select best nodes
std::vector<adnl::AdnlNodeIdShort> select_best_nodes(const std::vector<adnl::AdnlNodeIdShort>& nodes, td::uint32 count) {
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
  for (td::uint32 i = 0; i < std::min(count, static_cast<td::uint32>(scored_nodes.size())); i++) {
    result.push_back(scored_nodes[i].second);
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

  LOG(INFO) << "📦 Starting optimized download of archive slice #" << masterchain_seqno_ 
            << " " << shard_prefix_.to_str();

  if (download_from_.is_zero() && client_.empty()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          // **OPTIMIZATION: Select best nodes instead of random**
          auto best_nodes = select_best_nodes(vec, std::min(static_cast<td::uint32>(vec.size()), static_cast<td::uint32>(3)));
          if (!best_nodes.empty()) {
            LOG(INFO) << "🔍 Selected best node from " << vec.size() << " candidates";
            td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, best_nodes[0]);
          } else {
            // Fallback to random if all nodes are blacklisted
            td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, vec[0]);
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

  // **OPTIMIZATION: Check if node is blacklisted**
  auto it = node_qualities_.find(download_from);
  if (it != node_qualities_.end() && it->second.is_blacklisted()) {
    LOG(WARNING) << "❌ Node " << download_from << " is blacklisted, aborting";
    abort_query(td::Status::Error(ErrorCode::notready, "node blacklisted"));
    return;
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
    // **OPTIMIZATION: Track node failure**
    auto& quality = node_qualities_[download_from_];
    quality.failure_count++;
    quality.last_failure = td::Timestamp::now();
    LOG(WARNING) << "❌ Node " << download_from_ << " failed to parse ArchiveInfo, score=" << quality.get_score();
    
    abort_query(F.move_as_error_prefix("failed to parse ArchiveInfo answer"));
    return;
  }
  auto f = F.move_as_ok();

  bool fail = false;
  ton_api::downcast_call(*f.get(), td::overloaded(
                                       [&](const ton_api::tonNode_archiveNotFound &obj) {
                                         // **OPTIMIZATION: Track node failure**
                                         auto& quality = node_qualities_[download_from_];
                                         quality.failure_count++;
                                         quality.last_failure = td::Timestamp::now();
                                         LOG(WARNING) << "❌ Node " << download_from_ << " archive not found, score=" << quality.get_score();
                                         
                                         abort_query(td::Status::Error(ErrorCode::notready, "remote db not found"));
                                         fail = true;
                                       },
                                       [&](const ton_api::tonNode_archiveInfo &obj) { archive_id_ = obj.id_; }));
  if (fail) {
    return;
  }

  prev_logged_timer_ = td::Timer();
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

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
