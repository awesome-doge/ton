/*
    This file is part of TON Blockchain Library - Optimized Version
    
    Enhanced archive slice download with smart node selection and parallel downloading
*/
#pragma once

#include "overlay/overlays.h"
#include "ton/ton-types.h"
#include "validator/validator.h"
#include "adnl/adnl-ext-client.h"
#include "td/utils/port/FileFd.h"
#include <map>
#include <set>
#include <vector>

namespace ton {

namespace validator {

namespace fullnode {

// Node quality tracking structure
struct NodeQuality {
  td::uint32 success_count = 0;
  td::uint32 failure_count = 0;
  td::Timestamp last_success;
  td::Timestamp last_failure;
  double avg_speed = 0.0;  // bytes per second
  
  double get_score() const {
    if (success_count + failure_count == 0) return 0.5;
    double success_rate = double(success_count) / (success_count + failure_count);
    double time_penalty = last_failure.is_in_past(600.0) ? 0.0 : 0.3;  // Recent failures penalty
    return std::max(0.0, success_rate - time_penalty);
  }
  
  bool is_blacklisted() const {
    return failure_count >= 5 && !last_failure.is_in_past(1800.0);  // 30min blacklist
  }
};

class DownloadArchiveSliceOptimized : public td::actor::Actor {
 public:
  DownloadArchiveSliceOptimized(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                               adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                               adnl::AdnlNodeIdShort download_from, td::Timestamp timeout,
                               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                               td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
                               td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<adnl::Adnl> adnl,
                               td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<std::string> promise);

  void abort_query(td::Status reason);
  void alarm() override;
  void finish_query();

  void start_up() override;
  void got_candidate_nodes(std::vector<adnl::AdnlNodeIdShort> nodes);
  void try_next_strategy();
  void attempt_parallel_download();
  void attempt_single_download(adnl::AdnlNodeIdShort node);
  void got_archive_info(adnl::AdnlNodeIdShort node, td::Result<td::BufferSlice> result);
  void get_archive_slice_from_node(adnl::AdnlNodeIdShort node);
  void got_archive_slice(adnl::AdnlNodeIdShort node, td::Result<td::BufferSlice> result);
  void handle_node_success(adnl::AdnlNodeIdShort node, double speed);
  void handle_node_failure(adnl::AdnlNodeIdShort node, td::Status error);

  static constexpr td::uint32 slice_size() {
    return 1 << 21;
  }

 private:
  // Core parameters
  BlockSeqno masterchain_seqno_;
  ShardIdFull shard_prefix_;
  std::string tmp_dir_;
  std::string tmp_name_;
  td::FileFd fd_;
  adnl::AdnlNodeIdShort local_id_;
  overlay::OverlayIdShort overlay_id_;
  
  // Download state
  td::uint64 offset_ = 0;
  td::uint64 archive_id_ = 0;
  bool archive_id_found_ = false;
  
  // Strategy and timing
  td::uint32 strategy_attempt_ = 0;
  td::uint32 retry_count_ = 0;
  td::Timestamp timeout_;
  td::Timestamp strategy_deadline_;
  
  // Node management
  std::vector<adnl::AdnlNodeIdShort> candidate_nodes_;
  std::set<adnl::AdnlNodeIdShort> active_attempts_;
  std::set<adnl::AdnlNodeIdShort> failed_nodes_;
  adnl::AdnlNodeIdShort successful_node_ = adnl::AdnlNodeIdShort::zero();
  
  // Static node quality tracking (shared across instances)
  static std::map<adnl::AdnlNodeIdShort, NodeQuality> node_qualities_;
  
  // Actor references
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlSenderInterface> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::Promise<std::string> promise_;

  // Logging
  td::uint64 prev_logged_sum_ = 0;
  td::Timer prev_logged_timer_;
  td::Timer download_start_timer_;
  
  // Helper methods
  std::vector<adnl::AdnlNodeIdShort> select_best_nodes(const std::vector<adnl::AdnlNodeIdShort>& nodes, td::uint32 count);
  td::Timestamp get_dynamic_timeout(td::uint32 strategy);
  void cleanup_active_attempts();
  void log_download_progress();
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton 