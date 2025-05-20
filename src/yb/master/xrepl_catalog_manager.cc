// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/cdc/cdc_service.h"
#include "yb/cdc/cdc_state_table.h"
#include "yb/cdc/xcluster_util.h"

#include "yb/client/meta_cache.h"
#include "yb/client/schema.h"
#include "yb/client/table.h"
#include "yb/client/table_handle.h"
#include "yb/client/table_info.h"

#include "yb/common/colocated_util.h"
#include "yb/common/common_flags.h"
#include "yb/common/pg_system_attr.h"
#include "yb/common/schema_pbutil.h"

#include "yb/docdb/docdb_pgapi.h"

#include "yb/gutil/bind.h"
#include "yb/gutil/bind_helpers.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager-internal.h"
#include "yb/master/catalog_manager.h"
#include "yb/util/is_operation_done_result.h"
#include "yb/master/xcluster/xcluster_manager.h"
#include "yb/master/xcluster/xcluster_replication_group.h"
#include "yb/master/xcluster/master_xcluster_util.h"
#include "yb/master/xcluster_consumer_registry_service.h"
#include "yb/master/xcluster_rpc_tasks.h"
#include "yb/master/master.h"
#include "yb/master/master_ddl.pb.h"
#include "yb/master/master_heartbeat.pb.h"
#include "yb/master/master_replication.pb.h"
#include "yb/master/master_util.h"
#include "yb/master/snapshot_transfer_manager.h"
#include "yb/master/ysql_tablegroup_manager.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/debug-util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/string_util.h"
#include "yb/util/sync_point.h"
#include "yb/util/thread.h"
#include "yb/util/trace.h"

#include "yb/yql/pggate/ybc_pg_typedefs.h"

using std::string;
using namespace std::literals;
using std::vector;

DEFINE_RUNTIME_uint32(cdc_wal_retention_time_secs, 4 * 3600,
    "WAL retention time in seconds to be used for tables for which a CDC stream was created.");

DEFINE_RUNTIME_bool(check_bootstrap_required, false,
    "Is it necessary to check whether bootstrap is required for Universe Replication.");

DEFINE_test_flag(bool, disable_cdc_state_insert_on_setup, false,
    "Disable inserting new entries into cdc state as part of the setup flow.");

DEFINE_RUNTIME_int32(cdcsdk_table_processing_limit_per_run, 2,
    "The number of newly added tables we will add to CDCSDK streams, per run of the background "
    "task.");

DEFINE_RUNTIME_bool(xcluster_skip_schema_compatibility_checks_on_alter, false,
    "When xCluster replication sends a DDL change, skip checks "
    "for any schema compatibility");

DEFINE_RUNTIME_int32(wait_replication_drain_retry_timeout_ms, 2000,
    "Timeout in milliseconds in between CheckReplicationDrain calls to tservers "
    "in case of retries.");

DEFINE_RUNTIME_int32(ns_replication_sync_retry_secs, 5,
    "Frequency at which the bg task will try to sync with producer and add tables to "
    "the current NS-level replication, when there are non-replicated consumer tables.");

DEFINE_RUNTIME_int32(ns_replication_sync_backoff_secs, 60,
    "Frequency of the add table task for a NS-level replication, when there are no "
    "non-replicated consumer tables.");

DEFINE_RUNTIME_int32(ns_replication_sync_error_backoff_secs, 300,
    "Frequency of the add table task for a NS-level replication, when there are too "
    "many consecutive errors happening for the replication.");

DEFINE_RUNTIME_bool(disable_universe_gc, false, "Whether to run the GC on universes or not.");

DEFINE_RUNTIME_int32(cdc_parent_tablet_deletion_task_retry_secs, 30,
    "Frequency at which the background task will verify parent tablets retained for xCluster or "
    "CDCSDK replication and determine if they can be cleaned up.");

DEFINE_NON_RUNTIME_uint32(max_replication_slots, 10,
    "Controls the maximum number of replication slots that are allowed to exist.");

DEFINE_test_flag(bool, hang_wait_replication_drain, false,
    "Used in tests to temporarily block WaitForReplicationDrain.");

DEFINE_test_flag(bool, exit_unfinished_deleting, false,
    "Whether to exit part way through the deleting universe process.");

DEFINE_test_flag(bool, exit_unfinished_merging, false,
    "Whether to exit part way through the merging universe process.");

DEFINE_test_flag(
    bool, xcluster_fail_create_consumer_snapshot, false,
    "In the SetupReplicationWithBootstrap flow, test failure to create snapshot on consumer.");

DEFINE_test_flag(
    bool, xcluster_fail_restore_consumer_snapshot, false,
    "In the SetupReplicationWithBootstrap flow, test failure to restore snapshot on consumer.");

DEFINE_test_flag(
    bool, allow_ycql_transactional_xcluster, false,
    "Determines if xCluster transactional replication on YCQL tables is allowed.");

DEFINE_RUNTIME_AUTO_bool(cdc_enable_postgres_replica_identity, kLocalPersisted, false, true,
    "Enable new record types in CDC streams");

DEFINE_RUNTIME_bool(enable_backfilling_cdc_stream_with_replication_slot, false,
    "When enabled, allows adding a replication slot name to an existing CDC stream via the yb-admin"
    " ysql_backfill_change_data_stream_with_replication_slot command."
    "Intended to be used for making CDC streams created before replication slot support work with"
    " the replication slot commands.");

DEFINE_test_flag(bool, fail_universe_replication_merge, false, "Causes MergeUniverseReplication to "
    "fail with an error.");

DEFINE_test_flag(bool, xcluster_fail_setup_stream_update, false, "Fail UpdateCDCStream RPC call");

DEFINE_RUNTIME_AUTO_bool(cdcsdk_enable_dynamic_tables_disable_option,
                         kLocalPersisted,
                         false,
                         true,
                         "This flag needs to be true in order to disable addition of dynamic tables"
                         " to CDC stream. This flag is required to be to true for execution of "
                         "yb-admin commands - "
                         "\'disable_dynamic_table_addition_on_change_data_stream\', "
                         "\'remove_user_table_from_change_data_stream\'");
TAG_FLAG(cdcsdk_enable_dynamic_tables_disable_option, advanced);
TAG_FLAG(cdcsdk_enable_dynamic_tables_disable_option, hidden);

DEFINE_test_flag(bool, cdcsdk_skip_updating_cdc_state_entries_on_table_removal, false,
    "Skip updating checkpoint to max for cdc state table entries while removing a user table from "
    "CDCSDK stream.");

DEFINE_test_flag(bool, cdcsdk_add_indexes_to_stream, false, "Allows addition of index to a stream");

DEPRECATE_FLAG(bool, cdcsdk_enable_cleanup_of_non_eligible_tables_from_stream, "09_2024");

DEFINE_test_flag(bool, cdcsdk_disable_drop_table_cleanup, false,
                 "When enabled, cleanup of dropped tables from CDC streams will be skipped.");

DEFINE_test_flag(bool, cdcsdk_disable_deleted_stream_cleanup, false,
                 "When enabled, cleanup of deleted CDCSDK streams will be skipped.");

DEFINE_RUNTIME_AUTO_bool(cdcsdk_enable_identification_of_non_eligible_tables,
                         kLocalPersisted,
                         false,
                         true,
                         "This flag, when true, identifies all non-eligible tables that are part of"
                         " a CDC stream metadata while loading the CDC streams on a master "
                         "restart/leadership change. This identification happens on all CDC "
                         "streams in the universe");
TAG_FLAG(cdcsdk_enable_identification_of_non_eligible_tables, advanced);
TAG_FLAG(cdcsdk_enable_identification_of_non_eligible_tables, hidden);

DEFINE_test_flag(bool, cdcsdk_skip_table_removal_from_qualified_list, false,
                 "When enabled, table would not be removed from the qualified table list as part "
                 "of the table removal process from CDC stream");

DECLARE_bool(xcluster_wait_on_ddl_alter);
DECLARE_int32(master_rpc_timeout_ms);
DECLARE_bool(ysql_yb_enable_replication_commands);
DECLARE_bool(yb_enable_cdc_consistent_snapshot_streams);
DECLARE_bool(enable_xcluster_auto_flag_validation);
DECLARE_bool(ysql_yb_enable_replica_identity);
DECLARE_bool(cdcsdk_enable_dynamic_table_addition_with_table_cleanup);



#define RETURN_ACTION_NOT_OK(expr, action) \
  RETURN_NOT_OK_PREPEND((expr), Format("An error occurred while $0", action))

// assumes the existence of a local variable bootstrap_info
#define MARK_BOOTSTRAP_FAILED_NOT_OK(s) \
  do { \
    auto&& _s = (s); \
    if (PREDICT_FALSE(!_s.ok())) { \
      MarkReplicationBootstrapFailed(bootstrap_info, _s); \
      return; \
    } \
  } while (false)

#define VERIFY_RESULT_MARK_BOOTSTRAP_FAILED(expr) \
  RESULT_CHECKER_HELPER(expr, MARK_BOOTSTRAP_FAILED_NOT_OK(ResultToStatus(__result)))

#define RETURN_INVALID_REQUEST_STATUS(error_msg) \
return STATUS( \
      InvalidArgument, error_msg, \
      MasterError(MasterErrorPB::INVALID_REQUEST))

#define SET_CDCSDK_STREAM_CREATION_STATE(value) \
  if (mode == CreateNewCDCStreamMode::kCdcsdkNamespaceAndTableIds) { \
    cdcsdk_stream_creation_state = value; \
  } \

namespace yb {
using client::internal::RemoteTabletServer;

namespace master {
using TableMetaPB = ImportSnapshotMetaResponsePB::TableMetaPB;

namespace {

template <typename RequestType>
Status ValidateUniverseUUID(const RequestType& req, CatalogManager& catalog_manager) {
  if (req->has_universe_uuid() && !req->universe_uuid().empty()) {
    auto universe_uuid = catalog_manager.GetUniverseUuidIfExists();
    SCHECK(
        universe_uuid && universe_uuid->ToString() == req->universe_uuid(), InvalidArgument,
        "Invalid Universe UUID $0. Expected $1", req->universe_uuid(),
        (universe_uuid ? universe_uuid->ToString() : "empty"));
  }

  return Status::OK();
}

}  // namespace

////////////////////////////////////////////////////////////
// CDC Stream Loader
////////////////////////////////////////////////////////////

class CDCStreamLoader : public Visitor<PersistentCDCStreamInfo> {
 public:
  explicit CDCStreamLoader(CatalogManager* catalog_manager, XClusterManager& xcluster_manager)
      : catalog_manager_(catalog_manager), xcluster_manager_(xcluster_manager) {}

  void AddDefaultValuesIfMissing(const SysCDCStreamEntryPB& metadata, CDCStreamInfo::WriteLock* l) {
    bool source_type_present = false;
    bool checkpoint_type_present = false;

    // Iterate over all the options to check if checkpoint_type and source_type are present.
    for (auto option : metadata.options()) {
      if (option.key() == cdc::kSourceType) {
        source_type_present = true;
      }
      if (option.key() == cdc::kCheckpointType) {
        checkpoint_type_present = true;
      }
    }

    if (!source_type_present) {
      auto source_type_opt = l->mutable_data()->pb.add_options();
      source_type_opt->set_key(cdc::kSourceType);
      source_type_opt->set_value(cdc::CDCRequestSource_Name(cdc::XCLUSTER));
    }

    if (!checkpoint_type_present) {
      auto checkpoint_type_opt = l->mutable_data()->pb.add_options();
      checkpoint_type_opt->set_key(cdc::kCheckpointType);
      checkpoint_type_opt->set_value(cdc::CDCCheckpointType_Name(cdc::IMPLICIT));
    }
  }

  Status Visit(const std::string& stream_id_str, const SysCDCStreamEntryPB& metadata) override
      REQUIRES(catalog_manager_->mutex_) {
    auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(stream_id_str));
    {
      std::lock_guard l(catalog_manager_->xrepl_stream_ids_in_use_mutex_);
      InsertOrDie(&catalog_manager_->xrepl_stream_ids_in_use_, stream_id);
    }
    DCHECK(!ContainsKey(catalog_manager_->cdc_stream_map_, stream_id))
        << "CDC stream already exists: " << stream_id;

    // If CDCStream entry exists, then the current cluster is a producer.
    catalog_manager_->SetCDCServiceEnabled();

    scoped_refptr<NamespaceInfo> ns;
    scoped_refptr<TableInfo> table;

    if (metadata.has_namespace_id()) {
      ns = FindPtrOrNull(catalog_manager_->namespace_ids_map_, metadata.namespace_id());

      if (!ns) {
        LOG(DFATAL) << "Invalid namespace ID " << metadata.namespace_id() << " for stream "
                    << stream_id;
        // TODO (#2059): Potentially signals a race condition that namesapce got deleted
        // while stream was being created.
        // Log error and continue without loading the stream.
        return Status::OK();
      }
    } else {
      table = catalog_manager_->tables_->FindTableOrNull(metadata.table_id(0));
      if (!table) {
        LOG(ERROR) << "Invalid table ID " << metadata.table_id(0) << " for stream " << stream_id;
        // TODO (#2059): Potentially signals a race condition that table got deleted while stream
        //  was being created.
        // Log error and continue without loading the stream.
        return Status::OK();
      }
    }

    // Setup the CDC stream info.
    auto stream = make_scoped_refptr<CDCStreamInfo>(stream_id);
    auto l = stream->LockForWrite();
    l.mutable_data()->pb.CopyFrom(metadata);

    // If no source_type and checkpoint_type is present, that means the stream was created in
    // a previous version where these options were not present.
    AddDefaultValuesIfMissing(metadata, &l);

    // If the table has been deleted, then mark this stream as DELETING so it can be deleted by the
    // catalog manager background thread. Otherwise if this stream is missing an entry
    // for state, then mark its state as Active.

    if (((table && table->LockForRead()->is_deleting()) ||
         (ns && ns->state() == SysNamespaceEntryPB::DELETING)) &&
        !l.data().is_deleting()) {
      l.mutable_data()->pb.set_state(SysCDCStreamEntryPB::DELETING);
    } else if (!l.mutable_data()->pb.has_state()) {
      l.mutable_data()->pb.set_state(SysCDCStreamEntryPB::ACTIVE);
    }

    // Add the CDC stream to the CDC stream map.
    catalog_manager_->cdc_stream_map_[stream->StreamId()] = stream;
    if (table) {
      xcluster_manager_.RecordOutboundStream(stream, metadata.table_id(0));
    }
    if (ns) {
      for (const auto& table_id : metadata.table_id()) {
        catalog_manager_->cdcsdk_tables_to_stream_map_[table_id].insert(stream->StreamId());
      }
      if (metadata.has_cdcsdk_ysql_replication_slot_name()) {
        catalog_manager_->cdcsdk_replication_slots_to_stream_map_.insert_or_assign(
            ReplicationSlotName(metadata.cdcsdk_ysql_replication_slot_name()),
                                stream->StreamId());
      }
    }

    l.Commit();

    // For CDCSDK Streams, we scan all the tables in the namespace, and compare it with all the
    // tables associated with the stream.
    if ((metadata.state() == SysCDCStreamEntryPB::ACTIVE ||
         metadata.state() == SysCDCStreamEntryPB::DELETING_METADATA) &&
        ns && ns->state() == SysNamespaceEntryPB::RUNNING) {
      auto eligible_tables_info = catalog_manager_->FindAllTablesForCDCSDK(metadata.namespace_id());
      catalog_manager_->FindAllTablesMissingInCDCSDKStream(
          stream_id, metadata.table_id(), eligible_tables_info, metadata.unqualified_table_id());

      if (stream->GetCdcsdkYsqlReplicationSlotName().empty()) {
        // Check for any non-eligible tables like indexes, matview etc in CDC stream only if the
        // stream is not associated with a replication slot.
        catalog_manager_->FindAllNonEligibleTablesInCDCSDKStream(
            stream_id, metadata.table_id(), eligible_tables_info);

        // Check for any unprocessed unqualified tables that needs to be removed from CDCSDK
        // streams.
        catalog_manager_->FindAllUnproccesedUnqualifiedTablesInCDCSDKStream(
            stream_id, metadata.table_id(), metadata.unqualified_table_id(), eligible_tables_info);
      }
    }

    LOG(INFO) << "Loaded metadata for CDC stream " << stream->ToString() << ": "
              << metadata.ShortDebugString();

    return Status::OK();
  }

 private:
  CatalogManager* catalog_manager_;
  XClusterManager& xcluster_manager_;

  DISALLOW_COPY_AND_ASSIGN(CDCStreamLoader);
};

void CatalogManager::ClearXReplState() {
  xcluster_auto_flags_revalidation_needed_ = true;

  // Clear CDC stream map.
  xrepl_maps_loaded_ = false;
  {
    std::lock_guard l(xrepl_stream_ids_in_use_mutex_);
    xrepl_stream_ids_in_use_.clear();
  }
  cdc_stream_map_.clear();

  // Clear CDCSDK stream map.
  cdcsdk_tables_to_stream_map_.clear();
  cdcsdk_replication_slots_to_stream_map_.clear();

  // Clear universe replication map.
  universe_replication_map_.clear();
  xcluster_consumer_table_stream_ids_map_.clear();
  {
    std::lock_guard l(xcluster_consumer_replication_error_map_mutex_);
    xcluster_consumer_replication_error_map_.clear();
  }
}

Status CatalogManager::LoadXReplStream() {
  LOG_WITH_FUNC(INFO) << "Loading CDC streams into memory.";
  auto cdc_stream_loader = std::make_unique<CDCStreamLoader>(this, *xcluster_manager_);
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(cdc_stream_loader.get()),
      "Failed while visiting CDC streams in sys catalog");
  xrepl_maps_loaded_ = true;

  // Load retained_by_cdcsdk_ only after loading all CDC streams.
  for (const auto& tablet : hidden_tablets_) {
    TabletDeleteRetainerInfo delete_retainer;
    CDCSDKPopulateDeleteRetainerInfoForTabletDrop(*tablet, delete_retainer);
    RecordCDCSDKHiddenTablets({tablet}, delete_retainer);
  }

  // Refresh the Consumer registry.
  if (cluster_config_) {
    auto l = cluster_config_->LockForRead();
    if (l->pb.has_consumer_registry()) {
      auto& producer_map = l->pb.consumer_registry().producer_map();
      for (const auto& [replication_group_id, _] : producer_map) {
        SyncXClusterConsumerReplicationStatusMap(
            xcluster::ReplicationGroupId(replication_group_id), producer_map);
      }
    }
  }

  return Status::OK();
}

void CatalogManager::RecordCDCSDKHiddenTablets(
    const std::vector<TabletInfoPtr>& tablets, const TabletDeleteRetainerInfo& delete_retainer) {
  if (!delete_retainer.active_cdcsdk) {
    return;
  }

  for (const auto& hidden_tablet : tablets) {
    auto tablet_lock = hidden_tablet->LockForRead();
    auto& tablet_pb = tablet_lock->pb;
    HiddenReplicationParentTabletInfo info{
        .table_id_ = hidden_tablet->table()->id(),
        .parent_tablet_id_ =
            tablet_pb.has_split_parent_tablet_id() ? tablet_pb.split_parent_tablet_id() : "",
        .split_tablets_ = {tablet_pb.split_tablet_ids(0), tablet_pb.split_tablet_ids(1)}};

    retained_by_cdcsdk_.emplace(hidden_tablet->id(), std::move(info));
  }
}

////////////////////////////////////////////////////////////
// Universe Replication Loader
////////////////////////////////////////////////////////////

class UniverseReplicationLoader : public Visitor<PersistentUniverseReplicationInfo> {
 public:
  explicit UniverseReplicationLoader(CatalogManager* catalog_manager)
      : catalog_manager_(catalog_manager) {}

  Status Visit(
      const std::string& replication_group_id_str, const SysUniverseReplicationEntryPB& metadata)
      REQUIRES(catalog_manager_->mutex_) {
    const xcluster::ReplicationGroupId replication_group_id(replication_group_id_str);
    DCHECK(!ContainsKey(
        catalog_manager_->universe_replication_map_,
        xcluster::ReplicationGroupId(replication_group_id)))
        << "Producer universe already exists: " << replication_group_id;

    // Setup the universe replication info.
    scoped_refptr<UniverseReplicationInfo> const ri =
        new UniverseReplicationInfo(replication_group_id);
    {
      auto l = ri->LockForWrite();
      l.mutable_data()->pb.CopyFrom(metadata);

      if (!l->is_active() && !l->is_deleted_or_failed()) {
        // Replication was not fully setup.
        LOG(WARNING) << "Universe replication in transient state: " << replication_group_id;

        // TODO: Should we delete all failed universe replication items?
      }

      // Add universe replication info to the universe replication map.
      catalog_manager_->universe_replication_map_[ri->ReplicationGroupId()] = ri;

      // Add any failed universes to be cleared
      if (l->is_deleted_or_failed() || l->pb.state() == SysUniverseReplicationEntryPB::DELETING ||
          xcluster::IsAlterReplicationGroupId(
              xcluster::ReplicationGroupId(l->pb.replication_group_id()))) {
        catalog_manager_->universes_to_clear_.push_back(ri->ReplicationGroupId());
      }

      // Check if this is a namespace-level replication.
      if (l->pb.has_is_ns_replication() && l->pb.is_ns_replication()) {
        DCHECK(!ContainsKey(catalog_manager_->namespace_replication_map_, replication_group_id))
            << "Duplicated namespace-level replication producer universe:" << replication_group_id;
        catalog_manager_->namespace_replication_enabled_.store(true, std::memory_order_release);

        // Force the consumer to sync with producer immediately.
        auto& metadata =
            catalog_manager_
                ->namespace_replication_map_[replication_group_id];
        metadata.next_add_table_task_time = CoarseMonoClock::Now();
      }

      l.Commit();
        }

    // Also keep track of consumer tables.
    for (const auto& table : metadata.validated_tables()) {
      auto stream_id = FindWithDefault(metadata.table_streams(), table.first, "");
      if (stream_id.empty()) {
        LOG(WARNING) << "Unable to find stream id for table: " << table.first;
        continue;
      }
      catalog_manager_->xcluster_consumer_table_stream_ids_map_[table.second].emplace(
          metadata.replication_group_id(), VERIFY_RESULT(xrepl::StreamId::FromString(stream_id)));
    }

    LOG(INFO) << "Loaded metadata for universe replication " << ri->ToString();
    VLOG(1) << "Metadata for universe replication " << ri->ToString() << ": "
            << metadata.ShortDebugString();

    return Status::OK();
  }

 private:
  CatalogManager* catalog_manager_;

  DISALLOW_COPY_AND_ASSIGN(UniverseReplicationLoader);
};

Status CatalogManager::LoadUniverseReplication() {
  LOG_WITH_FUNC(INFO) << "Loading universe replication info into memory.";
  auto universe_replication_loader = std::make_unique<UniverseReplicationLoader>(this);
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(universe_replication_loader.get()),
      "Failed while visiting universe replication info in sys catalog");

  return Status::OK();
}

////////////////////////////////////////////////////////////
// Universe Replication Bootstrap Loader
////////////////////////////////////////////////////////////

class UniverseReplicationBootstrapLoader
    : public Visitor<PersistentUniverseReplicationBootstrapInfo> {
 public:
  explicit UniverseReplicationBootstrapLoader(CatalogManager* catalog_manager)
      : catalog_manager_(catalog_manager) {}

  Status Visit(
      const std::string& replication_group_id_str,
      const SysUniverseReplicationBootstrapEntryPB& metadata) REQUIRES(catalog_manager_->mutex_) {
    const xcluster::ReplicationGroupId replication_group_id(replication_group_id_str);
    DCHECK(!ContainsKey(
        catalog_manager_->universe_replication_bootstrap_map_,
        xcluster::ReplicationGroupId(replication_group_id)))
        << "Producer universe already exists: " << replication_group_id;

    // Setup the universe replication info.
    scoped_refptr<UniverseReplicationBootstrapInfo> const bootstrap_info =
        new UniverseReplicationBootstrapInfo(replication_group_id);
    {
      auto l = bootstrap_info->LockForWrite();
      l.mutable_data()->pb.CopyFrom(metadata);

      if (!l->is_done() && !l->is_deleted_or_failed()) {
        // Replication was not fully setup.
        LOG(WARNING) << "Universe replication bootstrap in transient state: "
                     << replication_group_id;

        // Delete tasks in transient state.
        l.mutable_data()->pb.set_failed_on(l->state());
        l.mutable_data()->pb.set_state(SysUniverseReplicationBootstrapEntryPB::DELETING);
        catalog_manager_->replication_bootstraps_to_clear_.push_back(
            bootstrap_info->ReplicationGroupId());
      }

      // Add universe replication bootstrap info to the universe replication map.
      catalog_manager_->universe_replication_bootstrap_map_[bootstrap_info->ReplicationGroupId()] =
          bootstrap_info;

      // Add any failed bootstraps to be cleared
      if (l->is_deleted_or_failed() ||
          l->pb.state() == SysUniverseReplicationBootstrapEntryPB::DELETING) {
        catalog_manager_->replication_bootstraps_to_clear_.push_back(
            bootstrap_info->ReplicationGroupId());
      }
      l.Commit();
    }

    LOG(INFO) << "Loaded metadata for universe replication bootstrap" << bootstrap_info->ToString();
    VLOG(1) << "Metadata for universe replication bootstrap " << bootstrap_info->ToString() << ": "
            << metadata.ShortDebugString();

    return Status::OK();
  }

 private:
  CatalogManager* catalog_manager_;
};

Status CatalogManager::LoadUniverseReplicationBootstrap() {
  LOG_WITH_FUNC(INFO) << "Loading universe replication bootstrap info into memory.";
  auto loader = std::make_unique<UniverseReplicationBootstrapLoader>(this);
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(loader.get()),
      "Failed while visiting universe replication bootstrap info in sys catalog");

  return Status::OK();
}

// Helper class to print a vector of CDCStreamInfo pointers.
namespace {

template <class CDCStreamInfoPointer>
std::string CDCStreamInfosAsString(const std::vector<CDCStreamInfoPointer>& cdc_streams) {
  std::vector<std::string> cdc_stream_ids;
  for (const auto& cdc_stream : cdc_streams) {
    cdc_stream_ids.push_back(cdc_stream->id());
  }
  return AsString(cdc_stream_ids);
}

Status ReturnErrorOrAddWarning(
    const Status& s, bool ignore_errors, DeleteUniverseReplicationResponsePB* resp) {
  if (!s.ok()) {
    if (ignore_errors) {
      // Continue executing, save the status as a warning.
      AppStatusPB* warning = resp->add_warnings();
      StatusToPB(s, warning);
      return Status::OK();
    }
    return s.CloneAndAppend("\nUse 'ignore-errors' to ignore this error.");
  }
  return s;
}
}  // namespace

Status CatalogManager::DropXClusterStreamsOfTables(const std::unordered_set<TableId>& table_ids) {
  if (table_ids.empty()) {
    return Status::OK();
  }

  std::vector<CDCStreamInfoPtr> streams;
  {
    SharedLock lock(mutex_);
    for (const auto& tid : table_ids) {
      auto table_streams = GetXReplStreamsForTable(tid, cdc::XCLUSTER);
      streams.insert(streams.end(), table_streams.begin(), table_streams.end());
    }
  }

  if (streams.empty()) {
    return Status::OK();
  }

  LOG(INFO) << "Deleting xCluster streams for tables:" << AsString(table_ids);

  // Do not delete them here, just mark them as DELETING and the catalog manager background thread
  // will handle the deletion.
  return DropXReplStreams(streams, SysCDCStreamEntryPB::DELETING);
}

Status CatalogManager::DropCDCSDKStreams(const std::unordered_set<TableId>& table_ids) {
  if (table_ids.empty()) {
    return Status::OK();
  }

  std::vector<CDCStreamInfoPtr> streams;
  {
    LockGuard lock(mutex_);
    for (const auto& table_id : table_ids) {
      cdcsdk_tables_to_stream_map_.erase(table_id);
    }
    streams = FindCDCSDKStreamsToDeleteMetadata(table_ids);
  }
  if (streams.empty()) {
    return Status::OK();
  }

  LOG(INFO) << "Deleting CDCSDK streams metadata for tables:" << AsString(table_ids);

  // Do not delete them here, just mark them as DELETING_METADATA and the catalog manager background
  // thread will handle the deletion.
  return DropXReplStreams(streams, SysCDCStreamEntryPB::DELETING_METADATA);
}

Status CatalogManager::AddNewTableToCDCDKStreamsMetadata(
    const TableId& table_id, const NamespaceId& ns_id) {
  LockGuard lock(cdcsdk_unprocessed_table_mutex_);
  VLOG(1) << "Added table: " << table_id << ", under namesapce: " << ns_id
          << ", to namespace_to_cdcsdk_unprocessed_table_map_ to be processed by CDC streams";
  namespace_to_cdcsdk_unprocessed_table_map_[ns_id].insert(table_id);

  return Status::OK();
}

std::vector<CDCStreamInfoPtr> CatalogManager::GetXReplStreamsForTable(
    const TableId& table_id, const cdc::CDCRequestSource cdc_request_source) const {
  std::vector<CDCStreamInfoPtr> streams;
  for (const auto& entry : cdc_stream_map_) {
    auto ltm = entry.second->LockForRead();

    if (!ltm->table_id().empty() &&
        (std::find(ltm->table_id().begin(), ltm->table_id().end(), table_id) !=
         ltm->table_id().end()) &&
        !ltm->started_deleting()) {
      if (cdc_request_source == cdc::XCLUSTER && ltm->namespace_id().empty()) {
        streams.push_back(entry.second);
      } else if (cdc_request_source == cdc::CDCSDK && !ltm->namespace_id().empty()) {
        // For CDCSDK, the table should exclusively belong to qualified table list.
        if (ltm->unqualified_table_id().empty() ||
            std::find(
                ltm->unqualified_table_id().begin(), ltm->unqualified_table_id().end(), table_id) ==
                ltm->unqualified_table_id().end()) {
          streams.push_back(entry.second);
        }
      }
    }
  }
  return streams;
}

std::vector<CDCStreamInfoPtr> CatalogManager::FindCDCSDKStreamsToDeleteMetadata(
    const std::unordered_set<TableId>& table_ids) const {
  std::vector<CDCStreamInfoPtr> streams;

  for (const auto& [_, stream_info] : cdc_stream_map_) {
    auto ltm = stream_info->LockForRead();
    if (ltm->is_deleting_metadata() || ltm->namespace_id().empty()) {
      continue;
    }

    if (std::any_of(ltm->table_id().begin(),
                    ltm->table_id().end(),
                    [&table_ids](const auto& table_id) {
                      return table_ids.contains(table_id);
                    })) {
      streams.push_back(stream_info);
    } else if (ltm->pb.unqualified_table_id_size() > 0) {
      if (std::any_of(
              ltm->unqualified_table_id().begin(), ltm->unqualified_table_id().end(),
              [&table_ids](const auto& unqualified_table_id) {
                return table_ids.contains(unqualified_table_id);
              })) {
        streams.push_back(stream_info);
      }
    }
  }
  return streams;
}

void CatalogManager::GetAllCDCStreams(std::vector<CDCStreamInfoPtr>* streams) {
  streams->clear();
  SharedLock lock(mutex_);
  streams->reserve(cdc_stream_map_.size());
  for (const CDCStreamInfoMap::value_type& e : cdc_stream_map_) {
    if (!e.second->LockForRead()->is_deleting()) {
      streams->push_back(e.second);
    }
  }
}

Status CatalogManager::BackfillMetadataForXRepl(
    const TableInfoPtr& table, const LeaderEpoch& epoch) {
  auto& table_id = table->id();
  VLOG_WITH_FUNC(4) << "Backfilling CDC Metadata for table: " << table_id;

  AlterTableRequestPB alter_table_req_pg_type;
  bool backfill_required = false;
  {
    SharedLock lock(mutex_);
    auto l = table->LockForRead();
    if (table->GetTableType() == PGSQL_TABLE_TYPE) {
      if (!table->has_pg_type_oid()) {
        LOG_WITH_FUNC(INFO) << "backfilling pg_type_oid for table " << table_id;
        auto const att_name_typid_map = VERIFY_RESULT(GetPgAttNameTypidMap(table));
        vector<uint32_t> type_oids;
        for (const auto& entry : att_name_typid_map) {
          type_oids.push_back(entry.second);
        }
        auto ns = VERIFY_RESULT(FindNamespaceByIdUnlocked(table->namespace_id()));
        auto const type_oid_info_map = VERIFY_RESULT(GetPgTypeInfo(ns, &type_oids));
        for (const auto& entry : att_name_typid_map) {
          VLOG(1) << "For table:" << table->name() << " column:" << entry.first
                  << ", pg_type_oid: " << entry.second;
          auto* step = alter_table_req_pg_type.add_alter_schema_steps();
          step->set_type(::yb::master::AlterTableRequestPB_StepType::
                             AlterTableRequestPB_StepType_SET_COLUMN_PG_TYPE);
          auto set_column_pg_type = step->mutable_set_column_pg_type();
          set_column_pg_type->set_name(entry.first);
          uint32_t pg_type_oid = entry.second;

          const YBCPgTypeEntity* type_entity =
              docdb::DocPgGetTypeEntity({(int32_t)pg_type_oid, -1});

          if (type_entity == nullptr && type_oid_info_map.contains(pg_type_oid)) {
            VLOG(1) << "Looking up primitive type for: " << pg_type_oid;
            PgTypeInfo pg_type_info = type_oid_info_map.at(pg_type_oid);
            YbgGetPrimitiveTypeOid(
                pg_type_oid, pg_type_info.typtype, pg_type_info.typbasetype, &pg_type_oid);
            VLOG(1) << "Found primitive type oid: " << pg_type_oid;
          }
          set_column_pg_type->set_pg_type_oid(pg_type_oid);
        }
        backfill_required = true;
      }

      // If pg_type_oid has to be backfilled, we backfill the pgschema_name irrespective of whether
      // it is present or not. It is a safeguard against
      // https://phabricator.dev.yugabyte.com/D17099 which fills the pgschema_name in memory if it
      // is not present without backfilling it to master's disk or tservers.
      // Skip this check for colocated parent tables as they do not have pgschema names.
      if (!IsColocationParentTableId(table_id) &&
          (backfill_required || table->pgschema_name().empty())) {
        LOG_WITH_FUNC(INFO) << "backfilling pgschema_name for table " << table_id;
        string pgschema_name = VERIFY_RESULT(GetPgSchemaName(table));
        VLOG(1) << "For table: " << table->name() << " found pgschema_name: " << pgschema_name;
        alter_table_req_pg_type.set_pgschema_name(pgschema_name);
        backfill_required = true;
      }
    }
  }

  if (backfill_required) {
    // The alter table asynchrnously propagates the change to the tablets. It is okay here as these
    // fields are only required at stream consumption and there is a gap between stream creation and
    // consumption because the former is generally done manually.
    alter_table_req_pg_type.mutable_table()->set_table_id(table_id);
    AlterTableResponsePB alter_table_resp_pg_type;
    return this->AlterTable(
        &alter_table_req_pg_type, &alter_table_resp_pg_type, /*rpc=*/nullptr, epoch);
  } else {
    LOG_WITH_FUNC(INFO)
        << "found pgschema_name and pg_type_oid, no backfilling required for table id: "
        << table_id;
    return Status::OK();
  }
}

Status CatalogManager::CreateCDCStream(
    const CreateCDCStreamRequestPB* req, CreateCDCStreamResponsePB* resp, rpc::RpcContext* rpc,
    const LeaderEpoch& epoch) {
  LOG(INFO) << "CreateCDCStream from " << RequestorString(rpc) << ": " << req->ShortDebugString();

  if (!req->has_table_id() && !req->has_namespace_id()) {
    RETURN_INVALID_REQUEST_STATUS("One of table_id or namespace_id must be provided");
  }

  std::string id_type_option_value(cdc::kTableId);
  std::string record_type_option_value;
  std::string source_type_option_value(CDCRequestSource_Name(cdc::CDCRequestSource::XCLUSTER));

  for (auto option : req->options()) {
    if (option.key() == cdc::kIdType) {
      id_type_option_value = option.value();
    }
    if (option.key() == cdc::kSourceType) {
      source_type_option_value = option.value();
    }
    if (option.key() == cdc::kRecordType ) {
      if (FLAGS_ysql_yb_enable_replica_identity && req->has_cdcsdk_ysql_replication_slot_name()) {
        LOG(WARNING) << " The value for Before Image RecordType will be ignored for replication "
                        "slot consumption. The RecordType for each table will be determined by the "
                        "replica identity of the table at the time of stream creation.";
      }
      record_type_option_value = option.value();
    }
  }

  if (source_type_option_value == CDCRequestSource_Name(cdc::CDCRequestSource::XCLUSTER) ||
      (req->has_table_id() && id_type_option_value != cdc::kNamespaceId)) {
    // xCluster mode.
    std::optional<SysCDCStreamEntryPB::State> initial_state = std::nullopt;
    if (req->has_initial_state()) {
      initial_state = req->initial_state();
    }
    auto stream_id = VERIFY_RESULT(xcluster_manager_->CreateNewXClusterStreamForTable(
        req->table_id(), req->transactional(), initial_state, req->options(), epoch));
    resp->set_stream_id(stream_id.ToString());
    return Status::OK();
  }

  // CDCSDK mode.
  RETURN_NOT_OK(ValidateCDCSDKRequestProperties(
      *req, source_type_option_value, record_type_option_value, id_type_option_value));

  RETURN_NOT_OK(CreateNewCDCStreamForNamespace(*req, resp, rpc, epoch));

  return Status::OK();
}

Status CatalogManager::CreateNewCDCStreamForNamespace(
    const CreateCDCStreamRequestPB& req, CreateCDCStreamResponsePB* resp, rpc::RpcContext* rpc,
    const LeaderEpoch& epoch) {
  std::string namespace_id;
  // The namespace_id field was added to the request as part of the replication slots feature in
  // YSQL. When the replication slot feature is disabled, read from the table_id field for backwards
  // compatibility so that we still support atomic creation of namespace level CDCSDK streams in
  // yb-master independently of the replication slot feature.
  if (FLAGS_ysql_yb_enable_replication_commands) {
    namespace_id = req.namespace_id();
  } else {
    namespace_id = req.table_id();
  }

  auto ns = VERIFY_RESULT(FindNamespaceById(namespace_id));

  // TODO(#19211): Validate that if the ns type is PGSQL, it must have the replication slot name in
  // the request. This can only be done after we have ensured that YSQL is the only client
  // requesting to create CDC streams.

  std::vector<TableInfoPtr> tables;
  {
    LockGuard lock(mutex_);
    tables = FindAllTablesForCDCSDK(ns->id());
  }

  std::vector<TableId> table_ids;
  table_ids.reserve(tables.size());
  for (const auto& table : tables) {
    RETURN_NOT_OK(BackfillMetadataForXRepl(table, epoch));
    table_ids.push_back(table->id());
  }
  VLOG_WITH_FUNC(1) << Format("Creating CDCSDK stream for $0 tables", table_ids.size());

  return CreateNewCdcsdkStream(req, table_ids, ns->id(), resp, epoch, rpc);
}

xrepl::StreamId CatalogManager::GenerateNewXreplStreamId() {
  std::lock_guard l(xrepl_stream_ids_in_use_mutex_);

  while (true) {
    auto stream_id = xrepl::StreamId::GenerateRandom();
    if (!xrepl_stream_ids_in_use_.contains(stream_id)) {
      return stream_id;
    }
  }
}

void CatalogManager::RecoverXreplStreamId(const xrepl::StreamId& stream_id) {
  std::lock_guard l(xrepl_stream_ids_in_use_mutex_);
  xrepl_stream_ids_in_use_.erase(stream_id);
}

Status CatalogManager::CreateNewCdcsdkStream(
    const CreateCDCStreamRequestPB& req, const std::vector<TableId>& table_ids,
    const std::optional<const NamespaceId>& namespace_id, CreateCDCStreamResponsePB* resp,
    const LeaderEpoch& epoch, rpc::RpcContext* rpc) {
  VLOG_WITH_FUNC(1) << "table_ids: " << yb::ToString(table_ids)
                    << ", namespace_id: " << yb::ToString(namespace_id);

  auto start_time = MonoTime::Now();

  bool has_consistent_snapshot_option = false;
  bool consistent_snapshot_option_use = false;
  bool is_history_required_for_replica_identity = false;
  bool disable_dynamic_tables = false;

  CDCStreamInfoPtr stream;
  xrepl::StreamId stream_id = xrepl::StreamId::Nil();

  // Kick-off the CDC state table creation before any other logic.
  RETURN_NOT_OK(CreateCdcStateTableIfNotFound(epoch));

  // TODO(#18934): Move to the DDL transactional atomicity model.
  CDCSDKStreamCreationState cdcsdk_stream_creation_state = CDCSDKStreamCreationState::kInitialized;
  auto se_rollback_failed_create = ScopeExit([this, &stream_id, &cdcsdk_stream_creation_state] {
    WARN_NOT_OK(
        RollbackFailedCreateCDCSDKStream(stream_id, cdcsdk_stream_creation_state),
        Format(
            "Failed to cleanup failed CDC stream $0 at state $1", stream_id,
            cdcsdk_stream_creation_state));
  });

  ReplicationSlotName slot_name;
  auto has_replication_slot_name = req.has_cdcsdk_ysql_replication_slot_name();
  {
    TRACE("Acquired catalog manager lock");
    LockGuard lock(mutex_);

    if (has_replication_slot_name) {
      slot_name = ReplicationSlotName(req.cdcsdk_ysql_replication_slot_name());

      // Duplicate detection.
      if (cdcsdk_replication_slots_to_stream_map_.contains(
              ReplicationSlotName(req.cdcsdk_ysql_replication_slot_name()))) {
        auto stream_id = FindOrNull(cdcsdk_replication_slots_to_stream_map_, slot_name);
        SCHECK(
            stream_id, IllegalState, "Stream with slot name $0 was not found unexpectedly",
            slot_name);
        auto stream = FindOrNull(cdc_stream_map_, *stream_id);
        SCHECK(stream, IllegalState, "Stream with id $0 was not found unexpectedly", stream_id);
        if (!(*stream)->LockForRead()->is_deleting()) {
          return STATUS(
              AlreadyPresent, "CDC stream with the given replication slot name already exists",
              MasterError(MasterErrorPB::OBJECT_ALREADY_PRESENT));
        }

        // A prior replication slot with the same name exists which is in the DELETING state. Remove
        // from the map early so that we don't have to fail this request.
        cdcsdk_replication_slots_to_stream_map_.erase(slot_name);
      }

      if (cdcsdk_replication_slots_to_stream_map_.size() >= FLAGS_max_replication_slots) {
        return STATUS(
            ReplicationSlotLimitReached, "Replication slot limit reached",
            MasterError(MasterErrorPB::REPLICATION_SLOT_LIMIT_REACHED));
      }
    }

    // On a given namespace we allow either new model (replicaion slot) streams or old model
    // (yb-admin) streams. Streams of both types cannot be present on the same namespace.
    for (const auto& entry : cdc_stream_map_) {
      const auto& stream = entry.second;
      if (stream->namespace_id() == namespace_id) {
        if (has_replication_slot_name && stream->GetCdcsdkYsqlReplicationSlotName().empty()) {
          return STATUS(
              IllegalState,
              "Cannot create a replication slot on the same namespace which already has a yb-admin "
              "stream on it. ",
              MasterError(MasterErrorPB::INVALID_REQUEST));
        } else if (
            !has_replication_slot_name && !stream->GetCdcsdkYsqlReplicationSlotName().empty()) {
          return STATUS(
              IllegalState,
              "Cannot create a stream on the same namespace which already has replication slot on "
              "it. ",
              MasterError(MasterErrorPB::INVALID_REQUEST));
        }
      }
    }
  }

  // Check for consistent snapshot option
  if (req.has_cdcsdk_consistent_snapshot_option()) {
    has_consistent_snapshot_option = true;
    consistent_snapshot_option_use =
        req.cdcsdk_consistent_snapshot_option() == CDCSDKSnapshotOption::USE_SNAPSHOT;
  }
  has_consistent_snapshot_option =
      has_consistent_snapshot_option && FLAGS_yb_enable_cdc_consistent_snapshot_streams;

  // Check for dynamic tables option
  if (req.has_cdcsdk_stream_create_options() &&
      req.cdcsdk_stream_create_options().has_cdcsdk_dynamic_tables_option()) {
    disable_dynamic_tables = req.cdcsdk_stream_create_options().cdcsdk_dynamic_tables_option() ==
                             CDCSDKDynamicTablesOption::DYNAMIC_TABLES_DISABLED;
  }

  stream_id = GenerateNewXreplStreamId();
  auto se_recover_stream_id = ScopeExit([&stream_id, this] { RecoverXreplStreamId(stream_id); });

  stream = make_scoped_refptr<CDCStreamInfo>(stream_id);
  stream->mutable_metadata()->StartMutation();
  auto* metadata = &stream->mutable_metadata()->mutable_dirty()->pb;
  DCHECK(namespace_id) << "namespace_id is unexpectedly none";
  metadata->set_namespace_id(*namespace_id);
  for (const auto& table_id : table_ids) {
    metadata->add_table_id(table_id);
    if (FLAGS_ysql_yb_enable_replica_identity && has_replication_slot_name) {
      auto table = VERIFY_RESULT(FindTableById(table_id));
      Schema schema;
      RETURN_NOT_OK(table->GetSchema(&schema));
      PgReplicaIdentity replica_identity = schema.table_properties().replica_identity();

      // If atleast one of the tables in the stream has replica identity other than CHANGE &
      // NOTHING, we will set the history cutoff. UpdatepPeersAndMetrics thread will remove the
      // retention barriers for the tablets belonging to the tables with replica identity CHANGE or
      // NOTHING.
      is_history_required_for_replica_identity |=
          (replica_identity != PgReplicaIdentity::CHANGE &&
           replica_identity != PgReplicaIdentity::NOTHING);

      metadata->mutable_replica_identity_map()->insert({table_id, replica_identity});
      VLOG(1) << "Storing replica identity: " << replica_identity
              << " for table: " << table_id
              << " for stream_id: " << stream_id;
    }
  }

  metadata->set_transactional(req.transactional());

  metadata->mutable_options()->CopyFrom(req.options());

  SysCDCStreamEntryPB::State state = SysCDCStreamEntryPB::ACTIVE;
  if (req.has_initial_state()) {
    state = req.initial_state();
  } else if (has_consistent_snapshot_option) {
    // In case of consistent snapshot option, set state to INITIATED.
    state = SysCDCStreamEntryPB::INITIATED;
  }
  metadata->set_state(state);

  if (has_replication_slot_name) {
    metadata->set_cdcsdk_ysql_replication_slot_name(req.cdcsdk_ysql_replication_slot_name());
  }

  metadata->set_cdcsdk_disable_dynamic_table_addition(disable_dynamic_tables);

  if (req.has_cdcsdk_ysql_replication_slot_plugin_name()) {
    metadata->set_cdcsdk_ysql_replication_slot_plugin_name(
        req.cdcsdk_ysql_replication_slot_plugin_name());
  }

  {
    // Add the stream to the in-memory map.
    TRACE("Acquired catalog manager lock");
    LockGuard lock(mutex_);
    // Check again before inserting to handle concurrent creates.
    if (has_replication_slot_name && cdcsdk_replication_slots_to_stream_map_.contains(slot_name)) {
      return STATUS(
          AlreadyPresent, "CDC stream with the given replication slot name already exists",
          MasterError(MasterErrorPB::OBJECT_ALREADY_PRESENT));
    }

    cdc_stream_map_[stream->StreamId()] = stream;
    se_recover_stream_id.Cancel();

    for (const auto& table_id : table_ids) {
      cdcsdk_tables_to_stream_map_[table_id].insert(stream->StreamId());
    }
    if (has_replication_slot_name) {
      InsertOrDie(&cdcsdk_replication_slots_to_stream_map_, slot_name, stream->StreamId());
    }
  }
  TRACE("Inserted new CDC stream into CatalogManager maps");

  // Any failure beyond this point requires a rollback for CDCSDK streams.
  cdcsdk_stream_creation_state = CDCSDKStreamCreationState::kAddedToMaps;

  RETURN_NOT_OK(
      TEST_CDCSDKFailCreateStreamRequestIfNeeded("CreateCDCSDKStream::kBeforeSysCatalogEntry"));

  // Update the on-disk system catalog.
  RETURN_NOT_OK(CheckLeaderStatusAndSetupError(
      sys_catalog_->Upsert(leader_ready_term(), stream), "inserting CDC stream into sys-catalog",
      resp));

  cdcsdk_stream_creation_state = CDCSDKStreamCreationState::kPreCommitMutation;
  TRACE("Wrote CDC stream to sys-catalog");

  RETURN_NOT_OK(
      TEST_CDCSDKFailCreateStreamRequestIfNeeded("CreateCDCSDKStream::kBeforeInMemoryStateCommit"));

  // Commit the in-memory state.
  stream->mutable_metadata()->CommitMutation();
  cdcsdk_stream_creation_state = CDCSDKStreamCreationState::kPostCommitMutation;

  resp->set_stream_id(stream->id());

  LOG(INFO) << "Created CDC stream " << stream->ToString();

  RETURN_NOT_OK(
      TEST_CDCSDKFailCreateStreamRequestIfNeeded("CreateCDCSDKStream::kAfterInMemoryStateCommit"));

  // Skip if disable_cdc_state_insert_on_setup is set.
  // If this is a bootstrap (initial state not ACTIVE), let the BootstrapProducer logic take care
  // of populating entries in cdc_state.
  if (PREDICT_FALSE(FLAGS_TEST_disable_cdc_state_insert_on_setup) ||
      (req.has_initial_state() && req.initial_state() != master::SysCDCStreamEntryPB::ACTIVE)) {
    cdcsdk_stream_creation_state = CDCSDKStreamCreationState::kReady;
    return Status::OK();
  }

  // At this point, perform all the ALTER TABLE operations to set all retention barriers
  // This will be called synchronously. That is, once this function returns, we are sure
  // that all of the ALTER TABLE operations have completed.

  uint64 consistent_snapshot_time = 0;
  bool is_history_required_for_record_type = false;
  if (!FLAGS_ysql_yb_enable_replica_identity || !has_replication_slot_name) {
    for (auto option : req.options()) {
      if (option.key() == cdc::kRecordType) {
        is_history_required_for_record_type =
            option.value() != CDCRecordType_Name(cdc::CDCRecordType::CHANGE) &&
            option.value() != CDCRecordType_Name(cdc::CDCRecordType::PG_NOTHING);
      }
    }
  }

  // Step 1: Insert checkpoint Invalid in cdc state table.
  // The rollback mechanism relies on finding entries in the CDC state table, so we do this insert
  // before any change that we would like to revert in case of failures.
  // These inserts are treated as non-consistent snapshot since we haven't yet established the
  // consistent snapshot time.
  RETURN_NOT_OK(PopulateCDCStateTable(
      stream->StreamId(), table_ids, false /* has_consistent_snapshot_option */,
      false /* consistent_snapshot_option_use */, 0 /* ignored */, 0 /* ignored */,
      has_replication_slot_name));

  RETURN_NOT_OK(
      TEST_CDCSDKFailCreateStreamRequestIfNeeded("CreateCDCSDKStream::kAfterDummyCDCStateEntries"));

    // Step 2: Set retention barriers for all tables.
  auto require_history_cutoff = consistent_snapshot_option_use ||
                                is_history_required_for_record_type ||
                                is_history_required_for_replica_identity;
  RETURN_NOT_OK(SetAllCDCSDKRetentionBarriers(
      req, rpc, epoch, table_ids, stream->StreamId(), has_consistent_snapshot_option,
      require_history_cutoff));

  RETURN_NOT_OK(
      TEST_CDCSDKFailCreateStreamRequestIfNeeded("CreateCDCSDKStream::kAfterRetentionBarriers"));

  // Step 3: At this stage, the retention barriers have been set using ALTER TABLE and the
  // SnapshotSafeOpId details have been written to the CDC state table via callback.
  // Establish the consistent snapshot time.
  // This time is the same across all involved tablets and is the mechanism through which
  // consistency is established
  auto stream_creation_time = GetCurrentTimeMicros();
  if (has_consistent_snapshot_option) {
    auto cs_hybrid_time = Clock()->MaxGlobalNow();
    consistent_snapshot_time = cs_hybrid_time.ToUint64();
    LOG(INFO) << "Consistent Snapshot Time for stream " << stream->StreamId().ToString()
              << " is: " << consistent_snapshot_time << " = " << cs_hybrid_time;
      resp->set_cdcsdk_consistent_snapshot_time(consistent_snapshot_time);

    // Save the consistent_snapshot_time in the SysCDCStreamEntryPB catalog
    auto l = stream->LockForWrite();
    l.mutable_data()->pb.mutable_cdcsdk_stream_metadata()->set_snapshot_time(
        consistent_snapshot_time);
    l.mutable_data()->pb.mutable_cdcsdk_stream_metadata()->set_consistent_snapshot_option(
        req.cdcsdk_consistent_snapshot_option());
    l.mutable_data()->pb.set_stream_creation_time(stream_creation_time);
    l.mutable_data()->pb.set_state(SysCDCStreamEntryPB::ACTIVE);
    RETURN_NOT_OK(sys_catalog_->Upsert(leader_ready_term(), stream));
    l.Commit();

    LOG(INFO) << "Updating stream metadata with snapshot time " << stream->ToString();
  }
  RETURN_NOT_OK(PopulateCDCStateTable(
      stream->StreamId(), table_ids, has_consistent_snapshot_option, consistent_snapshot_option_use,
      consistent_snapshot_time, stream_creation_time, has_replication_slot_name));

  RETURN_NOT_OK(TEST_CDCSDKFailCreateStreamRequestIfNeeded(
      "CreateCDCSDKStream::kAfterStoringConsistentSnapshotDetails"));

  cdcsdk_stream_creation_state = CDCSDKStreamCreationState::kReady;

  LOG(INFO) << "Stream " << stream_id << " creation took "
            << MonoTime::Now().GetDeltaSince(start_time).ToMilliseconds() << "ms";

  TRACE("Created CDC state entries");
  return Status::OK();
}

Status CatalogManager::RollbackFailedCreateCDCSDKStream(
    const xrepl::StreamId& stream_id, CDCSDKStreamCreationState& cdcsdk_stream_creation_state) {
  if (cdcsdk_stream_creation_state == CDCSDKStreamCreationState::kInitialized ||
      cdcsdk_stream_creation_state == CDCSDKStreamCreationState::kReady ||
      stream_id == xrepl::StreamId::Nil()) {
    return Status::OK();
  }

  LOG(WARNING) << "Rolling back the CDC stream creation for stream_id = " << stream_id
               << ", cdcsdk_stream_creation_state = " << cdcsdk_stream_creation_state;

  CDCStreamInfoPtr stream;
  {
    TRACE("Acquired catalog manager lock for rolling back CDCSDK stream creation");
    SharedLock lock(mutex_);
    stream = cdc_stream_map_[stream_id];
  }

  switch (cdcsdk_stream_creation_state) {
    case CDCSDKStreamCreationState::kAddedToMaps: {
      LockGuard lock(mutex_);
      RETURN_NOT_OK(CleanupXReplStreamFromMaps(stream));
      break;
    }
    case CDCSDKStreamCreationState::kPreCommitMutation:
      // Call AbortMutation since we didn't commit the in-memory changes so that the write lock
      // is released.
      stream->mutable_metadata()->AbortMutation();
      FALLTHROUGH_INTENDED;
    case CDCSDKStreamCreationState::kPostCommitMutation: {
      RETURN_NOT_OK(DropXReplStreams({stream}, SysCDCStreamEntryPB::DELETING));
      break;
    }

    case CDCSDKStreamCreationState::kInitialized: FALLTHROUGH_INTENDED;
    case CDCSDKStreamCreationState::kReady:
      VLOG(2) << "Nothing to rollback";
  }

  return Status::OK();
}

Status CatalogManager::PopulateCDCStateTable(const xrepl::StreamId& stream_id,
                                             const std::vector<TableId>& table_ids,
                                             bool has_consistent_snapshot_option,
                                             bool consistent_snapshot_option_use,
                                             uint64_t consistent_snapshot_time,
                                             uint64_t stream_creation_time,
                                             bool has_replication_slot_name) {
  // Validate that the AlterTable callback has populated the checkpoint i.e. it is no longer
  // OpId::Invalid().
  std::unordered_set<TabletId> seen_tablet_ids;
  if (has_consistent_snapshot_option) {
    std::vector<cdc::CDCStateTableKey> cdc_state_entries;
    Status iteration_status;
    auto all_entry_keys = VERIFY_RESULT(cdc_state_table_->GetTableRange(
        cdc::CDCStateTableEntrySelector().IncludeCheckpoint(), &iteration_status));
    for (const auto& entry_result : all_entry_keys) {
      RETURN_NOT_OK(entry_result);
      const auto& entry = *entry_result;

      if (stream_id == entry.key.stream_id) {
        seen_tablet_ids.insert(entry.key.tablet_id);
        SCHECK(
           *entry.checkpoint != OpId().Invalid(), IllegalState,
            Format(
                "Checkpoint for tablet id $0 unexpectedly found Invalid for stream id $1",
                entry.key.tablet_id, stream_id));
      }
    }
    RETURN_NOT_OK(iteration_status);
  }

  std::vector<cdc::CDCStateTableEntry> entries;
  for (const auto& table_id : table_ids) {
    auto table = VERIFY_RESULT(FindTableById(table_id));
    for (const auto& tablet : table->GetTablets()) {
      cdc::CDCStateTableEntry entry(tablet->id(), stream_id);
      if (has_consistent_snapshot_option) {
        // We must have seen this tablet id in the above check for Invalid checkpoint. If not, this
        // means that the list of tablets is different from what it was at the start of the stream
        // creation which indicates a tablet split. In that case, fail the creation and let the
        // client retry the creation again.
        if (!seen_tablet_ids.contains(tablet->id())) {
          return STATUS_FORMAT(
              IllegalState, "CDC State Table entry unexpectedly not found for tablet id $0",
              tablet->id());
        }

        // For USE_SNAPSHOT option, leave entry in POST_SNAPSHOT_BOOTSTRAP state
        // For NOEXPORT_SNAPSHOT option, leave entry in SNAPSHOT_DONE state
        if (consistent_snapshot_option_use)
          entry.snapshot_key = "";

        entry.active_time = stream_creation_time;
        entry.cdc_sdk_safe_time = consistent_snapshot_time;
      } else {
        entry.checkpoint = OpId().Invalid();
        entry.active_time = 0;
        entry.cdc_sdk_safe_time = 0;
      }
      entries.push_back(std::move(entry));

      // For a consistent snapshot streamm, if it is a Colocated table,
      // add the colocated table snapshot entry also
      if (has_consistent_snapshot_option && table->colocated()) {
        cdc::CDCStateTableEntry col_entry(tablet->id(), stream_id, table_id);
        if (consistent_snapshot_option_use)
          col_entry.snapshot_key = "";

        col_entry.active_time = GetCurrentTimeMicros();
        col_entry.cdc_sdk_safe_time = consistent_snapshot_time;
        entries.push_back(std::move(col_entry));
      }
    }
  }

  // Add a new entry in cdc_state table representing the replication slot for the associated stream.
  // This entry holds metadata for two main usages:
  // 1. Represent the slot's consistent point i.e. first record sent in the streaming phase will
  // have LSN & txnID set to 2.
  // 2. Initialize components (LSN & txnID generators) of the CDCSDK Virtual WAL on restarts.
  //
  // If these values are changed here, also update the consistent point sent as part of the
  // creation of logical replication slot in walsender.c and slotfuncs.c.
  if (FLAGS_ysql_yb_enable_replication_slot_consumption && has_consistent_snapshot_option &&
      has_replication_slot_name) {
    cdc::CDCStateTableEntry entry(kCDCSDKSlotEntryTabletId, stream_id);
    std::ostringstream oss;
    oss << consistent_snapshot_time << 'F';
    entry.confirmed_flush_lsn = 2;
    entry.restart_lsn = 1;
    entry.xmin = 1;
    entry.record_id_commit_time = consistent_snapshot_time;
    entry.cdc_sdk_safe_time = consistent_snapshot_time;
    entry.last_pub_refresh_time = consistent_snapshot_time;
    entry.pub_refresh_times = "";
    entry.last_decided_pub_refresh_time = oss.str();
    entries.push_back(entry);
    VLOG(1) << "Added entry in cdc_state for the replication slot with tablet_id: "
            << kCDCSDKSlotEntryTabletId << " stream_id: " << stream_id;
  }

  return cdc_state_table_->UpsertEntries(entries);
}

Status CatalogManager::SetAllCDCSDKRetentionBarriers(
  const CreateCDCStreamRequestPB& req, rpc::RpcContext* rpc, const LeaderEpoch& epoch,
  const std::vector<TableId>& table_ids, const xrepl::StreamId& stream_id,
  const bool has_consistent_snapshot_option, const bool require_history_cutoff) {
  VLOG_WITH_FUNC(4) << "Setting All retention barriers for stream: " << stream_id;

  for (const auto& table_id : table_ids) {
    auto table = VERIFY_RESULT(FindTableById(table_id));
    {
      auto l = table->LockForRead();
      if (l->started_deleting()) {
        return STATUS(
            NotFound, "Table does not exist", table_id,
            MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
      }
    }

    AlterTableRequestPB alter_table_req;
    alter_table_req.mutable_table()->set_table_id(table_id);
    alter_table_req.set_wal_retention_secs(GetAtomicFlag(&FLAGS_cdc_wal_retention_time_secs));

    if (has_consistent_snapshot_option) {
      alter_table_req.set_cdc_sdk_stream_id(stream_id.ToString());
      alter_table_req.set_cdc_sdk_require_history_cutoff(require_history_cutoff);
    }

    AlterTableResponsePB alter_table_resp;
    Status s = this->AlterTable(&alter_table_req, &alter_table_resp, rpc, epoch);
    if (!s.ok()) {
      return STATUS(
          InternalError,
          Format("Unable to set retention barries for table, error: $0", s.message()),
          table_id, MasterError(MasterErrorPB::INTERNAL_ERROR));
    }
  }

  if (has_consistent_snapshot_option) {
    auto deadline = rpc->GetClientDeadline();
    // TODO(#18934): Handle partial failures by rolling back all changes.
    for (const auto& table_id : table_ids) {
      RETURN_NOT_OK(WaitForAlterTableToFinish(table_id, deadline));
    }
    RETURN_NOT_OK(WaitForSnapshotSafeOpIdToBePopulated(stream_id, table_ids, deadline));
  }

  return Status::OK();
}

Status CatalogManager::SetXReplWalRetentionForTable(
    const TableInfoPtr& table, const LeaderEpoch& epoch) {
  auto& table_id = table->id();
  VLOG_WITH_FUNC(4) << "Setting WAL retention for table: " << table_id;

  SCHECK(
      !table->IsPreparing(), IllegalState,
      "Cannot set WAL retention of a table that has not yet been fully created");

  const auto min_wal_retention_secs = FLAGS_cdc_wal_retention_time_secs;
  const auto table_wal_retention_secs = table->LockForRead()->pb.wal_retention_secs();
  if (table_wal_retention_secs >= min_wal_retention_secs) {
    VLOG_WITH_FUNC(1) << "Table " << table_id << " already has WAL retention set to "
                      << table_wal_retention_secs
                      << ", which is equal or higher than cdc_wal_retention_time_secs: "
                      << min_wal_retention_secs;
  } else {
    AlterTableRequestPB alter_table_req;
    alter_table_req.mutable_table()->set_table_id(table_id);
    alter_table_req.set_wal_retention_secs(min_wal_retention_secs);

    AlterTableResponsePB alter_table_resp;
    RETURN_NOT_OK_PREPEND(
        this->AlterTable(&alter_table_req, &alter_table_resp, /*rpc=*/nullptr, epoch),
        Format("Unable to change the WAL retention time for table $0", table_id));
  }

  // Ideally we should WaitForAlterTableToFinish to ensure the change has propagated to all
  // tablet peers. But since we have 15min default WAL retention and this operation completes much
  // sooner, we skip it.
  return Status::OK();
}

Status CatalogManager::PopulateCDCStateTableWithCDCSDKSnapshotSafeOpIdDetails(
    const scoped_refptr<TableInfo>& table,
    const yb::TabletId& tablet_id,
    const xrepl::StreamId& cdc_sdk_stream_id,
    const yb::OpIdPB& snapshot_safe_opid,
    const yb::HybridTime& proposed_snapshot_time,
    bool require_history_cutoff) {

  TEST_SYNC_POINT("PopulateCDCStateTableWithCDCSDKSnapshotSafeOpIdDetails::Start");

  LOG_WITH_FUNC(INFO) << "Table id: " << table->id()
                      << "Tablet id: " << tablet_id
                      << ", Stream id:" << cdc_sdk_stream_id.ToString()
                      << ", snapshot safe opid: " << snapshot_safe_opid.term()
                      << " and " << snapshot_safe_opid.index()
                      << ", proposed snapshot time: " << proposed_snapshot_time.ToUint64()
                      << ", require history cutoff: " << require_history_cutoff;

  std::vector<cdc::CDCStateTableEntry> entries;

  cdc::CDCStateTableEntry entry(tablet_id, cdc_sdk_stream_id);
  entry.checkpoint = OpId::FromPB(snapshot_safe_opid);
  entry.cdc_sdk_safe_time = proposed_snapshot_time.ToUint64();
  if (require_history_cutoff)
    entry.snapshot_key = "";

  entry.active_time = GetCurrentTimeMicros();
  entry.last_replication_time = proposed_snapshot_time.GetPhysicalValueMicros();
  entries.push_back(std::move(entry));

  // add the colocated table snapshot row if it is a colocated table
  if (table->colocated()) {
    cdc::CDCStateTableEntry col_entry(tablet_id, cdc_sdk_stream_id, table->id());
    col_entry.checkpoint = OpId::FromPB(snapshot_safe_opid);
    col_entry.cdc_sdk_safe_time = proposed_snapshot_time.ToUint64();
    if (require_history_cutoff)
      col_entry.snapshot_key = "";

    col_entry.active_time = GetCurrentTimeMicros();
    col_entry.last_replication_time = proposed_snapshot_time.GetPhysicalValueMicros();
    entries.push_back(std::move(col_entry));
  }

  RETURN_NOT_OK(TEST_CDCSDKFailCreateStreamRequestIfNeeded(
      "CreateCDCSDKStream::kWhileStoringConsistentSnapshotDetails"));

  return cdc_state_table_->UpsertEntries(entries);
}

Status CatalogManager::PopulateCDCStateTableOnNewTableCreation(
    const scoped_refptr<TableInfo>& table,
    const TabletId& tablet_id,
    const OpId& safe_opid) {

  TEST_SYNC_POINT("PopulateCDCStateTableOnNewTableCreation::Start");

  auto namespace_id = table->namespace_id();
  std::vector<CDCStreamInfoPtr> streams;

  // Get all the CDCSDK streams on the namespace
  {
    SharedLock lock(mutex_);
    for (const auto& entry : cdc_stream_map_) {
      const auto& stream_info = entry.second;
      if (stream_info->IsCDCSDKStream() && stream_info->namespace_id() == namespace_id) {
        streams.emplace_back(stream_info);
      }
    }
  }

  // This is not expected to happen since we check atleast one stream exists before calling create
  // tablet rpc
  RSTATUS_DCHECK(
      !streams.empty(), NotFound, "Did not find any stream on the namespace: $0", namespace_id);

  std::vector<cdc::CDCStateTableEntry> entries;
  entries.reserve(streams.size());

  for (auto const& stream : streams) {
    entries.emplace_back(tablet_id, stream->StreamId());
    auto& entry = entries.back();
    if (stream->IsConsistentSnapshotStream()) {
      auto consistent_snapshot_time = stream->GetConsistentSnapshotHybridTime();
      entry.checkpoint = safe_opid;
      entry.active_time = GetCurrentTimeMicros();
      entry.cdc_sdk_safe_time = consistent_snapshot_time.ToUint64();
      entry.last_replication_time = consistent_snapshot_time.GetPhysicalValueMicros();
    } else {
      entry.checkpoint = OpId::Invalid();
      entry.active_time = 0;
      entry.cdc_sdk_safe_time = 0;
    }
    LOG_WITH_FUNC(INFO) << "Table id: " << table->id() << ", tablet id: " << tablet_id
                        << ", stream id: " << stream->StreamId()
                        << ", Safe OpId: " << safe_opid.term << " and " << safe_opid.index
                        << ", cdc_sdk_safe_time: " << *(entry.cdc_sdk_safe_time);
  }

  auto status = cdc_state_table_->InsertEntries(entries);
  if (!status.ok()) {
    LOG(WARNING) << "Encoutered error while trying to add tablet:" << tablet_id
                 << " of table: " << table->id() << ", to cdc_state table: " << status;
    return status;
  }

  TEST_SYNC_POINT("PopulateCDCStateTableOnNewTableCreation::End");
  return Status::OK();
}

Status CatalogManager::WaitForSnapshotSafeOpIdToBePopulated(
    const xrepl::StreamId& stream_id, const std::vector<TableId>& table_ids,
    CoarseTimePoint deadline) {

  auto num_expected_tablets = 0;
  for (const auto& table_id : table_ids) {
    auto table = VERIFY_RESULT(FindTableById(table_id));
    num_expected_tablets += table->GetTablets().size();
  }

  return WaitFor(
      [&stream_id, &num_expected_tablets, this]() -> Result<bool> {
        VLOG(1) << "Checking snapshot safe opids for stream: " << stream_id;

        std::vector<cdc::CDCStateTableKey> cdc_state_entries;
        Status iteration_status;
        auto all_entry_keys = VERIFY_RESULT(cdc_state_table_->GetTableRange(
            cdc::CDCStateTableEntrySelector().IncludeCheckpoint(), &iteration_status));

        auto num_rows = 0;
        for (const auto& entry_result : all_entry_keys) {
          RETURN_NOT_OK(entry_result);
          const auto& entry = *entry_result;

          if (stream_id == entry.key.stream_id) {
            num_rows++;
            if (!entry.checkpoint.has_value() || *entry.checkpoint == OpId().Invalid()) {
              return false;
            }
          }
        }

        RETURN_NOT_OK(iteration_status);
        VLOG(1) << "num_rows=" << num_rows << ", num_expected_tablets=" << num_expected_tablets;
        // In case of colocated tables, there would be extra rows, check for >=
        return (num_rows >= num_expected_tablets);
      },
      deadline - CoarseMonoClock::now(),
      Format("Waiting for snapshot safe opids to be populated for stream_id: $0", stream_id),
      500ms /* initial_delay */, 1 /* delay_multiplier */);
}

Status CatalogManager::DeleteCDCStream(
    const DeleteCDCStreamRequestPB* req, DeleteCDCStreamResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteCDCStream request from " << RequestorString(rpc) << ": "
            << req->ShortDebugString();

  if (req->stream_id_size() == 0 && req->cdcsdk_ysql_replication_slot_name_size() == 0) {
    return STATUS(
        InvalidArgument, "No CDC Stream ID or YSQL Replication Slot Name given",
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  std::vector<CDCStreamInfoPtr> streams;
  {
    SharedLock lock(mutex_);

    for (const auto& stream_id : req->stream_id()) {
      auto stream_opt = VERIFY_RESULT(GetStreamIfValidForDelete(
          VERIFY_RESULT(xrepl::StreamId::FromString(stream_id)), req->force_delete()));
      if (stream_opt) {
        streams.emplace_back(std::move(*stream_opt));
      } else {
        resp->add_not_found_stream_ids(stream_id);
      }
    }

    for (const auto& replication_slot_name : req->cdcsdk_ysql_replication_slot_name()) {
      auto slot_name = ReplicationSlotName(replication_slot_name);
      auto stream_it = FindOrNull(cdcsdk_replication_slots_to_stream_map_, slot_name);
      auto stream_id = stream_it ? *stream_it : xrepl::StreamId::Nil();
      auto stream_opt =
          VERIFY_RESULT(GetStreamIfValidForDelete(std::move(stream_id), req->force_delete()));
      if (stream_opt) {
        streams.emplace_back(std::move(*stream_opt));
      } else {
        resp->add_not_found_cdcsdk_ysql_replication_slot_names(replication_slot_name);
      }
    }
  }

  const auto& not_found_stream_ids = resp->not_found_stream_ids();
  const auto& not_found_cdcsdk_ysql_replication_slot_names =
      resp->not_found_cdcsdk_ysql_replication_slot_names();
  if ((!not_found_stream_ids.empty() || !not_found_cdcsdk_ysql_replication_slot_names.empty()) &&
      !req->ignore_errors()) {
    std::vector<std::string> missing_streams(
        resp->not_found_stream_ids_size() +
        resp->not_found_cdcsdk_ysql_replication_slot_names_size());
    missing_streams.insert(
        missing_streams.end(), not_found_stream_ids.begin(), not_found_stream_ids.end());
    missing_streams.insert(
        missing_streams.end(), not_found_cdcsdk_ysql_replication_slot_names.begin(),
        not_found_cdcsdk_ysql_replication_slot_names.end());
    return STATUS(
        NotFound,
        Format(
            "Did not find all requested CDC streams. Missing streams: [$0]. Request: $1",
            JoinStrings(missing_streams, ","), req->ShortDebugString()),
        MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  // Do not delete them here, just mark them as DELETING and the catalog manager background thread
  // will handle the deletion.
  Status s = DropXReplStreams(streams, SysCDCStreamEntryPB::DELETING);
  if (!s.ok()) {
    if (s.IsIllegalState()) {
      PANIC_RPC(rpc, s.message().ToString());
    }
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }

  LOG(INFO) << "Successfully deleted CDC streams " << CDCStreamInfosAsString(streams)
            << " per request from " << RequestorString(rpc);

  return Status::OK();
}

Result<std::optional<CDCStreamInfoPtr>> CatalogManager::GetStreamIfValidForDelete(
    const xrepl::StreamId& stream_id, bool force_delete) {
  auto stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  if (stream == nullptr || stream->LockForRead()->started_deleting()) {
    return std::nullopt;
  }

  auto ltm = stream->LockForRead();
  if (!force_delete && ltm->pb.state() == SysCDCStreamEntryPB::ACTIVE) {
    for (const auto& option : ltm->pb.options()) {
      if (option.key() == "record_format") {
        if (option.value() == "WAL") {
          return STATUS(
              NotSupported,
              "Cannot delete an xCluster Stream in replication. "
              "Use 'force_delete' to override",
              MasterError(MasterErrorPB::INVALID_REQUEST));
        }
        break;
      }
    }
  }
  return stream;
}

Status CatalogManager::DropXReplStreams(
    const std::vector<CDCStreamInfoPtr>& streams, SysCDCStreamEntryPB::State delete_state) {
  if (streams.empty()) {
    return Status::OK();
  }
  RSTATUS_DCHECK(
      delete_state == SysCDCStreamEntryPB::DELETING_METADATA ||
          delete_state == SysCDCStreamEntryPB::DELETING,
      IllegalState,
      Format("Invalid delete state $0 provided", SysCDCStreamEntryPB::State_Name(delete_state)));

  std::vector<CDCStreamInfo::WriteLock> locks;
  std::vector<CDCStreamInfo*> streams_to_mark;
  locks.reserve(streams.size());
  for (auto& stream : streams) {
    auto l = stream->LockForWrite();
    l.mutable_data()->pb.set_state(delete_state);
    locks.push_back(std::move(l));
    streams_to_mark.push_back(stream.get());
  }
  // The mutation will be aborted when 'l' exits the scope on early return.
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->Upsert(leader_ready_term(), streams_to_mark),
      "updating XRepl streams in sys-catalog"));
  LOG(INFO) << "Successfully marked XRepl streams " << CDCStreamInfosAsString(streams_to_mark)
            << " as " << SysCDCStreamEntryPB::State_Name(delete_state) << " in sys catalog";
  for (auto& lock : locks) {
    lock.Commit();
  }
  return Status::OK();
}

Status CatalogManager::FindCDCSDKStreamsForAddedTables(
    TableStreamIdsMap* table_to_unprocessed_streams_map) {
  std::unordered_map<NamespaceId, std::unordered_set<TableId>> namespace_to_unprocessed_table_map;
  {
    SharedLock lock(cdcsdk_unprocessed_table_mutex_);
    int32_t found_unprocessed_tables = 0;
    for (const auto& [ns_id, table_ids] : namespace_to_cdcsdk_unprocessed_table_map_) {
      for (const auto& table_id : table_ids) {
        namespace_to_unprocessed_table_map[ns_id].insert(table_id);
        if (++found_unprocessed_tables >= FLAGS_cdcsdk_table_processing_limit_per_run) {
          break;
        }
      }
      if (found_unprocessed_tables == FLAGS_cdcsdk_table_processing_limit_per_run) {
        break;
      }
    }
  }

  SharedLock lock(mutex_);
  for (const auto& [stream_id, stream_info] : cdc_stream_map_) {
    if (stream_info->namespace_id().empty()) {
      continue;
    }

    // skip streams on which dynamic table addition is disabled.
    if(stream_info->IsDynamicTableAdditionDisabled()) {
      continue;
    }

    auto const unprocessed_tables =
        FindOrNull(namespace_to_unprocessed_table_map, stream_info->namespace_id());
    if (!unprocessed_tables) {
      continue;
    }

    auto ltm = stream_info->LockForRead();
    if (ltm->pb.state() == SysCDCStreamEntryPB::ACTIVE ||
        ltm->pb.state() == SysCDCStreamEntryPB::DELETING_METADATA) {
      for (const auto& unprocessed_table_id : *unprocessed_tables) {
        auto table = tables_->FindTableOrNull(unprocessed_table_id);
        if (!table) {
          LOG_WITH_FUNC(WARNING) << "Table " << unprocessed_table_id
                                 << " deleted before it could be processed";
          continue;
        }
        Schema schema;
        auto status = table->GetSchema(&schema);
        if (!status.ok()) {
          LOG_WITH_FUNC(WARNING) << "Error while getting schema for table: " << table->name();
          continue;
        }

        if (!IsTableEligibleForCDCSDKStream(table, schema)) {
          RemoveTableFromCDCSDKUnprocessedMap(unprocessed_table_id, stream_info->namespace_id());
          continue;
        }

        bool present_in_qualified_table_list =
            std::find(ltm->table_id().begin(), ltm->table_id().end(), unprocessed_table_id) !=
            ltm->table_id().end();
        bool present_in_unqualified_table_list = false;
        if (ltm->pb.unqualified_table_id_size() > 0) {
          present_in_unqualified_table_list =
              std::find(
                  ltm->unqualified_table_id().begin(), ltm->unqualified_table_id().end(),
                  unprocessed_table_id) != ltm->unqualified_table_id().end();
        }

        if (!present_in_qualified_table_list && !present_in_unqualified_table_list) {
          (*table_to_unprocessed_streams_map)[unprocessed_table_id].push_back(stream_info);
          VLOG(1) << "Will try and add table: " << unprocessed_table_id
                  << ", to stream: " << stream_info->id();
        }
      }
    }
  }

  for (const auto& [ns_id, unprocessed_table_ids] : namespace_to_unprocessed_table_map) {
    for (const auto& unprocessed_table_id : unprocessed_table_ids) {
      if (!table_to_unprocessed_streams_map->contains(unprocessed_table_id)) {
        // This means we found no active CDCSDK stream where this table was missing, hence we can
        // remove this table from 'RemoveTableFromCDCSDKUnprocessedMap'.
        RemoveTableFromCDCSDKUnprocessedMap(unprocessed_table_id, ns_id);
      }
    }
  }

  return Status::OK();
}

void CatalogManager::FindAllTablesMissingInCDCSDKStream(
    const xrepl::StreamId& stream_id,
    const google::protobuf::RepeatedPtrField<std::string>& table_ids,
    const std::vector<TableInfoPtr>& eligible_tables_info,
    const google::protobuf::RepeatedPtrField<std::string>& unqualified_table_ids) {
  std::unordered_set<TableId> stream_qualified_table_ids;
  std::unordered_set<TableId> stream_unqualified_table_ids;
  // Store all qualified table_ids associated with the stream in 'stream_qualified_table_ids'.
  for (const auto& table_id : table_ids) {
    stream_qualified_table_ids.insert(table_id);
  }

  for (const auto& table_id : unqualified_table_ids) {
    stream_unqualified_table_ids.insert(table_id);
  }

  // Get all the tables associated with the namespace.
  // If we find any table present only in the namespace, but not in the stream's qualified &
  // unqualified table list, we add the table id to 'cdcsdk_unprocessed_tables'.
  for (const auto& table_info : eligible_tables_info) {
    auto ltm = table_info->LockForRead();
    if (!stream_qualified_table_ids.contains(table_info->id()) &&
        !stream_unqualified_table_ids.contains(table_info->id())) {
      LOG(INFO) << "Found unprocessed table: " << table_info->id()
                << ", for stream: " << stream_id;
      LockGuard lock(cdcsdk_unprocessed_table_mutex_);
      namespace_to_cdcsdk_unprocessed_table_map_[table_info->namespace_id()].insert(
          table_info->id());
    }
  }
}

Status CatalogManager::FindCDCSDKStreamsForNonEligibleTables(
    TableStreamIdsMap* non_user_tables_to_streams_map) {
  std::unordered_map<NamespaceId, std::unordered_set<TableId>> namespace_to_non_user_table_map;
  {
    SharedLock lock(cdcsdk_non_eligible_table_mutex_);
    int32_t found_non_user_tables = 0;
    for (const auto& [ns_id, table_ids] : namespace_to_cdcsdk_non_eligible_table_map_) {
      for (const auto& table_id : table_ids) {
        namespace_to_non_user_table_map[ns_id].insert(table_id);
        if (++found_non_user_tables >= FLAGS_cdcsdk_table_processing_limit_per_run) {
          break;
        }
      }

      if (found_non_user_tables == FLAGS_cdcsdk_table_processing_limit_per_run) {
        break;
      }
    }
  }

  if (namespace_to_non_user_table_map.empty()) {
    return Status::OK();
  }

  {
    SharedLock lock(mutex_);
    for (const auto& [stream_id, stream_info] : cdc_stream_map_) {
      if (stream_info->namespace_id().empty()) {
        continue;
      }

      // Removal of non-eligible tables will only be done on CDC stream that are not associated
      // with a replication slot.
      if (!stream_info->GetCdcsdkYsqlReplicationSlotName().empty()) {
        continue;
      }

      const auto non_user_tables =
          FindOrNull(namespace_to_non_user_table_map, stream_info->namespace_id());
      if (!non_user_tables) {
        continue;
      }

      auto ltm = stream_info->LockForRead();
      if (ltm->pb.state() == SysCDCStreamEntryPB::ACTIVE ||
          ltm->pb.state() == SysCDCStreamEntryPB::DELETING_METADATA) {
        for (const auto& non_user_table_id : *non_user_tables) {
          auto table = tables_->FindTableOrNull(non_user_table_id);
          if (!table) {
            LOG_WITH_FUNC(WARNING)
                << "Table " << non_user_table_id << " deleted before it could be removed";
            continue;
          }

          if (std::find(ltm->table_id().begin(), ltm->table_id().end(), non_user_table_id) !=
              ltm->table_id().end()) {
            (*non_user_tables_to_streams_map)[non_user_table_id].push_back(stream_info);
            VLOG(1) << "Will try and remove table: " << non_user_table_id
                    << ", from stream: " << stream_info->id();
          }
        }
      }
    }
  }

  for (const auto& [ns_id, non_user_table_ids] : namespace_to_non_user_table_map) {
    for (const auto& non_user_table_id : non_user_table_ids) {
      if (!non_user_tables_to_streams_map->contains(non_user_table_id)) {
        // This means we found no active CDCSDK stream where this table was present, hence we can
        // remove this table from 'namespace_to_cdcsdk_non_eligible_table_map_'.
        RemoveTableFromCDCSDKNonEligibleTableMap(non_user_table_id, ns_id);
      }
    }
  }

  return Status::OK();
}

void CatalogManager::FindAllNonEligibleTablesInCDCSDKStream(
    const xrepl::StreamId& stream_id,
    const google::protobuf::RepeatedPtrField<std::string>& table_ids,
    const std::vector<TableInfoPtr>& eligible_tables_info) {
  // If we find any table present only in the the stream, but not in the list of eligible tables in
  // namespace for CDC, we add the table id to 'namespace_to_cdcsdk_non_eligible_table_map_'.
  std::unordered_set<TableId> user_table_ids;
  for (const auto& table_info : eligible_tables_info) {
    user_table_ids.insert(table_info->id());
  }

  std::unordered_set<TableId> stream_table_ids;
  // Store all table_ids associated with the stream in 'stream_table_ids'.
  for (const auto& table_id : table_ids) {
    if (!user_table_ids.contains(table_id)) {
      auto table_info = GetTableInfoUnlocked(table_id);
      if (table_info) {
        Schema schema;
        Status status = table_info->GetSchema(&schema);
        if (!status.ok()) {
          LOG_WITH_FUNC(WARNING) << "Error while getting schema for table: " << table_info->name();
          // Skip this table for now, it will be revisited for removal on master restart/master
          // leader change.
          continue;
        }

        // Re-confirm this table is not meant to be part of a CDC stream.
        if (!IsTableEligibleForCDCSDKStream(table_info, schema)) {
          LOG(INFO) << "Found a non-eligible table: " << table_info->id()
                    << ", for stream: " << stream_id;
          LockGuard lock(cdcsdk_non_eligible_table_mutex_);
          namespace_to_cdcsdk_non_eligible_table_map_[table_info->namespace_id()].insert(
              table_info->id());
        } else {
          // Ideally we are not expected to enter the else clause.
          LOG(WARNING) << "Found table " << table_id << " in metadata of stream " << stream_id
                       << " that is not present in the eligible list of tables "
                          "from the namespace for CDC";
        }
      } else {
        LOG(INFO) << "Found table " << table_id << " in stream " << stream_id
                  << " metadata that is not present in master.";
      }
    }
  }
}

void CatalogManager::FindAllUnproccesedUnqualifiedTablesInCDCSDKStream(
    const xrepl::StreamId& stream_id,
    const google::protobuf::RepeatedPtrField<std::string>& qualified_table_ids,
    const google::protobuf::RepeatedPtrField<std::string>& unqualified_table_ids,
    const std::vector<TableInfoPtr>& eligible_tables_info) {
  std::unordered_set<TableId> eligible_tables_for_stream;
  std::unordered_set<TableId> qualified_tables_in_stream;
  for (const auto& table : eligible_tables_info) {
    eligible_tables_for_stream.insert(table->id());
  }

  qualified_tables_in_stream.insert(qualified_table_ids.begin(), qualified_table_ids.end());

  // Unprocessed unqualified tables will be present in both the lists (qualified & unqualified).
  for (const auto& unqualified_table_id : unqualified_table_ids) {
    if (qualified_tables_in_stream.contains(unqualified_table_id)) {
      DCHECK(eligible_tables_for_stream.contains(unqualified_table_id));
      LOG(INFO) << "Found an unprocessed unqualified table " << unqualified_table_id
                << " for stream: " << stream_id;
      LockGuard lock(cdcsdk_unqualified_table_removal_mutex_);
      cdcsdk_unprocessed_unqualified_tables_to_streams_[unqualified_table_id].insert(stream_id);
    }
  }
}

Status CatalogManager::FindCDCSDKStreamsForUnprocessedUnqualifiedTables(
    TableStreamIdsMap* tables_to_be_removed_streams_map) {
  std::unordered_map<TableId, std::unordered_set<xrepl::StreamId>> unprocessed_table_to_streams_map;
  {
    SharedLock l(cdcsdk_unqualified_table_removal_mutex_);
    int unprocessed_tables = 0;
    for (const auto& [table_id, streams] : cdcsdk_unprocessed_unqualified_tables_to_streams_) {
      unprocessed_table_to_streams_map[table_id] = streams;
      if (++unprocessed_tables >= FLAGS_cdcsdk_table_processing_limit_per_run) {
        break;
      }
    }
  }

  if (unprocessed_table_to_streams_map.empty()) {
    return Status::OK();
  }

  std::unordered_map<TableId, std::unordered_set<xrepl::StreamId>> streams_not_to_be_processed;
  {
    SharedLock lock(mutex_);
    for (const auto& [table_id, streams] : unprocessed_table_to_streams_map) {
      for (const auto& stream_id : streams) {
        CDCStreamInfoPtr stream;
        stream = FindPtrOrNull(cdc_stream_map_, stream_id);

        Status s = ValidateStreamForTableRemoval(stream);
        if (!s.ok()) {
          // This stream cannot be processed for removal of tables, therefore delete the stream from
          // the set.
          streams_not_to_be_processed[table_id].insert(stream_id);
          continue;
        }

        (*tables_to_be_removed_streams_map)[table_id].push_back(stream);
        VLOG(1) << "Will try to remove table: " << table_id
                << ", from stream: " << stream->StreamId();
      }
    }
  }

  for (const auto& [table_id, streams] : streams_not_to_be_processed) {
    // For each table, remove all streams that cannot not be processed from
    // 'cdcsdk_unprocessed_unqualified_tables_to_streams_' map.
    RemoveStreamsFromUnprocessedRemovedTableMap(table_id, streams);
  }

  return Status::OK();
}

Status CatalogManager::ValidateCDCSDKRequestProperties(
    const CreateCDCStreamRequestPB& req, const std::string& source_type_option_value,
    const std::string& record_type_option_value, const std::string& id_type_option_value) {
  if (source_type_option_value != CDCRequestSource_Name(cdc::CDCRequestSource::CDCSDK)) {
    RETURN_INVALID_REQUEST_STATUS("Namespace CDC stream is only supported for CDCSDK");
  }

  if (id_type_option_value != cdc::kNamespaceId) {
    RETURN_INVALID_REQUEST_STATUS(
        "Invalid id_type in options. Expected to be NAMESPACEID for all CDCSDK streams");
  }

  if (!FLAGS_ysql_yb_enable_replication_commands &&
      req.has_cdcsdk_ysql_replication_slot_name()) {
    // Should never happen since the YSQL commands also check the flag.
    RETURN_INVALID_REQUEST_STATUS(
        "Creation of CDCSDK stream with a replication slot name is disallowed");
  }

  // TODO: Validate that the replication slot output plugin name is provided if
  // ysql_yb_enable_replication_slot_consumption is true. This can only be done after we have
  // fully deprecated the yb-admin commands for CDC stream creation.

  // No need to validate the record_type for replication slot consumption.
  if (FLAGS_ysql_yb_enable_replica_identity && req.has_cdcsdk_ysql_replication_slot_name()) {
    return Status::OK();
  }

  cdc::CDCRecordType record_type_pb;
  if (!cdc::CDCRecordType_Parse(record_type_option_value, &record_type_pb)) {
    return STATUS(InvalidArgument, "Invalid CDCRecordType value", record_type_option_value);
  }

  switch (record_type_pb) {
    case cdc::CDCRecordType::PG_FULL:
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::PG_CHANGE_OLD_NEW:
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::PG_DEFAULT:
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::PG_NOTHING: {
      SCHECK(
          FLAGS_cdc_enable_postgres_replica_identity, InvalidArgument,
          "Using new record types is disallowed in the middle of an upgrade. Finalize the upgrade "
          "and try again.",
          (req));
      break;
    }
    case cdc::CDCRecordType::ALL:
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::FULL_ROW_NEW_IMAGE:
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::MODIFIED_COLUMNS_OLD_AND_NEW_IMAGES:
      // TODO(#19930): Disallow older record types once we have disallowed the YSQL CDC commands in
      // yb-admin.
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::CHANGE: {
      return Status::OK();
    }
  }

  return Status::OK();
}

std::vector<TableInfoPtr> CatalogManager::FindAllTablesForCDCSDK(const NamespaceId& ns_id) {
  std::vector<TableInfoPtr> tables;

  for (const auto& table_info : tables_->GetAllTables()) {
    Schema schema;
    {
      auto ltm = table_info->LockForRead();
      if (!ltm->visible_to_client()) {
        continue;
      }
      if (ltm->namespace_id() != ns_id) {
        continue;
      }

      auto status = SchemaFromPB(ltm->schema(), &schema);
      if (!status.ok()) {
        LOG_WITH_FUNC(WARNING) << "Error while getting schema for table: " << table_info->name();
        continue;
      }
    }

    if (!IsTableEligibleForCDCSDKStream(table_info.get(), schema)) {
      continue;
    }

    tables.push_back(table_info);
  }

  return tables;
}

bool CatalogManager::IsTableEligibleForCDCSDKStream(
    const TableInfoPtr& table_info, const std::optional<Schema>& schema) const {
  if (schema.has_value()) {
    bool has_pk = true;
    for (const auto& col : schema->columns()) {
      if (col.order() == static_cast<int32_t>(PgSystemAttrNum::kYBRowId)) {
        // ybrowid column is added for tables that don't have user-specified primary key.
        VLOG(1) << "Table: " << table_info->id()
                << ", will not be added to CDCSDK stream, since it does not have a primary key";
        has_pk = false;
        break;
      }
    }

    if (!has_pk) {
      return false;
    }

    // Allow adding user created indexes to CDC stream.
    if (FLAGS_TEST_cdcsdk_add_indexes_to_stream && IsUserIndexUnlocked(*table_info)) {
      return true;
    }
  }

  if (IsMatviewTable(*table_info)) {
    // Materialized view should not be added as they are not supported for streaming.
    return false;
  }

  if (!IsUserTableUnlocked(*table_info)) {
    // Non-user tables like indexes, system tables etc should not be added as they are not
    // supported for streaming.
    return false;
  }

  return true;
}

/*
 * Processing for relevant tables that have been added after the creation of a stream
 * This involves
 *   1) Enabling the WAL retention for the tablets of the table
 *   2) INSERTING records for the tablets of this table and each stream for which
 *      this table is relevant into the cdc_state table. This is not requirred for replication slot
 *      consumption since setting up of retention barriers and inserting state table entries is done
 *      at the time of table creation.
 *   3) Storing the replica identity of the table in the stream metadata
 */
Status CatalogManager::ProcessNewTablesForCDCSDKStreams(
    const TableStreamIdsMap& table_to_unprocessed_streams_map,
    const LeaderEpoch& epoch) {
  int32_t processed_tables = 0;
  for (const auto& [table_id, streams] : table_to_unprocessed_streams_map) {
    if (processed_tables++ >= FLAGS_cdcsdk_table_processing_limit_per_run) {
      VLOG(1) << "Reached the limit of number of newly added tables to process per iteration. Will "
                 "process the reamining tables in the next iteration.";
      break;
    }

    GetTableLocationsRequestPB req;
    GetTableLocationsResponsePB resp;
    Status s;
    req.mutable_table()->set_table_id(table_id);
    req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
    req.set_require_tablets_running(true);
    req.set_include_inactive(false);

    s = GetTableLocations(&req, &resp);

    TEST_SYNC_POINT("ProcessNewTablesForCDCSDKStreams::Start");
    if (!s.ok()) {
      if (s.IsNotFound()) {
        // The table has been deleted. We will remove the table's entry from the stream's
        // metadata.
        RemoveTableFromCDCSDKUnprocessedMap(table_id, streams.begin()->get()->namespace_id());
        VLOG(1) << "Removed table: " << table_id
                << ", from namespace_to_cdcsdk_unprocessed_table_map_ , beacuse table not found";
      } else {
        LOG(WARNING) << "Encountered error calling: 'GetTableLocations' for table: " << table_id
                      << "while trying to add tablet details to cdc_state table. Error: " << s;
      }
      continue;
    }
    if (!resp.IsInitialized()) {
      VLOG(2) << "The table: " << table_id
              << ", is not initialised yet. Will add entries for tablets to cdc_state table once "
                  "all tablets are up and running";
      continue;
    }

    // Since an entry is made to table_to_unprocessed_streams_map only when there exists a stream on
    // the namespace of dynamically created table, each table in table_to_unprocessed_streams_map
    // will have atleast one corresponding stream.
    DCHECK(!streams.empty());

    // Since for a given namespace all the streams on it can either belong to the replication slot
    // consumption model or the older (YB connector) consumption model, we check the first stream
    // for each table in table_to_unprocessed_streams_map to determine which replication model is
    // active on namespace to which the table belongs.
    bool has_replication_slot_consumption =
        !streams.front()->GetCdcsdkYsqlReplicationSlotName().empty();

    if (!FLAGS_ysql_yb_enable_replication_slot_consumption || !has_replication_slot_consumption) {
      // Set the WAL retention for this new table
      // Make asynchronous ALTER TABLE requests to do this, just as was done during stream creation
      AlterTableRequestPB alter_table_req;
      alter_table_req.mutable_table()->set_table_id(table_id);
      alter_table_req.set_wal_retention_secs(FLAGS_cdc_wal_retention_time_secs);
      AlterTableResponsePB alter_table_resp;
      s = this->AlterTable(&alter_table_req, &alter_table_resp, nullptr, epoch);
      if (!s.ok()) {
        LOG(WARNING) << "Unable to change the WAL retention time for table " << table_id;
        continue;
      }
    }

    NamespaceId namespace_id;
    bool stream_pending = false;
    Status status;
    for (const auto& stream : streams) {
      if PREDICT_FALSE (stream == nullptr) {
        LOG(WARNING) << "Could not find CDC stream: " << stream->id();
        continue;
      }

      // INSERT the required cdc_state table entries. This is not requirred for replication slot
      // consumption since setting up of retention barriers and inserting state table entries is
      // done at the time of table creation.
      if (!FLAGS_ysql_yb_enable_replication_slot_consumption ||
          !has_replication_slot_consumption) {
        const auto& tablets = resp.tablet_locations();
        std::vector<cdc::CDCStateTableEntry> entries;
        entries.reserve(tablets.size());

        for (const auto& tablet : tablets) {
          cdc::CDCStateTableEntry entry(tablet.tablet_id(), stream->StreamId());
          entry.checkpoint = OpId::Invalid();
          entry.active_time = 0;
          entry.cdc_sdk_safe_time = 0;
          entries.push_back(std::move(entry));
        }

        status = cdc_state_table_->InsertEntries(entries);

        if (!status.ok()) {
          LOG(WARNING) << "Encoutered error while trying to add tablets of table: " << table_id
                       << ", to cdc_state table for stream" << stream->id() << ": " << status;
          stream_pending = true;
          continue;
        }
      }

      auto stream_lock = stream->LockForWrite();
      if (stream_lock->is_deleting()) {
        continue;
      }

      if (stream_lock->pb.unqualified_table_id_size() > 0) {
        // Skip adding the table to qualified table list if the table_id is present in the
        // unqualified table list for the stream.
        auto table_id_itr = std::find(
            stream_lock->unqualified_table_id().begin(), stream_lock->unqualified_table_id().end(),
            table_id);
        if (table_id_itr != stream_lock->unqualified_table_id().end()) {
          continue;
        }
      }

      stream_lock.mutable_data()->pb.add_table_id(table_id);

      // Store the replica identity information of the table in the stream metadata for replication
      // slot consumption.
      if (FLAGS_ysql_yb_enable_replica_identity && has_replication_slot_consumption) {
        auto table = VERIFY_RESULT(FindTableById(table_id));
        Schema schema;
        RETURN_NOT_OK(table->GetSchema(&schema));
        PgReplicaIdentity replica_identity = schema.table_properties().replica_identity();

        stream_lock.mutable_data()->pb.mutable_replica_identity_map()->insert(
            {table_id, replica_identity});
        VLOG(1) << "Storing replica identity: " << replica_identity
                << " for table: " << table_id
                << " for stream_id: " << stream->StreamId();
      }

      // Also need to persist changes in sys catalog.
      status = sys_catalog_->Upsert(leader_ready_term(), stream);
      if (!status.ok()) {
        LOG(WARNING) << "Encountered error while trying to update sys_catalog of stream: "
                     << stream->id() << ", with table: " << table_id;
        stream_pending = true;
        continue;
      }

      // Add the table/ stream pair details to 'cdcsdk_tables_to_stream_map_', so that parent
      // tablets on which tablet split is successful will be hidden rather than deleted straight
      // away, as needed.
      {
        LockGuard lock(mutex_);
        cdcsdk_tables_to_stream_map_[table_id].insert(stream->StreamId());
      }

      stream_lock.Commit();
      LOG(INFO) << "Added tablets of table: " << table_id
                << ", to cdc_state table for stream: " << stream->id();

      namespace_id = stream->namespace_id();
    }

    // Remove processed tables from 'namespace_to_unprocessed_table_map_'.
    if (!stream_pending) {
      RemoveTableFromCDCSDKUnprocessedMap(table_id, namespace_id);
    }
  }

  return Status::OK();
}

Status CatalogManager::ValidateStreamForTableRemoval(const CDCStreamInfoPtr& stream) {
  if (stream == nullptr || stream->LockForRead()->is_deleting()) {
    return STATUS(
        NotFound, "Could not find CDC stream", MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  if (!stream->IsCDCSDKStream()) {
    return STATUS(NotSupported, "Not a CDC stream");
  }

  if (!stream->GetCdcsdkYsqlReplicationSlotName().empty()) {
    return STATUS(
        NotSupported,
        "Operation not supported on CDC streams that are associated with a replication slot");
  }

  return Status::OK();
}

Status CatalogManager::ValidateTableForRemovalFromCDCSDKStream(
    const scoped_refptr<TableInfo>& table, bool check_for_ineligibility) {
  if (table == nullptr || table->LockForRead()->is_deleting()) {
    return STATUS(NotFound, "Could not find table", MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  if (check_for_ineligibility) {
    Schema schema;
    Status status = table->GetSchema(&schema);
    if (!status.ok()) {
      return STATUS(
          InternalError, Format("Error while getting schema for table: $0", table->name()));
    }

    {
      SharedLock lock(mutex_);
      if (!IsTableEligibleForCDCSDKStream(table, schema)) {
        return STATUS(InvalidArgument, "Only allowed to remove user tables from CDC streams");
      }
    }
  }

  return Status::OK();
}

Status CatalogManager::ProcessTablesToBeRemovedFromCDCSDKStreams(
    const TableStreamIdsMap& unprocessed_tables_to_streams_map, bool non_eligible_table_cleanup,
    const LeaderEpoch& epoch) {
  int32_t removed_tables = 0;
  for (const auto& [table_id, streams] : unprocessed_tables_to_streams_map) {
    if (removed_tables >= FLAGS_cdcsdk_table_processing_limit_per_run) {
      VLOG(1) << "Reached the limit of number of tables to be removed per iteration. Will "
                 "remove the remaining tables in the next iteration.";
      break;
    }

    scoped_refptr<TableInfo> table;
    {
      SharedLock lock(mutex_);
      table = tables_->FindTableOrNull(table_id);
    }

    std::unordered_set<xrepl::StreamId> streams_successfully_processed;
    Status s = ValidateTableForRemovalFromCDCSDKStream(table, !non_eligible_table_cleanup);
    if (!s.ok()) {
      LOG(WARNING) << "Table " << table_id
                   << " not available for removal from CDC streams: " << s;
      // Table is not available for cleanup. We can remove the entry from the map.
      if (non_eligible_table_cleanup) {
        RemoveTableFromCDCSDKNonEligibleTableMap(table_id, streams.begin()->get()->namespace_id());
      } else {
        for (const auto& stream : streams) {
          streams_successfully_processed.insert(stream->StreamId());
        }
        RemoveStreamsFromUnprocessedRemovedTableMap(table_id, streams_successfully_processed);
      }
      ++removed_tables;
      continue;
    }

    // Delete the table from all streams now.
    NamespaceId namespace_id;
    Status status;
    for (const auto& stream : streams) {
      auto stream_id = stream->StreamId();
      status = ValidateStreamForTableRemoval(stream);
      if (!status.ok()) {
        LOG(WARNING) << "Stream " << stream_id << " not available for table removal: " << status;
        streams_successfully_processed.insert(stream_id);
        continue;
      }

      TEST_SYNC_POINT("ProcessTablesToBeRemovedFromCDCSDKStreams::ValidationCompleted");

      TEST_SYNC_POINT("ProcessTablesToBeRemovedFromCDCSDKStreams::StartStateTableEntryUpdate");

      if (!FLAGS_TEST_cdcsdk_skip_updating_cdc_state_entries_on_table_removal) {
        std::unordered_set<TableId> tables_in_stream_metadata;
        {
          auto stream_lock = stream->LockForRead();
          for (const auto& table_id : stream_lock->table_id()) {
            tables_in_stream_metadata.insert(table_id);
          }
        }

        // Explicitly remove the table from the set since we want to remove the tablet entries of
        // this table from the cdc state table.
        tables_in_stream_metadata.erase(table_id);
        auto result =
            UpdateCheckpointForTabletEntriesInCDCState(stream_id, tables_in_stream_metadata, table);

        if (!result.ok()) {
          LOG(WARNING)
              << "Encountered error while trying to update/delete tablets entries of table: "
              << table_id << ", from cdc_state table for stream: " << stream_id << " - " << result;
          continue;
        }
      }

      TEST_SYNC_POINT("ProcessTablesToBeRemovedFromCDCSDKStreams::StateTableEntryUpdateCompleted");

      TEST_SYNC_POINT(
          "ProcessTablesToBeRemovedFromCDCSDKStreams::StartRemovalFromQualifiedTableList");

      if (!FLAGS_TEST_cdcsdk_skip_table_removal_from_qualified_list) {
        Status status = RemoveTableFromCDCStreamMetadataAndMaps(stream, table_id, epoch);
        if (!status.ok()) {
          LOG(WARNING) << "Encountered error while trying to remove table " << table_id
                       << " from qualified table list of stream " << stream_id << " and maps. - "
                       << status;
          continue;
        }
      }

      LOG(INFO) << "Successfully removed table " << table_id
                << " from qualified table list and updated corresponding cdc_state table entries "
                   "for stream: "
                << stream_id;

      namespace_id = stream->namespace_id();
      streams_successfully_processed.insert(stream_id);
    }

    if (non_eligible_table_cleanup) {
      // Remove non_user tables from 'namespace_to_cdcsdk_non_user_table_map_'.
      if (streams_successfully_processed.size() == streams.size()) {
        RemoveTableFromCDCSDKNonEligibleTableMap(table_id, namespace_id);
      }
    } else {
      // Remove streams for the table from 'cdcsdk_unprocessed_unqualified_tables_to_streams_' map.
      RemoveStreamsFromUnprocessedRemovedTableMap(table_id, streams_successfully_processed);
    }

    ++removed_tables;
  }

  return Status::OK();
}

Status CatalogManager::AddTableForRemovalFromCDCSDKStream(
    const std::unordered_set<TableId>& table_ids, const CDCStreamInfoPtr& stream) {
  std::unordered_set<TableId> tables_added_to_unqualified_list;
  auto ltm = stream->LockForWrite();
  for (const auto& table_id : table_ids) {
    auto itr =
        std::find(ltm->unqualified_table_id().begin(), ltm->unqualified_table_id().end(), table_id);
    if (itr == ltm->unqualified_table_id().end()) {
      tables_added_to_unqualified_list.insert(table_id);
      ltm.mutable_data()->pb.add_unqualified_table_id(table_id);
    }
  }

  if (tables_added_to_unqualified_list.empty()) {
    return Status::OK();
  }

  RETURN_ACTION_NOT_OK(
      sys_catalog_->Upsert(leader_ready_term(), stream), "Updating CDC stream in system catalog");

  ltm.Commit();

  {
    LockGuard lock(cdcsdk_unqualified_table_removal_mutex_);
    for (const auto& table_id : tables_added_to_unqualified_list) {
      cdcsdk_unprocessed_unqualified_tables_to_streams_[table_id].insert(stream->StreamId());
      VLOG(1) << "Added table: " << table_id << " under stream: " << stream->StreamId()
              << ", to cdcsdk_unprocessed_unqualified_tables_to_streams_ for removal from the "
                 "stream.";
    }
  }

  return Status::OK();
}

void CatalogManager::RemoveTableFromCDCSDKUnprocessedMap(
    const TableId& table_id, const NamespaceId& ns_id) {
  LockGuard lock(cdcsdk_unprocessed_table_mutex_);
  auto unprocessed_tables = FindOrNull(namespace_to_cdcsdk_unprocessed_table_map_, ns_id);
  if (unprocessed_tables) {
    unprocessed_tables->erase(table_id);
    if (unprocessed_tables->empty()) {
      namespace_to_cdcsdk_unprocessed_table_map_.erase(ns_id);
    }
  }
}

void CatalogManager::RemoveTableFromCDCSDKNonEligibleTableMap(
    const TableId& table_id, const NamespaceId& ns_id) {
  LockGuard lock(cdcsdk_non_eligible_table_mutex_);
  auto non_user_tables = FindOrNull(namespace_to_cdcsdk_non_eligible_table_map_, ns_id);
  if (!non_user_tables) {
    return;
  }

  non_user_tables->erase(table_id);
  if (non_user_tables->empty()) {
    namespace_to_cdcsdk_non_eligible_table_map_.erase(ns_id);
  }
}

void CatalogManager::RemoveStreamsFromUnprocessedRemovedTableMap(
    const TableId& table_id, const std::unordered_set<xrepl::StreamId>& stream_ids) {
  LockGuard lock(cdcsdk_unqualified_table_removal_mutex_);
  auto streams = FindOrNull(cdcsdk_unprocessed_unqualified_tables_to_streams_, table_id);
  if (!streams) {
    return;
  }

  for (const auto& stream_id : stream_ids) {
    streams->erase(stream_id);
  }
  if (streams->empty()) {
    cdcsdk_unprocessed_unqualified_tables_to_streams_.erase(table_id);
  }
}

Result<std::vector<CDCStreamInfoPtr>> CatalogManager::FindXReplStreamsMarkedForDeletion(
    SysCDCStreamEntryPB::State deletion_state) {
  std::vector<CDCStreamInfoPtr> streams;
  TRACE("Acquired catalog manager lock");
  SharedLock lock(mutex_);
  for (const CDCStreamInfoMap::value_type& entry : cdc_stream_map_) {
    auto ltm = entry.second->LockForRead();
    if (deletion_state == SysCDCStreamEntryPB::DELETING_METADATA && ltm->is_deleting_metadata()) {
      LOG(INFO) << "Stream " << entry.second->id() << " was marked as DELETING_METADATA";
      streams.push_back(entry.second);
    } else if (deletion_state == SysCDCStreamEntryPB::DELETING && ltm->is_deleting()) {
      LOG(INFO) << "Stream " << entry.second->id() << " was marked as DELETING";
      streams.push_back(entry.second);
    }
  }
  return streams;
}

Status CatalogManager::GetDroppedTablesFromCDCSDKStream(
    const std::unordered_set<TableId>& table_ids,
    std::set<TabletId>* tablets_with_streams, std::set<TableId>* dropped_tables) {
  for (const auto& table_id : table_ids) {
    TabletInfos tablets;
    scoped_refptr<TableInfo> table;
    {
      TRACE("Acquired catalog manager lock");
      SharedLock lock(mutex_);
      table = tables_->FindTableOrNull(table_id);
    }
    // GetTablets locks lock_ in shared mode.
    if (table) {
      tablets = table->GetTablets(IncludeInactive::kTrue);
    }

    // For the table dropped, GetTablets() will be empty.
    // For all other tables, GetTablets() will be non-empty.
    for (const auto& tablet : tablets) {
      tablets_with_streams->insert(tablet->tablet_id());
    }

    if (tablets.size() == 0) {
      dropped_tables->insert(table_id);
    }
  }

  return Status::OK();
}

Status CatalogManager::GetValidTabletsAndDroppedTablesForStream(
    const CDCStreamInfoPtr stream, std::set<TabletId>* tablets_with_streams,
    std::set<TableId>* dropped_tables) {
  std::unordered_set<TableId> qualified_tables;
  std::unordered_set<TableId> unqualified_tables;
  {
    auto stream_lock = stream->LockForRead();
    for (const auto& table_id : stream_lock->table_id()) {
      qualified_tables.insert(table_id);
    }

    if (stream_lock->pb.unqualified_table_id_size() > 0) {
      for (const auto& table_id : stream_lock->unqualified_table_id()) {
        unqualified_tables.insert(table_id);
      }
    }
  }

  RETURN_NOT_OK(
      GetDroppedTablesFromCDCSDKStream(qualified_tables, tablets_with_streams, dropped_tables));

  if (!unqualified_tables.empty()) {
    RETURN_NOT_OK(
        GetDroppedTablesFromCDCSDKStream(unqualified_tables, tablets_with_streams, dropped_tables));
  }

  return Status::OK();
}

Result<CDCStreamInfoPtr> CatalogManager::GetXReplStreamInfo(const xrepl::StreamId& stream_id) {
  SharedLock lock(mutex_);
  auto stream_info = FindPtrOrNull(cdc_stream_map_, stream_id);
  SCHECK(stream_info, NotFound, Format("XRepl Stream $0 not found", stream_id));
  return stream_info;
}

Status CatalogManager::CleanupCDCSDKDroppedTablesFromStreamInfo(
    const LeaderEpoch& epoch,
    const StreamTablesMap& drop_stream_tablelist) {
  std::vector<CDCStreamInfoPtr> streams_to_update;
  std::vector<CDCStreamInfo::WriteLock> locks;

  TRACE("Cleaning CDCSDK streams from map and system catalog.");
  {
    for (auto& [stream_id, drop_table_list] : drop_stream_tablelist) {
      auto cdc_stream_info = VERIFY_RESULT(GetXReplStreamInfo(stream_id));
      auto ltm = cdc_stream_info->LockForWrite();
      bool need_to_update_stream = false;

      // Remove those tables info, that are dropped from the cdc_stream_map_ and update the
      // system catalog.
      for (auto table_id : drop_table_list) {
        auto table_id_iter = std::find(ltm->table_id().begin(), ltm->table_id().end(), table_id);
        if (table_id_iter != ltm->table_id().end()) {
          need_to_update_stream = true;
          ltm.mutable_data()->pb.mutable_table_id()->erase(table_id_iter);
          ltm.mutable_data()->pb.mutable_replica_identity_map()->erase(table_id);
        }

        if (ltm->pb.unqualified_table_id_size() > 0) {
          auto unqualified_table_id_iter = std::find(
              ltm->unqualified_table_id().begin(), ltm->unqualified_table_id().end(), table_id);
          if (unqualified_table_id_iter != ltm->unqualified_table_id().end()) {
            need_to_update_stream = true;
            ltm.mutable_data()->pb.mutable_unqualified_table_id()->erase(unqualified_table_id_iter);
            ltm.mutable_data()->pb.mutable_replica_identity_map()->erase(table_id);
          }
        }
      }
      if (need_to_update_stream) {
        streams_to_update.push_back(cdc_stream_info);
        locks.push_back(std::move(ltm));
      }
    }
    // Return if there are no stream to update.
    if (streams_to_update.size() == 0) {
      return Status::OK();
    }
  }

  // Do system catalog UPDATE and DELETE based on the streams_to_update and streams_to_delete.
  RETURN_ACTION_NOT_OK(
      sys_catalog_->Upsert(epoch, streams_to_update),
      "Updating CDC streams in system catalog");

  for (auto& lock : locks) {
    lock.Commit();
  }
  return Status::OK();
}

Status CatalogManager::CleanUpCDCSDKStreamsMetadata(const LeaderEpoch& epoch) {
  // DELETING_METADATA special state is used by CDCSDK, to do CDCSDK streams metadata cleanup from
  // cache as well as from the system catalog for the drop table scenario.
  auto streams =
      VERIFY_RESULT(FindXReplStreamsMarkedForDeletion(SysCDCStreamEntryPB::DELETING_METADATA));
  if (streams.empty()) {
    return Status::OK();
  }

  TEST_SYNC_POINT("CleanUpCDCStreamMetadata::StartStep1");
  // for efficient filtering of cdc_state table entries to only the list received in streams.
  std::unordered_set<xrepl::StreamId> stream_ids_metadata_to_be_cleaned_up;
  for(const auto& stream : streams) {
    stream_ids_metadata_to_be_cleaned_up.insert(stream->StreamId());
  }
  // Step-1: Get entries from cdc_state table.
  std::vector<cdc::CDCStateTableKey> cdc_state_entries;
  Status iteration_status;
  auto all_entry_keys =
      VERIFY_RESULT(cdc_state_table_->GetTableRange({} /* just key columns */, &iteration_status));
  for (const auto& entry_result : all_entry_keys) {
    RETURN_NOT_OK(entry_result);
    const auto& entry = *entry_result;
    // Only add those entries that belong to the received list of streams and does not represent the
    // replication slot's state table entry. Replication slot's entry is skipped in order to avoid
    // its deletion since it does not represent a real tablet_id and the cleanup algorithm works
    // under the assumption that all cdc state entires are representing real tablet_ids.
    if (entry.key.tablet_id != kCDCSDKSlotEntryTabletId &&
        stream_ids_metadata_to_be_cleaned_up.contains(entry.key.stream_id)) {
      cdc_state_entries.emplace_back(entry.key);
    }
  }
  RETURN_NOT_OK(iteration_status);
  TEST_SYNC_POINT("CleanUpCDCStreamMetadata::CompletedStep1");

  TEST_SYNC_POINT("CleanUpCDCStreamMetadata::StartStep2");
  // Step-2: Get list of tablets to keep for each stream.
  // Map of valid tablets to keep for each stream.
  std::unordered_map<xrepl::StreamId, std::set<TabletId>> tablets_to_keep_per_stream;
  // Map to identify the list of dropped tables for the stream.
  StreamTablesMap drop_stream_table_list;
  for (const auto& stream : streams) {
    const auto& stream_id = stream->StreamId();
    // Get the set of all tablets not associated with the table dropped. Tablets belonging to this
    // set will not be deleted from cdc_state.
    // The second set consists of all the tables that were associated with the stream, but dropped.
    RETURN_NOT_OK(GetValidTabletsAndDroppedTablesForStream(
        stream, &tablets_to_keep_per_stream[stream_id], &drop_stream_table_list[stream_id]));
  }

  std::vector<cdc::CDCStateTableKey> keys_to_delete;
  for(const auto& entry : cdc_state_entries) {
    const auto tablets = FindOrNull(tablets_to_keep_per_stream, entry.stream_id);

    RSTATUS_DCHECK(tablets, IllegalState,
      "No entry found in tablets_to_keep_per_stream map for the stream");

    if (!tablets->contains(entry.tablet_id)) {
      // Either this tablet belongs to a dropped table or a dynamic table. If the corresponding
      // table of the tablet (all the tables in case of a colocated tablet) belongs to the dropped
      // table list computed previously, then the cdc_state entry will be deleted. If the tablet
      // itself is not found, we can safely delete the cdc_state entry.
      auto tablet_info_result = GetTabletInfo(entry.tablet_id);
      if (!tablet_info_result.ok()) {
        LOG_WITH_FUNC(WARNING) << "Did not find tablet info for tablet_id: " << entry.tablet_id
                               << " , will not delete its cdc_state entry for stream id:"
                               << entry.stream_id << "in this iteration";
        continue;
      }

      auto table_ids = (*tablet_info_result)->GetTableIds();
      DCHECK_GT(table_ids.size(), 0);
      bool all_tables_on_tablet_dropped = true;
      for (const auto& table_id : table_ids) {
        if (drop_stream_table_list[entry.stream_id].contains(table_id)) {
          continue;
        }
        // In a race scenario between cleanup of non eligible tables from CDC stream & same table
        // being dropped, removal of cdc state entries for this non eligible table is done via drop
        // table metadata cleanup flow.
        auto table_info = GetTableInfo(table_id);
        if (table_info) {
          Schema schema;
          Status status = table_info->GetSchema(&schema);
          if (!status.ok()) {
            LOG_WITH_FUNC(WARNING) << "Error while getting schema for table: " << table_info->name()
                                   << ", status: " << status.ToString();
            continue;
          }

          {
            SharedLock lock(mutex_);
            if (!IsTableEligibleForCDCSDKStream(table_info, schema)) {
              continue;
            }
          }
        }
        all_tables_on_tablet_dropped = false;
        break;
      }

      // Skip deleting the cdc state table entry if atleast one table is still present.
      if (all_tables_on_tablet_dropped) {
        keys_to_delete.emplace_back(entry.tablet_id, entry.stream_id);
      }
    }
  }

  if (!keys_to_delete.empty()) {
    LOG(INFO) << "Deleting cdc_state table entries " << AsString(keys_to_delete);
    RETURN_NOT_OK(cdc_state_table_->DeleteEntries(keys_to_delete));
  }

  // Cleanup the streams from system catalog and from internal maps.
  return CleanupCDCSDKDroppedTablesFromStreamInfo(epoch, drop_stream_table_list);
}

Status CatalogManager::CleanUpDeletedXReplStreams(const LeaderEpoch& epoch) {
  auto streams = VERIFY_RESULT(FindXReplStreamsMarkedForDeletion(SysCDCStreamEntryPB::DELETING));
  if (streams.empty()) {
    return Status::OK();
  }

  // First. For each deleted stream, delete the cdc state rows.
  // Delete all the entries in cdc_state table that contain all the deleted cdc streams.

  // We only want to iterate through cdc_state once, so create a map here to efficiently check if
  // a row belongs to a stream that should be deleted.
  std::unordered_map<xrepl::StreamId, CDCStreamInfo*> stream_id_to_stream_info_map;
  for (const auto& stream : streams) {
    stream_id_to_stream_info_map.emplace(stream->StreamId(), stream.get());
  }

  // We use GetTableRangeAsync here since it could be that we came here to rollback a CDCSDK stream
  // with the CDC state table creation still in progress. This can happen in case the stream being
  // rolled back is the first CDC stream in the universe. In this case, we skip the rollback and the
  // caller (CatalogManagerBgTasks) is expected to retry this cleanup at a later time.
  Status iteration_status;
  auto all_entry_keys = VERIFY_RESULT(
      cdc_state_table_->GetTableRangeAsync({} /* just key columns */, &iteration_status));
  std::vector<cdc::CDCStateTableKey> entries_to_delete;
  std::vector<cdc::CDCStateTableEntry> entries_to_update;

  // Remove all entries from cdc_state with the given stream ids.
  for (const auto& entry_result : all_entry_keys) {
    RETURN_NOT_OK(entry_result);
    const auto& entry = *entry_result;
    const auto stream = FindPtrOrNull(stream_id_to_stream_info_map, entry.key.stream_id);
    if (!stream) {
      continue;
    }

    if (!stream->namespace_id().empty()) {
      // CDCSDK stream.
      cdc::CDCStateTableEntry update_entry(entry.key);
      update_entry.checkpoint = OpId::Max();
      entries_to_update.emplace_back(std::move(update_entry));
      LOG(INFO) << "Setting checkpoint to OpId::Max() for CDCSDK stream " << entry.key.ToString();
    } else {
      // XCluster stream.
      entries_to_delete.emplace_back(entry.key);
      LOG(INFO) << "Deleting xCluster stream " << entry.key.ToString();
    }
  }
  RETURN_NOT_OK(iteration_status);

  RETURN_NOT_OK_PREPEND(
      cdc_state_table_->UpdateEntries(entries_to_update),
      "Error setting checkpoint to OpId::Max() in cdc_state table");

  RETURN_NOT_OK_PREPEND(
      cdc_state_table_->DeleteEntries(entries_to_delete),
      "Error deleting XRepl stream rows from cdc_state table");

  std::vector<CDCStreamInfo::WriteLock> locks;
  locks.reserve(streams.size());
  std::vector<CDCStreamInfo*> streams_to_delete;
  streams_to_delete.reserve(streams.size());

  for (auto& stream : streams) {
    locks.push_back(stream->LockForWrite());
    streams_to_delete.push_back(stream.get());
  }

  RETURN_NOT_OK(xcluster_manager_->RemoveStreamsFromSysCatalog(epoch, streams_to_delete));

  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Delete(epoch, streams_to_delete),
      "Error deleting XRepl streams from sys-catalog");

  TRACE("Removing from maps");
  {
    LockGuard lock(mutex_);
    for (const auto& stream : streams_to_delete) {
      RETURN_NOT_OK(CleanupXReplStreamFromMaps(stream));
    }
  }
  LOG(INFO) << "Successfully deleted XRepl streams: " << CDCStreamInfosAsString(streams_to_delete);

  for (auto& lock : locks) {
    lock.Commit();
  }
  return Status::OK();
}

Status CatalogManager::CleanupXReplStreamFromMaps(CDCStreamInfoPtr stream) {
  const auto& stream_id = stream->StreamId();
  if (cdc_stream_map_.erase(stream_id) < 1) {
    return STATUS(IllegalState, "XRepl stream not found in map", stream_id.ToString());
  }

  xcluster_manager_->CleanupStreamFromMaps(*stream);

  for (auto& id : stream->table_id()) {
    cdcsdk_tables_to_stream_map_[id].erase(stream_id);
  }

  // Delete entry from cdcsdk_replication_slots_to_stream_map_ if the map contains the same
  // stream_id for the replication_slot_name key.
  // It can contain a different stream_id in scenarios where a CreateCDCStream with same
  // replication slot name was immediately invoked after DeleteCDCStream before the background
  // cleanup task was executed.
  auto cdcsdk_ysql_replication_slot_name = stream->GetCdcsdkYsqlReplicationSlotName();
  if (!cdcsdk_ysql_replication_slot_name.empty() &&
      cdcsdk_replication_slots_to_stream_map_.contains(cdcsdk_ysql_replication_slot_name) &&
      cdcsdk_replication_slots_to_stream_map_.at(cdcsdk_ysql_replication_slot_name) == stream_id) {
    cdcsdk_replication_slots_to_stream_map_.erase(cdcsdk_ysql_replication_slot_name);
  }

  RecoverXreplStreamId(stream_id);

  return Status::OK();
}

Status CatalogManager::GetCDCStream(
    const GetCDCStreamRequestPB* req, GetCDCStreamResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "GetCDCStream from " << RequestorString(rpc) << ": " << req->DebugString();

  if (!req->has_stream_id() && !req->has_cdcsdk_ysql_replication_slot_name()) {
    return STATUS(
        InvalidArgument, "One of CDC Stream ID or Replication slot name must be provided",
        req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    xrepl::StreamId stream_id = xrepl::StreamId::Nil();
    if (req->has_stream_id()) {
      stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));
    } else {
      auto replication_slot_name = ReplicationSlotName(req->cdcsdk_ysql_replication_slot_name());
      if (!cdcsdk_replication_slots_to_stream_map_.contains(replication_slot_name)) {
        LOG_WITH_FUNC(WARNING) << "Did not find replication_slot_name: " << replication_slot_name
                     << " in cdcsdk_replication_slots_to_stream_map_.";
        return STATUS(
              NotFound, "Could not find CDC stream", req->ShortDebugString(),
            MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
      }
      stream_id = cdcsdk_replication_slots_to_stream_map_.at(replication_slot_name);
    }

      stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  }

  if (stream == nullptr || stream->LockForRead()->is_deleting()) {
    return STATUS(
        NotFound, "Could not find CDC stream", req->ShortDebugString(),
        MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  auto stream_lock = stream->LockForRead();

  CDCStreamInfoPB* stream_info = resp->mutable_stream();

  stream_info->set_stream_id(stream->id());
  std::string id_type_option_value(cdc::kTableId);

  for (auto option : stream_lock->options()) {
    if (option.has_key() && option.key() == cdc::kIdType) id_type_option_value = option.value();
  }

  if (id_type_option_value == cdc::kNamespaceId) {
    stream_info->set_namespace_id(stream_lock->namespace_id());
  }

  for (auto& table_id : stream_lock->table_id()) {
    stream_info->add_table_id(table_id);
  }

  if (stream_lock->pb.unqualified_table_id_size() > 0) {
    // Only applicable for CDCSDK streams.
    for (auto& table_id : stream_lock->unqualified_table_id()) {
      stream_info->add_unqualified_table_id(table_id);
    }
  }

  stream_info->mutable_options()->CopyFrom(stream_lock->options());
  stream_info->set_transactional(stream_lock->transactional());

  if (stream_lock->pb.has_state()) {
    auto state_option = stream_info->add_options();
    state_option->set_key(cdc::kStreamState);
    state_option->set_value(SysCDCStreamEntryPB::State_Name(stream_lock->pb.state()));
  }

  if (stream_lock->pb.has_cdcsdk_ysql_replication_slot_name()) {
    stream_info->set_cdcsdk_ysql_replication_slot_name(
        stream_lock->pb.cdcsdk_ysql_replication_slot_name());
  }

  if (stream_lock->pb.has_cdcsdk_ysql_replication_slot_plugin_name()) {
    stream_info->set_cdcsdk_ysql_replication_slot_plugin_name(
        stream_lock->pb.cdcsdk_ysql_replication_slot_plugin_name());
  }

  if (stream_lock->pb.has_cdcsdk_stream_metadata()) {
    auto cdcsdk_stream_metadata = stream_lock->pb.cdcsdk_stream_metadata();
    if (cdcsdk_stream_metadata.has_snapshot_time()) {
      stream_info->set_cdcsdk_consistent_snapshot_time(cdcsdk_stream_metadata.snapshot_time());
    }
    if (cdcsdk_stream_metadata.has_consistent_snapshot_option()) {
      stream_info->set_cdcsdk_consistent_snapshot_option(
          cdcsdk_stream_metadata.consistent_snapshot_option());
    }
  }

  if (stream_lock->pb.has_stream_creation_time()) {
    stream_info->set_stream_creation_time(stream_lock->pb.stream_creation_time());
  }

  if (FLAGS_cdcsdk_enable_dynamic_tables_disable_option &&
      stream_lock->pb.has_cdcsdk_disable_dynamic_table_addition()) {
    stream_info->set_cdcsdk_disable_dynamic_table_addition(
        stream_lock->pb.cdcsdk_disable_dynamic_table_addition());
  }

  auto replica_identity_map = stream_lock->pb.replica_identity_map();
  stream_info->mutable_replica_identity_map()->swap(replica_identity_map);

  return Status::OK();
}

Status CatalogManager::GetCDCDBStreamInfo(
    const GetCDCDBStreamInfoRequestPB* req, GetCDCDBStreamInfoResponsePB* resp) {
  if (!req->has_db_stream_id()) {
    return STATUS(
        InvalidArgument, "CDC DB Stream ID must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    stream = FindPtrOrNull(
        cdc_stream_map_, VERIFY_RESULT(xrepl::StreamId::FromString(req->db_stream_id())));
  }

  if (stream == nullptr || stream->LockForRead()->is_deleting()) {
    return STATUS(
        NotFound, "Could not find CDC stream", req->ShortDebugString(),
        MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  auto stream_lock = stream->LockForRead();

  if (!stream->namespace_id().empty()) {
    resp->set_namespace_id(stream->namespace_id());
  }

  for (const auto& table_id : stream_lock->table_id()) {
    const auto table_info = resp->add_table_info();
    table_info->set_stream_id(req->db_stream_id());
    table_info->set_table_id(table_id);
  }

  if (stream_lock->pb.unqualified_table_id_size() > 0) {
    for (const auto& unqualified_table_id : stream_lock->unqualified_table_id()) {
      const auto unqualified_table_info = resp->add_unqualified_table_info();
      unqualified_table_info->set_stream_id(req->db_stream_id());
      unqualified_table_info->set_table_id(unqualified_table_id);
    }
  }

  return Status::OK();
}

Status CatalogManager::ListCDCStreams(
    const ListCDCStreamsRequestPB* req, ListCDCStreamsResponsePB* resp) {
  scoped_refptr<TableInfo> table;
  bool filter_table = req->has_table_id();
  if (filter_table) {
    table = VERIFY_RESULT(FindTableById(req->table_id()));
  }

  SharedLock lock(mutex_);
  for (const CDCStreamInfoMap::value_type& entry : cdc_stream_map_) {
    bool skip_stream = false;
    bool id_type_option_present = false;

    // if the request is to list the DB streams of a specific namespace then the other namespaces
    // should not be considered
    if (req->has_namespace_id() && (req->namespace_id() != entry.second->namespace_id())) {
      continue;
    }

    if (filter_table && entry.second->table_id().size() > 0 &&
        table->id() != entry.second->table_id().Get(0)) {
      continue;  // Skip deleting/deleted streams and streams from other tables.
    }

    auto ltm = entry.second->LockForRead();

    if (ltm->is_deleting()) {
      continue;
    }

    for (const auto& option : ltm->options()) {
      if (option.key() == cdc::kIdType) {
        id_type_option_present = true;
        if (req->has_id_type()) {
          if (req->id_type() == IdTypePB::NAMESPACE_ID && option.value() != cdc::kNamespaceId) {
            skip_stream = true;
            break;
          }
          if (req->id_type() == IdTypePB::TABLE_ID && option.value() == cdc::kNamespaceId) {
            skip_stream = true;
            break;
          }
        }
      }
    }

    if ((!id_type_option_present && req->id_type() == IdTypePB::NAMESPACE_ID) || skip_stream)
      continue;

    CDCStreamInfoPB* stream = resp->add_streams();
    stream->set_stream_id(entry.second->id());
    for (const auto& table_id : ltm->table_id()) {
      stream->add_table_id(table_id);
    }

    if (ltm->pb.unqualified_table_id_size() > 0) {
      // Only applicable for CDCSDK streams.
      for (const auto& table_id : ltm->unqualified_table_id()) {
        stream->add_unqualified_table_id(table_id);
      }
    }

    stream->mutable_options()->CopyFrom(ltm->options());
    // Also add an option for the current state.
    if (ltm->pb.has_state()) {
      auto state_option = stream->add_options();
      state_option->set_key(cdc::kStreamState);
      state_option->set_value(master::SysCDCStreamEntryPB::State_Name(ltm->pb.state()));
    }

    if (ltm->pb.has_namespace_id()) {
      stream->set_namespace_id(ltm->pb.namespace_id());
    }

    if (ltm->pb.has_cdcsdk_ysql_replication_slot_name()) {
      stream->set_cdcsdk_ysql_replication_slot_name(
        ltm->pb.cdcsdk_ysql_replication_slot_name());
    }

    if (ltm->pb.has_cdcsdk_ysql_replication_slot_plugin_name()) {
      stream->set_cdcsdk_ysql_replication_slot_plugin_name(
          ltm->pb.cdcsdk_ysql_replication_slot_plugin_name());
    }

    if (ltm->pb.has_cdcsdk_stream_metadata()) {
      auto cdcsdk_stream_metadata = ltm->pb.cdcsdk_stream_metadata();
      if (cdcsdk_stream_metadata.has_snapshot_time()) {
        stream->set_cdcsdk_consistent_snapshot_time(cdcsdk_stream_metadata.snapshot_time());
      }
      if (cdcsdk_stream_metadata.has_consistent_snapshot_option()) {
        stream->set_cdcsdk_consistent_snapshot_option(
            cdcsdk_stream_metadata.consistent_snapshot_option());
      }
    }

    if (ltm->pb.has_stream_creation_time()) {
      stream->set_stream_creation_time(ltm->pb.stream_creation_time());
    }

    if (FLAGS_cdcsdk_enable_dynamic_tables_disable_option &&
        ltm->pb.has_cdcsdk_disable_dynamic_table_addition()) {
      stream->set_cdcsdk_disable_dynamic_table_addition(
          ltm->pb.cdcsdk_disable_dynamic_table_addition());
    }
  }
  return Status::OK();
}

Status CatalogManager::IsObjectPartOfXRepl(
    const IsObjectPartOfXReplRequestPB* req, IsObjectPartOfXReplResponsePB* resp) {
  auto table_info = GetTableInfo(req->table_id());
  SCHECK(table_info, NotFound, "Table with id $0 does not exist", req->table_id());
  SharedLock lock(mutex_);
  resp->set_is_object_part_of_xrepl(
      IsTablePartOfXClusterUnlocked(table_info->id()) || IsTablePartOfCDCSDK(table_info->id()));
  return Status::OK();
}

Status CatalogManager::UpdateCDCStreams(
    const std::vector<xrepl::StreamId>& stream_ids,
    const std::vector<yb::master::SysCDCStreamEntryPB>& update_entries) {
  RSTATUS_DCHECK(stream_ids.size() > 0, InvalidArgument, "No stream ID provided.");
  RSTATUS_DCHECK(
      stream_ids.size() == update_entries.size(), InvalidArgument,
      "Mismatched number of stream IDs and update entries provided.");

  // Map StreamId to (CDCStreamInfo, SysCDCStreamEntryPB). StreamId is sorted in
  // increasing order in the map.
  std::map<xrepl::StreamId, std::pair<CDCStreamInfoPtr, yb::master::SysCDCStreamEntryPB>>
      id_to_update_infos;
  {
    SharedLock lock(mutex_);
    for (size_t i = 0; i < stream_ids.size(); i++) {
      auto stream_id = stream_ids[i];
      auto entry = update_entries[i];
      auto stream = FindPtrOrNull(cdc_stream_map_, stream_id);
      if (stream == nullptr) {
        return STATUS(
            NotFound, "Could not find CDC stream", stream_id.ToString(),
            MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
      }
      id_to_update_infos[stream_id] = {stream, entry};
    }
  }

  // Acquire CDCStreamInfo::WriteLock in increasing order of xrepl::StreamId to avoid deadlock.
  std::vector<CDCStreamInfo::WriteLock> stream_locks;
  std::vector<CDCStreamInfoPtr> streams_to_update;
  stream_locks.reserve(stream_ids.size());
  streams_to_update.reserve(stream_ids.size());
  for (const auto& [stream_id, update_info] : id_to_update_infos) {
    auto& [stream, entry] = update_info;

    stream_locks.emplace_back(stream->LockForWrite());
    auto& stream_lock = stream_locks.back();
    if (stream_lock->is_deleting()) {
      return STATUS(
          NotFound, "CDC stream has been deleted", stream->id(),
          MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
    auto& pb = stream_lock.mutable_data()->pb;
    pb.CopyFrom(entry);

    for (auto it = pb.mutable_options()->begin(); it != pb.mutable_options()->end(); ++it) {
      if (it->key() == cdc::kStreamState) {
        // State should be set only via the dedicated field.
        // This can happen because CDCStreamInfoPB stores the state in the options map whereas
        // SysCDCStreamEntryPB stores state as a separate field.
        // TODO(xrepl): Add a dedicated state field to CDCStreamInfoPB.
        LOG(WARNING) << "Ignoring cdc state option " << it->value() << " for stream " << stream_id;
        pb.mutable_options()->erase(it);
      }
    }
    streams_to_update.push_back(stream);
  }

  // First persist changes in sys catalog, then commit changes in the order of lock acquiring.
  RETURN_NOT_OK(sys_catalog_->Upsert(leader_ready_term(), streams_to_update));
  for (auto& stream_lock : stream_locks) {
    stream_lock.Commit();
  }

  return Status::OK();
}

Status CatalogManager::UpdateCDCStream(
    const UpdateCDCStreamRequestPB* req, UpdateCDCStreamResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "UpdateCDCStream from " << RequestorString(rpc) << ": " << req->DebugString();

  SCHECK(
      !FLAGS_TEST_xcluster_fail_setup_stream_update, IllegalState,
      "Test flag to fail setup stream update is set");

  std::vector<xrepl::StreamId> stream_ids;
  std::vector<yb::master::SysCDCStreamEntryPB> update_entries;
  stream_ids.reserve(req->streams_size() > 0 ? req->streams_size() : 1);
  update_entries.reserve(req->streams_size() > 0 ? req->streams_size() : 1);

  if (req->streams_size() == 0) {
    // Support backwards compatibility for single stream update.
    if (!req->has_stream_id()) {
      return STATUS(
          InvalidArgument, "Stream ID must be provided", req->ShortDebugString(),
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }
    if (!req->has_entry()) {
      return STATUS(
          InvalidArgument, "CDC Stream Entry must be provided", req->ShortDebugString(),
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }
    stream_ids.push_back(VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id())));
    update_entries.push_back(req->entry());
  } else {
    // Process batch update.
    for (const auto& stream : req->streams()) {
      stream_ids.push_back(VERIFY_RESULT(xrepl::StreamId::FromString(stream.stream_id())));
      update_entries.push_back(stream.entry());
    }
  }

  RETURN_NOT_OK(UpdateCDCStreams(stream_ids, update_entries));
  return Status::OK();
}

// Query if Bootstrapping is required for a CDC stream (e.g. Are we missing logs).
Status CatalogManager::IsBootstrapRequired(
    const IsBootstrapRequiredRequestPB* req,
    IsBootstrapRequiredResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "IsBootstrapRequired from " << RequestorString(rpc) << ": " << req->DebugString();
  RSTATUS_DCHECK(req->table_ids_size() > 0, InvalidArgument, "Table ID required");
  RSTATUS_DCHECK(
      req->stream_ids_size() == 0 || req->stream_ids_size() == req->table_ids_size(),
      InvalidArgument, "Stream ID optional, but must match table IDs if specified");
  bool streams_given = req->stream_ids_size() > 0;
  CoarseTimePoint deadline = rpc->GetClientDeadline();

  // To be updated by asynchronous callbacks. All these variables are allocated on the heap
  // because we could short-circuit and go out of scope while callbacks are still on the fly.
  auto data_lock = std::make_shared<std::mutex>();
  auto table_bootstrap_required = std::make_shared<std::unordered_map<TableId, bool>>();

  // For thread joining. See XClusterAsyncPromiseCallback.
  auto promise = std::make_shared<std::promise<Status>>();
  auto future = promise->get_future();
  auto task_completed = std::make_shared<bool>(false);  // Protected by data_lock.
  auto finished_tasks = std::make_shared<size_t>(0);    // Protected by data_lock.
  size_t total_tasks = req->table_ids_size();

  for (int t = 0; t < req->table_ids_size(); t++) {
    auto table_id = req->table_ids(t);
    auto stream_id = streams_given ? VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_ids(t)))
                                   : xrepl::StreamId::Nil();

    // TODO: Submit the task to a thread pool.
    // Capture everything by value to increase their refcounts.
    scoped_refptr<Thread> async_task;
    RETURN_NOT_OK(Thread::Create(
        "xrepl_catalog_manager", "is_bootstrap_required",
        [this, table_id, stream_id, deadline, data_lock, task_completed, table_bootstrap_required,
         finished_tasks, total_tasks, promise] {
          bool bootstrap_required = false;
          auto status =
              IsTableBootstrapRequired(table_id, stream_id, deadline, &bootstrap_required);
          std::lock_guard lock(*data_lock);
          if (*task_completed) {
            return;  // Prevent calling set_value below twice.
          }
          (*table_bootstrap_required)[table_id] = bootstrap_required;
          if (!status.ok() || ++(*finished_tasks) == total_tasks) {
            // Short-circuit if error already encountered.
            *task_completed = true;
            promise->set_value(status);
          }
        },
        &async_task));
  }

  // Wait until the first promise is raised, and prepare response.
  if (future.wait_until(deadline) == std::future_status::timeout) {
    return SetupError(
        resp->mutable_error(),
        STATUS(TimedOut, "Timed out waiting for IsTableBootstrapRequired to finish"));
  }
  RETURN_NOT_OK(future.get());
  for (const auto& table_bool : *table_bootstrap_required) {
    auto new_result = resp->add_results();
    new_result->set_table_id(table_bool.first);
    new_result->set_bootstrap_required(table_bool.second);
  }
  return Status::OK();
}

Result<scoped_refptr<UniverseReplicationInfo>>
CatalogManager::CreateUniverseReplicationInfoForProducer(
    const xcluster::ReplicationGroupId& replication_group_id,
    const google::protobuf::RepeatedPtrField<HostPortPB>& master_addresses,
    const std::vector<NamespaceId>& producer_namespace_ids,
    const std::vector<NamespaceId>& consumer_namespace_ids,
    const google::protobuf::RepeatedPtrField<std::string>& table_ids, bool transactional) {
  SCHECK_EQ(
      producer_namespace_ids.size(), consumer_namespace_ids.size(), InvalidArgument,
      "We should have the namespaceIds from both producer and consumer");

  scoped_refptr<UniverseReplicationInfo> ri;
  {
    TRACE("Acquired catalog manager lock");
    SharedLock lock(mutex_);

    if (FindPtrOrNull(universe_replication_map_, replication_group_id) != nullptr) {
      return STATUS(
          AlreadyPresent, "Replication group already present", replication_group_id.ToString());
    }

    for (const auto& [universe_rg_id, universe] : universe_replication_map_) {
      for (const auto& consumer_namespace_id : consumer_namespace_ids) {
        SCHECK_FORMAT(
            !IncludesConsumerNamespace(*universe, consumer_namespace_id), AlreadyPresent,
            "Namespace $0 already included in replication group $1", consumer_namespace_id,
            universe_rg_id);
      }
    }
  }

  // Create an entry in the system catalog DocDB for this new universe replication.
  ri = new UniverseReplicationInfo(replication_group_id);
  ri->mutable_metadata()->StartMutation();
  SysUniverseReplicationEntryPB* metadata = &ri->mutable_metadata()->mutable_dirty()->pb;
  metadata->set_replication_group_id(replication_group_id.ToString());
  metadata->mutable_producer_master_addresses()->CopyFrom(master_addresses);

  if (!producer_namespace_ids.empty()) {
    auto* db_scoped_info = metadata->mutable_db_scoped_info();
    for (size_t i = 0; i < producer_namespace_ids.size(); i++) {
      auto* ns_info = db_scoped_info->mutable_namespace_infos()->Add();
      ns_info->set_producer_namespace_id(producer_namespace_ids[i]);
      ns_info->set_consumer_namespace_id(consumer_namespace_ids[i]);
    }
  }
  metadata->mutable_tables()->CopyFrom(table_ids);
  metadata->set_state(SysUniverseReplicationEntryPB::INITIALIZING);
  metadata->set_transactional(transactional);

  RETURN_NOT_OK(CheckLeaderStatus(
      sys_catalog_->Upsert(leader_ready_term(), ri),
      "inserting universe replication info into sys-catalog"));

  TRACE("Wrote universe replication info to sys-catalog");
  // Commit the in-memory state now that it's added to the persistent catalog.
  ri->mutable_metadata()->CommitMutation();
  LOG(INFO) << "Setup universe replication from producer " << ri->ToString();

  {
    LockGuard lock(mutex_);
    universe_replication_map_[ri->ReplicationGroupId()] = ri;
  }

  // Make sure the AutoFlags are compatible.
  // This is done after the replication info is persisted since it performs RPC calls to source
  // universe and we can crash during this call.
  // TODO: When new master starts it can retry this step or mark the replication group as failed.
  if (FLAGS_enable_xcluster_auto_flag_validation) {
    const auto auto_flags_config = master_->GetAutoFlagsConfig();
    auto status = ResultToStatus(GetAutoFlagConfigVersionIfCompatible(*ri, auto_flags_config));

    if (!status.ok()) {
      MarkUniverseReplicationFailed(ri, status);
      return status.CloneAndAddErrorCode(MasterError(MasterErrorPB::INVALID_REQUEST));
    }

    auto l = ri->LockForWrite();
    l.mutable_data()->pb.set_validated_local_auto_flags_config_version(
        auto_flags_config.config_version());

    RETURN_NOT_OK(CheckLeaderStatus(
        sys_catalog_->Upsert(leader_ready_term(), ri),
        "inserting universe replication info into sys-catalog"));

    l.Commit();
  }
  return ri;
}

Result<scoped_refptr<UniverseReplicationBootstrapInfo>>
CatalogManager::CreateUniverseReplicationBootstrapInfoForProducer(
    const xcluster::ReplicationGroupId& replication_group_id,
    const google::protobuf::RepeatedPtrField<HostPortPB>& master_addresses,
    const LeaderEpoch& epoch, bool transactional) {
  scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info;
  {
    TRACE("Acquired catalog manager lock");
    SharedLock lock(mutex_);

    if (FindPtrOrNull(universe_replication_bootstrap_map_, replication_group_id) != nullptr) {
      return STATUS(
          InvalidArgument, "Bootstrap already present", replication_group_id.ToString(),
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }
  }

  // Create an entry in the system catalog DocDB for this new universe replication.
  bootstrap_info = new UniverseReplicationBootstrapInfo(replication_group_id);
  bootstrap_info->mutable_metadata()->StartMutation();

  SysUniverseReplicationBootstrapEntryPB* metadata =
      &bootstrap_info->mutable_metadata()->mutable_dirty()->pb;
  metadata->set_replication_group_id(replication_group_id.ToString());
  metadata->mutable_producer_master_addresses()->CopyFrom(master_addresses);
  metadata->set_state(SysUniverseReplicationBootstrapEntryPB::INITIALIZING);
  metadata->set_transactional(transactional);
  metadata->set_leader_term(epoch.leader_term);
  metadata->set_pitr_count(epoch.pitr_count);

  RETURN_NOT_OK(CheckLeaderStatus(
      sys_catalog_->Upsert(leader_ready_term(), bootstrap_info),
      "inserting universe replication bootstrap info into sys-catalog"));

  TRACE("Wrote universe replication bootstrap info to sys-catalog");
  // Commit the in-memory state now that it's added to the persistent catalog.
  bootstrap_info->mutable_metadata()->CommitMutation();
  LOG(INFO) << "Setup universe replication bootstrap from producer " << bootstrap_info->ToString();

  {
    LockGuard lock(mutex_);
    universe_replication_bootstrap_map_[bootstrap_info->ReplicationGroupId()] = bootstrap_info;
  }
  return bootstrap_info;
}

Status CatalogManager::ValidateMasterAddressesBelongToDifferentCluster(
    const google::protobuf::RepeatedPtrField<HostPortPB>& master_addresses) {
  std::vector<ServerEntryPB> cluster_master_addresses;
  RETURN_NOT_OK(master_->ListMasters(&cluster_master_addresses));
  std::unordered_set<HostPort, HostPortHash> cluster_master_hps;

  for (const auto& cluster_elem : cluster_master_addresses) {
    if (cluster_elem.has_registration()) {
      auto p_rpc_addresses = cluster_elem.registration().private_rpc_addresses();
      for (const auto& p_rpc_elem : p_rpc_addresses) {
        cluster_master_hps.insert(HostPort::FromPB(p_rpc_elem));
      }

      auto broadcast_addresses = cluster_elem.registration().broadcast_addresses();
      for (const auto& bc_elem : broadcast_addresses) {
        cluster_master_hps.insert(HostPort::FromPB(bc_elem));
      }
    }

    for (const auto& master_address : master_addresses) {
      auto master_hp = HostPort::FromPB(master_address);
      SCHECK(
          !cluster_master_hps.contains(master_hp), InvalidArgument,
          "Master address $0 belongs to the target universe", master_hp);
    }
  }
  return Status::OK();
}

Result<SnapshotInfoPB> CatalogManager::DoReplicationBootstrapCreateSnapshot(
    const std::vector<client::YBTableName>& tables,
    scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info) {
  LOG(INFO) << Format(
      "SetupReplicationWithBootstrap: create producer snapshot for replication $0",
      bootstrap_info->id());
  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::CREATE_PRODUCER_SNAPSHOT);

  auto xcluster_rpc_tasks = VERIFY_RESULT(bootstrap_info->GetOrCreateXClusterRpcTasks(
      bootstrap_info->LockForRead()->pb.producer_master_addresses()));

  TxnSnapshotId old_snapshot_id = TxnSnapshotId::Nil();

  // Send create request and wait for completion.
  auto snapshot_result = xcluster_rpc_tasks->CreateSnapshot(tables, &old_snapshot_id);

  // If the producer failed to complete the snapshot, we still want to store the snapshot_id for
  // cleanup purposes.
  if (!old_snapshot_id.IsNil()) {
    auto l = bootstrap_info->LockForWrite();
    l.mutable_data()->set_old_snapshot_id(old_snapshot_id);

    // Update sys_catalog.
    const Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);
    l.CommitOrWarn(s, "updating universe replication bootstrap info in sys-catalog");
  }

  return snapshot_result;
}

Result<std::vector<TableMetaPB>> CatalogManager::DoReplicationBootstrapImportSnapshot(
    const SnapshotInfoPB& snapshot,
    scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info) {
  ///////////////////////////
  // ImportSnapshotMeta
  ///////////////////////////
  LOG(INFO) << Format(
      "SetupReplicationWithBootstrap: import snapshot for replication $0", bootstrap_info->id());
  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::IMPORT_SNAPSHOT);

  NamespaceMap namespace_map;
  UDTypeMap type_map;
  ExternalTableSnapshotDataMap tables_data;

  // ImportSnapshotMeta timeout should be a function of the table size.
  auto deadline = CoarseMonoClock::Now() + MonoDelta::FromSeconds(10 + 1 * tables_data.size());
  auto epoch = bootstrap_info->LockForRead()->epoch();
  RETURN_NOT_OK(DoImportSnapshotMeta(
      snapshot, epoch, std::nullopt /* clone_target_namespace_name */, &namespace_map,
      &type_map, &tables_data, deadline));

  // Update sys catalog with new information.
  {
    auto l = bootstrap_info->LockForWrite();
    l.mutable_data()->set_new_snapshot_objects(namespace_map, type_map, tables_data);

    // Update sys_catalog.
    const Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);
    l.CommitOrWarn(s, "updating universe replication bootstrap info in sys-catalog");
  }

  ///////////////////////////
  // CreateConsumerSnapshot
  ///////////////////////////
  LOG(INFO) << Format(
      "SetupReplicationWithBootstrap: create consumer snapshot for replication $0",
      bootstrap_info->id());
  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::CREATE_CONSUMER_SNAPSHOT);

  CreateSnapshotRequestPB snapshot_req;
  CreateSnapshotResponsePB snapshot_resp;

  std::vector<TableMetaPB> tables_meta;
  for (const auto& [table_id, table_data] : tables_data) {
    if (table_data.table_meta) {
      tables_meta.push_back(std::move(*table_data.table_meta));
    }
  }

  for (const auto& table_meta : tables_meta) {
    SCHECK(
        ImportSnapshotMetaResponsePB_TableType_IsValid(table_meta.table_type()), InternalError,
        Format("Found unknown table type: $0", table_meta.table_type()));

    const string& new_table_id = table_meta.table_ids().new_id();
    RETURN_NOT_OK(WaitForCreateTableToFinish(new_table_id, deadline));

    snapshot_req.mutable_tables()->Add()->set_table_id(new_table_id);
  }

  snapshot_req.set_add_indexes(false);
  snapshot_req.set_transaction_aware(true);
  snapshot_req.set_imported(true);
  RETURN_NOT_OK(CreateTransactionAwareSnapshot(snapshot_req, &snapshot_resp, deadline));

  // Update sys catalog with new information.
  {
    auto l = bootstrap_info->LockForWrite();
    l.mutable_data()->set_new_snapshot_id(TryFullyDecodeTxnSnapshotId(snapshot_resp.snapshot_id()));

    // Update sys_catalog.
    const Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);
    l.CommitOrWarn(s, "updating universe replication bootstrap info in sys-catalog");
  }

  return std::vector<TableMetaPB>(tables_meta.begin(), tables_meta.end());
}

Status CatalogManager::DoReplicationBootstrapTransferAndRestoreSnapshot(
    const std::vector<TableMetaPB>& tables_meta,
    scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info) {
  // Retrieve required data from PB.
  TxnSnapshotId old_snapshot_id = TxnSnapshotId::Nil();
  TxnSnapshotId new_snapshot_id = TxnSnapshotId::Nil();
  google::protobuf::RepeatedPtrField<HostPortPB> producer_masters;
  auto epoch = bootstrap_info->epoch();
  {
    auto l = bootstrap_info->LockForRead();
    old_snapshot_id = l->old_snapshot_id();
    new_snapshot_id = l->new_snapshot_id();
    producer_masters.CopyFrom(l->pb.producer_master_addresses());
  }

  auto xcluster_rpc_tasks =
      VERIFY_RESULT(bootstrap_info->GetOrCreateXClusterRpcTasks(producer_masters));

  // Transfer snapshot.
  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::TRANSFER_SNAPSHOT);
  auto snapshot_transfer_manager =
      std::make_shared<SnapshotTransferManager>(master_, this, xcluster_rpc_tasks->client());
  RETURN_NOT_OK_PREPEND(
      snapshot_transfer_manager->TransferSnapshot(
          old_snapshot_id, new_snapshot_id, tables_meta, epoch),
      Format("Failed to transfer snapshot $0 from producer", old_snapshot_id.ToString()));

  // Restore snapshot.
  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::RESTORE_SNAPSHOT);
  auto restoration_id = VERIFY_RESULT(
      snapshot_coordinator_.Restore(new_snapshot_id, HybridTime(), epoch.leader_term));

  if (PREDICT_FALSE(FLAGS_TEST_xcluster_fail_restore_consumer_snapshot)) {
    return STATUS(Aborted, "Test failure");
  }

  // Wait for restoration to complete.
  return WaitFor(
      [this, &new_snapshot_id, &restoration_id]() -> Result<bool> {
        ListSnapshotRestorationsResponsePB resp;
        RETURN_NOT_OK(
            snapshot_coordinator_.ListRestorations(restoration_id, new_snapshot_id, &resp));

        SCHECK_EQ(
            resp.restorations_size(), 1, IllegalState,
            Format("Expected 1 restoration, got $0", resp.restorations_size()));
        const auto& restoration = *resp.restorations().begin();
        const auto& state = restoration.entry().state();
        return state == SysSnapshotEntryPB::RESTORED;
      },
      MonoDelta::kMax, "Waiting for restoration to finish", 100ms);
}

Status CatalogManager::ValidateReplicationBootstrapRequest(
    const SetupNamespaceReplicationWithBootstrapRequestPB* req) {
  SCHECK(
      !req->replication_id().empty(), InvalidArgument, "Replication ID must be provided",
      req->ShortDebugString());

  SCHECK(
      req->producer_master_addresses_size() > 0, InvalidArgument,
      "Producer master address must be provided", req->ShortDebugString());

  {
    auto l = ClusterConfig()->LockForRead();
    SCHECK(
        l->pb.cluster_uuid() != req->replication_id(), InvalidArgument,
        "Replication name cannot be the target universe UUID", req->ShortDebugString());
  }

  RETURN_NOT_OK_PREPEND(
      ValidateMasterAddressesBelongToDifferentCluster(req->producer_master_addresses()),
      req->ShortDebugString());

  GetUniverseReplicationRequestPB universe_req;
  GetUniverseReplicationResponsePB universe_resp;
  universe_req.set_replication_group_id(req->replication_id());
  SCHECK(
      GetUniverseReplication(&universe_req, &universe_resp, /* RpcContext */ nullptr).IsNotFound(),
      InvalidArgument, Format("Can't bootstrap replication that already exists"));

  return Status::OK();
}

void CatalogManager::DoReplicationBootstrap(
    const xcluster::ReplicationGroupId& replication_id,
    const std::vector<client::YBTableName>& tables,
    Result<TableBootstrapIdsMap> bootstrap_producer_result) {
  // First get the universe.
  scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    bootstrap_info = FindPtrOrNull(universe_replication_bootstrap_map_, replication_id);
    if (bootstrap_info == nullptr) {
      LOG(ERROR) << "UniverseReplicationBootstrap not found: " << replication_id;
      return;
    }
  }

  // Verify the result from BootstrapProducer & update values in PB if successful.
  auto table_bootstrap_ids =
      VERIFY_RESULT_MARK_BOOTSTRAP_FAILED(std::move(bootstrap_producer_result));
  {
    auto l = bootstrap_info->LockForWrite();
    auto map = l.mutable_data()->pb.mutable_table_bootstrap_ids();
    for (const auto& [table_id, bootstrap_id] : table_bootstrap_ids) {
      (*map)[table_id] = bootstrap_id.ToString();
    }

    // Update sys_catalog.
    const Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);
    l.CommitOrWarn(s, "updating universe replication bootstrap info in sys-catalog");
  }

  // Create producer snapshot.
  auto snapshot = VERIFY_RESULT_MARK_BOOTSTRAP_FAILED(
      DoReplicationBootstrapCreateSnapshot(tables, bootstrap_info));

  // Import snapshot and create consumer snapshot.
  auto tables_meta = VERIFY_RESULT_MARK_BOOTSTRAP_FAILED(
      DoReplicationBootstrapImportSnapshot(snapshot, bootstrap_info));

  // Transfer and restore snapshot.
  MARK_BOOTSTRAP_FAILED_NOT_OK(
      DoReplicationBootstrapTransferAndRestoreSnapshot(tables_meta, bootstrap_info));

  // Call SetupUniverseReplication
  SetupUniverseReplicationRequestPB replication_req;
  SetupUniverseReplicationResponsePB replication_resp;
  {
    auto l = bootstrap_info->LockForRead();
    replication_req.set_replication_group_id(l->pb.replication_group_id());
    replication_req.set_transactional(l->pb.transactional());
    replication_req.mutable_producer_master_addresses()->CopyFrom(
        l->pb.producer_master_addresses());
    for (const auto& [table_id, bootstrap_id] : table_bootstrap_ids) {
      replication_req.add_producer_table_ids(table_id);
      replication_req.add_producer_bootstrap_ids(bootstrap_id.ToString());
    }
  }

  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::SETUP_REPLICATION);
  MARK_BOOTSTRAP_FAILED_NOT_OK(
      SetupUniverseReplication(&replication_req, &replication_resp, /* rpc = */ nullptr));

  LOG(INFO) << Format(
      "Successfully completed replication bootstrap for $0", replication_id.ToString());
  SetReplicationBootstrapState(bootstrap_info, SysUniverseReplicationBootstrapEntryPB::DONE);
}

/*
 * SetupNamespaceReplicationWithBootstrap is setup in 5 stages.
 * 1. Validates user input & connect to producer.
 * 2. Calls BootstrapProducer with all user tables in namespace.
 * 3. Create snapshot on producer and import onto consumer.
 * 4. Download snapshots from producer and restore on consumer.
 * 5. SetupUniverseReplication.
 */
Status CatalogManager::SetupNamespaceReplicationWithBootstrap(
    const SetupNamespaceReplicationWithBootstrapRequestPB* req,
    SetupNamespaceReplicationWithBootstrapResponsePB* resp,
    rpc::RpcContext* rpc,
    const LeaderEpoch& epoch) {
  LOG(INFO) << Format(
      "SetupNamespaceReplicationWithBootstrap from $0: $1", RequestorString(rpc),
      req->DebugString());

  // PHASE 1: Validating user input.
  RETURN_NOT_OK(ValidateReplicationBootstrapRequest(req));

  // Create entry in sys catalog.
  auto replication_id = xcluster::ReplicationGroupId(req->replication_id());
  auto transactional = req->has_transactional() ? req->transactional() : false;
  auto bootstrap_info = VERIFY_RESULT(CreateUniverseReplicationBootstrapInfoForProducer(
      replication_id, req->producer_master_addresses(), epoch, transactional));

  // Connect to producer.
  auto xcluster_rpc_result =
      bootstrap_info->GetOrCreateXClusterRpcTasks(req->producer_master_addresses());
  if (!xcluster_rpc_result.ok()) {
    auto s = ResultToStatus(xcluster_rpc_result);
    MarkReplicationBootstrapFailed(bootstrap_info, s);
    return s;
  }
  auto xcluster_rpc_tasks = std::move(*xcluster_rpc_result);

  // Get user tables in producer namespace.
  auto tables_result = xcluster_rpc_tasks->client()->ListUserTables(req->producer_namespace());
  if (!tables_result.ok()) {
    auto s = ResultToStatus(tables_result);
    MarkReplicationBootstrapFailed(bootstrap_info, s);
    return s;
  }
  auto tables = std::move(*tables_result);

  // Bootstrap producer.
  SetReplicationBootstrapState(
      bootstrap_info, SysUniverseReplicationBootstrapEntryPB::BOOTSTRAP_PRODUCER);
  auto s = xcluster_rpc_tasks->BootstrapProducer(
      req->producer_namespace(), tables,
      Bind(&CatalogManager::DoReplicationBootstrap, Unretained(this), replication_id, tables));
  if (!s.ok()) {
    MarkReplicationBootstrapFailed(bootstrap_info, s);
    return s;
  }

  return Status::OK();
}

/*
 * UniverseReplication is setup in 4 stages within the Catalog Manager
 * 1. SetupUniverseReplication: Validates user input & requests Producer schema.
 * 2. GetTableSchemaCallback:   Validates Schema compatibility & requests Producer CDC init.
 * 3. AddCDCStreamToUniverseAndInitConsumer:  Setup RPC connections for CDC Streaming
 * 4. InitXClusterConsumer:          Initializes the Consumer settings to begin tailing data
 */
Status CatalogManager::SetupUniverseReplication(
    const SetupUniverseReplicationRequestPB* req,
    SetupUniverseReplicationResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "SetupUniverseReplication from " << RequestorString(rpc) << ": "
            << req->DebugString();

  // Sanity checking section.
  if (!req->has_replication_group_id()) {
    return STATUS(
        InvalidArgument, "Producer universe ID must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  if (req->producer_master_addresses_size() <= 0) {
    return STATUS(
        InvalidArgument, "Producer master address must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  if (req->producer_bootstrap_ids().size() > 0 &&
      req->producer_bootstrap_ids().size() != req->producer_table_ids().size()) {
    return STATUS(
        InvalidArgument, "Number of bootstrap ids must be equal to number of tables",
        req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  {
    auto l = ClusterConfig()->LockForRead();
    if (l->pb.cluster_uuid() == req->replication_group_id()) {
      return STATUS(
          InvalidArgument, "The request UUID and cluster UUID are identical.",
          req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
    }
  }

  RETURN_NOT_OK_PREPEND(
      ValidateMasterAddressesBelongToDifferentCluster(req->producer_master_addresses()),
      req->ShortDebugString());

  SetupReplicationInfo setup_info;
  setup_info.transactional = req->transactional();
  auto& table_id_to_bootstrap_id = setup_info.table_bootstrap_ids;

  if (!req->producer_bootstrap_ids().empty()) {
    if (req->producer_table_ids().size() != req->producer_bootstrap_ids_size()) {
      return STATUS(
          InvalidArgument, "Bootstrap ids must be provided for all tables", req->ShortDebugString(),
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }

    table_id_to_bootstrap_id.reserve(req->producer_table_ids().size());
    for (int i = 0; i < req->producer_table_ids().size(); i++) {
      table_id_to_bootstrap_id.insert_or_assign(
          req->producer_table_ids(i),
          VERIFY_RESULT(xrepl::StreamId::FromString(req->producer_bootstrap_ids(i))));
    }
  }

  SCHECK(
      req->producer_namespaces().empty() || req->transactional(), InvalidArgument,
      "Transactional flag must be set for Db scoped replication groups");

  std::vector<NamespaceId> producer_namespace_ids, consumer_namespace_ids;
  for (const auto& producer_ns_id : req->producer_namespaces()) {
    SCHECK(!producer_ns_id.id().empty(), InvalidArgument, "Invalid Namespace Id");
    SCHECK(!producer_ns_id.name().empty(), InvalidArgument, "Invalid Namespace name");
    SCHECK_EQ(
        producer_ns_id.database_type(), YQLDatabase::YQL_DATABASE_PGSQL, InvalidArgument,
        "Invalid Namespace database_type");

    producer_namespace_ids.push_back(producer_ns_id.id());

    NamespaceIdentifierPB consumer_ns_id;
    consumer_ns_id.set_database_type(YQLDatabase::YQL_DATABASE_PGSQL);
    consumer_ns_id.set_name(producer_ns_id.name());
    auto ns_info = VERIFY_RESULT(FindNamespace(consumer_ns_id));
    consumer_namespace_ids.push_back(ns_info->id());
  }

  // We should set the universe uuid even if we fail with AlreadyPresent error.
  {
    auto universe_uuid = GetUniverseUuidIfExists();
    if (universe_uuid) {
      resp->set_universe_uuid(universe_uuid->ToString());
    }
  }

  auto ri = VERIFY_RESULT(CreateUniverseReplicationInfoForProducer(
      xcluster::ReplicationGroupId(req->replication_group_id()), req->producer_master_addresses(),
      producer_namespace_ids, consumer_namespace_ids, req->producer_table_ids(),
      setup_info.transactional));

  // Initialize the CDC Stream by querying the Producer server for RPC sanity checks.
  auto result = ri->GetOrCreateXClusterRpcTasks(req->producer_master_addresses());
  if (!result.ok()) {
    MarkUniverseReplicationFailed(ri, ResultToStatus(result));
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, result.status());
  }
  std::shared_ptr<XClusterRpcTasks> xcluster_rpc = *result;

  // For each table, run an async RPC task to verify a sufficient Producer:Consumer schema match.
  for (int i = 0; i < req->producer_table_ids_size(); i++) {
    // SETUP CONTINUES after this async call.
    Status s;
    if (IsColocatedDbParentTableId(req->producer_table_ids(i))) {
      auto tables_info = std::make_shared<std::vector<client::YBTableInfo>>();
      s = xcluster_rpc->client()->GetColocatedTabletSchemaByParentTableId(
          req->producer_table_ids(i), tables_info,
          Bind(
              &CatalogManager::GetColocatedTabletSchemaCallback, Unretained(this),
              ri->ReplicationGroupId(), tables_info, setup_info));
    } else if (IsTablegroupParentTableId(req->producer_table_ids(i))) {
      auto tablegroup_id = GetTablegroupIdFromParentTableId(req->producer_table_ids(i));
      auto tables_info = std::make_shared<std::vector<client::YBTableInfo>>();
      s = xcluster_rpc->client()->GetTablegroupSchemaById(
          tablegroup_id, tables_info,
          Bind(
              &CatalogManager::GetTablegroupSchemaCallback, Unretained(this),
              ri->ReplicationGroupId(), tables_info, tablegroup_id, setup_info));
    } else {
      auto table_info = std::make_shared<client::YBTableInfo>();
      s = xcluster_rpc->client()->GetTableSchemaById(
          req->producer_table_ids(i), table_info,
          Bind(
              &CatalogManager::GetTableSchemaCallback, Unretained(this), ri->ReplicationGroupId(),
              table_info, setup_info));
    }

    if (!s.ok()) {
      MarkUniverseReplicationFailed(ri, s);
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
    }
  }

  LOG(INFO) << "Started schema validation for universe replication " << ri->ToString();
  return Status::OK();
}

void CatalogManager::MarkUniverseReplicationFailed(
    scoped_refptr<UniverseReplicationInfo> universe, const Status& failure_status) {
  auto l = universe->LockForWrite();
  MarkUniverseReplicationFailed(failure_status, &l, universe);
}

void CatalogManager::MarkUniverseReplicationFailed(
    const Status& failure_status, CowWriteLock<PersistentUniverseReplicationInfo>* universe_lock,
    scoped_refptr<UniverseReplicationInfo> universe) {
  auto& l = *universe_lock;
  if (l->pb.state() == SysUniverseReplicationEntryPB::DELETED) {
    l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DELETED_ERROR);
  } else {
    l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::FAILED);
  }

  LOG(WARNING) << "Universe replication " << universe->ToString()
               << " failed: " << failure_status.ToString();

  universe->SetSetupUniverseReplicationErrorStatus(failure_status);

  // Update sys_catalog.
  const Status s = sys_catalog_->Upsert(leader_ready_term(), universe);

  l.CommitOrWarn(s, "updating universe replication info in sys-catalog");
}

void CatalogManager::MarkReplicationBootstrapFailed(
    scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info,
    const Status& failure_status) {
  auto l = bootstrap_info->LockForWrite();
  MarkReplicationBootstrapFailed(failure_status, &l, bootstrap_info);
}

void CatalogManager::MarkReplicationBootstrapFailed(
    const Status& failure_status,
    CowWriteLock<PersistentUniverseReplicationBootstrapInfo>* bootstrap_info_lock,
    scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info) {
  auto& l = *bootstrap_info_lock;
  auto state = l->pb.state();
  if (state == SysUniverseReplicationBootstrapEntryPB::DELETED) {
    l.mutable_data()->pb.set_state(SysUniverseReplicationBootstrapEntryPB::DELETED_ERROR);
  } else {
    l.mutable_data()->pb.set_state(SysUniverseReplicationBootstrapEntryPB::FAILED);
    l.mutable_data()->pb.set_failed_on(state);
  }

  LOG(WARNING) << Format(
      "Replication bootstrap $0 failed: $1", bootstrap_info->ToString(),
      failure_status.ToString());

  bootstrap_info->SetReplicationBootstrapErrorStatus(failure_status);

  // Update sys_catalog.
  const Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);

  l.CommitOrWarn(s, "updating universe replication bootstrap info in sys-catalog");
}

void CatalogManager::SetReplicationBootstrapState(
    scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info,
    const SysUniverseReplicationBootstrapEntryPB::State& state) {
  auto l = bootstrap_info->LockForWrite();
  l.mutable_data()->set_state(state);

  // Update sys_catalog.
  const Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);
  l.CommitOrWarn(s, "updating universe replication bootstrap info in sys-catalog");
}

Status CatalogManager::IsBootstrapRequiredOnProducer(
    scoped_refptr<UniverseReplicationInfo> universe, const TableId& producer_table,
    const std::unordered_map<TableId, xrepl::StreamId>& table_bootstrap_ids) {
  if (!FLAGS_check_bootstrap_required) {
    return Status::OK();
  }
  auto master_addresses = universe->LockForRead()->pb.producer_master_addresses();
  boost::optional<xrepl::StreamId> bootstrap_id;
  if (table_bootstrap_ids.count(producer_table) > 0) {
    bootstrap_id = table_bootstrap_ids.at(producer_table);
  }

  auto xcluster_rpc = VERIFY_RESULT(universe->GetOrCreateXClusterRpcTasks(master_addresses));
  if (VERIFY_RESULT(xcluster_rpc->client()->IsBootstrapRequired({producer_table}, bootstrap_id))) {
    return STATUS(
        IllegalState,
        Format(
            "Error Missing Data in Logs. Bootstrap is required for producer $0", universe->id()));
  }
  return Status::OK();
}

Status CatalogManager::IsTableBootstrapRequired(
    const TableId& table_id,
    const xrepl::StreamId& stream_id,
    CoarseTimePoint deadline,
    bool* const bootstrap_required) {
  scoped_refptr<TableInfo> table = VERIFY_RESULT(FindTableById(table_id));
  RSTATUS_DCHECK(table != nullptr, NotFound, "Table ID not found: " + table_id);

  // Make a batch call for IsBootstrapRequired on every relevant TServer.
  std::map<std::shared_ptr<cdc::CDCServiceProxy>, cdc::IsBootstrapRequiredRequestPB>
      proxy_to_request;
  for (const auto& tablet : table->GetTablets()) {
    auto ts = VERIFY_RESULT(tablet->GetLeader());
    std::shared_ptr<cdc::CDCServiceProxy> proxy;
    RETURN_NOT_OK(ts->GetProxy(&proxy));
    proxy_to_request[proxy].add_tablet_ids(tablet->id());
  }

  // TODO: Make the RPCs async and parallel.
  *bootstrap_required = false;
  for (auto& proxy_request : proxy_to_request) {
    auto& tablet_req = proxy_request.second;
    cdc::IsBootstrapRequiredResponsePB tablet_resp;
    rpc::RpcController rpc;
    rpc.set_deadline(deadline);
    if (stream_id) {
      tablet_req.set_stream_id(stream_id.ToString());
    }
    auto& cdc_service = proxy_request.first;

    RETURN_NOT_OK(cdc_service->IsBootstrapRequired(tablet_req, &tablet_resp, &rpc));
    if (tablet_resp.has_error()) {
      RETURN_NOT_OK(StatusFromPB(tablet_resp.error().status()));
    } else if (tablet_resp.has_bootstrap_required() && tablet_resp.bootstrap_required()) {
      *bootstrap_required = true;
      break;
    }
  }

  return Status::OK();
}

Status CatalogManager::AddValidatedTableToUniverseReplication(
    scoped_refptr<UniverseReplicationInfo> universe,
    const TableId& producer_table,
    const TableId& consumer_table,
    const SchemaVersion& producer_schema_version,
    const SchemaVersion& consumer_schema_version,
    const ColocationSchemaVersions& colocated_schema_versions) {
  auto l = universe->LockForWrite();

  auto map = l.mutable_data()->pb.mutable_validated_tables();
  (*map)[producer_table] = consumer_table;

  SchemaVersionMappingEntryPB entry;
  if (IsColocationParentTableId(consumer_table)) {
    for (const auto& [colocation_id, producer_schema_version, consumer_schema_version] :
        colocated_schema_versions) {
      auto colocated_entry = entry.add_colocated_schema_versions();
      auto colocation_mapping = colocated_entry->mutable_schema_version_mapping();
      colocated_entry->set_colocation_id(colocation_id);
      colocation_mapping->set_producer_schema_version(producer_schema_version);
      colocation_mapping->set_consumer_schema_version(consumer_schema_version);
    }
  } else {
    auto mapping = entry.mutable_schema_version_mapping();
    mapping->set_producer_schema_version(producer_schema_version);
    mapping->set_consumer_schema_version(consumer_schema_version);
  }


  auto schema_versions_map = l.mutable_data()->pb.mutable_schema_version_mappings();
  (*schema_versions_map)[producer_table] = std::move(entry);

  // TODO: end of config validation should be where SetupUniverseReplication exits back to user
  LOG(INFO) << "UpdateItem in AddValidatedTable";

  // Update sys_catalog.
  RETURN_ACTION_NOT_OK(
      sys_catalog_->Upsert(leader_ready_term(), universe),
      "updating universe replication info in sys-catalog");
  l.Commit();

  return Status::OK();
}

Status CatalogManager::CreateCdcStreamsIfReplicationValidated(
    scoped_refptr<UniverseReplicationInfo> universe,
    const std::unordered_map<TableId, xrepl::StreamId>& table_bootstrap_ids) {
  auto l = universe->LockForWrite();
  if (l->is_deleted_or_failed()) {
    // Nothing to do since universe is being deleted.
    return STATUS(Aborted, "Universe is being deleted");
  }

  auto* mutable_pb = &l.mutable_data()->pb;

  if (mutable_pb->state() != SysUniverseReplicationEntryPB::INITIALIZING) {
    VLOG_WITH_FUNC(2) << "Universe replication is in invalid state " << l->pb.state();

    // Replication stream has already been validated, or is in FAILED state which cannot be
    // recovered.
    return Status::OK();
  }

  if (mutable_pb->validated_tables_size() != mutable_pb->tables_size()) {
    // Replication stream is not yet ready. All the tables have to be validated.
    return Status::OK();
  }

  auto master_addresses = mutable_pb->producer_master_addresses();
  cdc::StreamModeTransactional transactional(mutable_pb->transactional());
  auto res = universe->GetOrCreateXClusterRpcTasks(master_addresses);
  if (!res.ok()) {
    MarkUniverseReplicationFailed(res.status(), &l, universe);
    return STATUS(
        InternalError,
        Format(
            "Error while setting up client for producer $0: $1", universe->id(),
            res.status().ToString()));
  }
  std::shared_ptr<XClusterRpcTasks> xcluster_rpc = *res;

  // Now, all tables are validated.
  vector<TableId> validated_tables;
  auto& tbl_iter = mutable_pb->tables();
  validated_tables.insert(validated_tables.begin(), tbl_iter.begin(), tbl_iter.end());

  mutable_pb->set_state(SysUniverseReplicationEntryPB::VALIDATED);
  // Update sys_catalog.
  RETURN_ACTION_NOT_OK(
      sys_catalog_->Upsert(leader_ready_term(), universe),
      "updating universe replication info in sys-catalog");
  l.Commit();

  // Create CDC stream for each validated table, after persisting the replication state change.
  if (!validated_tables.empty()) {
    std::unordered_map<std::string, std::string> options;
    options.reserve(4);
    options.emplace(cdc::kRecordType, CDCRecordType_Name(cdc::CDCRecordType::CHANGE));
    options.emplace(cdc::kRecordFormat, CDCRecordFormat_Name(cdc::CDCRecordFormat::WAL));
    options.emplace(cdc::kSourceType, CDCRequestSource_Name(cdc::CDCRequestSource::XCLUSTER));
    options.emplace(cdc::kCheckpointType, CDCCheckpointType_Name(cdc::CDCCheckpointType::IMPLICIT));

    // Keep track of the bootstrap_id, table_id, and options of streams to update after
    // the last GetCDCStreamCallback finishes. Will be updated by multiple async
    // GetCDCStreamCallback.
    auto stream_update_infos = std::make_shared<StreamUpdateInfos>();
    stream_update_infos->reserve(validated_tables.size());
    auto update_infos_lock = std::make_shared<std::mutex>();

    for (const auto& table : validated_tables) {
      auto producer_bootstrap_id = FindOrNull(table_bootstrap_ids, table);
      if (producer_bootstrap_id && *producer_bootstrap_id) {
        auto table_id = std::make_shared<TableId>();
        auto stream_options = std::make_shared<std::unordered_map<std::string, std::string>>();
        xcluster_rpc->client()->GetCDCStream(
            *producer_bootstrap_id, table_id, stream_options,
            std::bind(
                &CatalogManager::GetCDCStreamCallback, this, *producer_bootstrap_id, table_id,
                stream_options, universe->ReplicationGroupId(), table, xcluster_rpc,
                std::placeholders::_1, stream_update_infos, update_infos_lock));
      } else {
        xcluster_rpc->client()->CreateCDCStream(
            table, options, transactional,
            std::bind(
                &CatalogManager::AddCDCStreamToUniverseAndInitConsumer, this,
                universe->ReplicationGroupId(), table, std::placeholders::_1,
                nullptr /* on_success_cb */));
      }
    }
  }
  return Status::OK();
}

Status CatalogManager::AddValidatedTableAndCreateCdcStreams(
    scoped_refptr<UniverseReplicationInfo> universe,
    const std::unordered_map<TableId, xrepl::StreamId>& table_bootstrap_ids,
    const TableId& producer_table,
    const TableId& consumer_table,
    const ColocationSchemaVersions& colocated_schema_versions) {
  RETURN_NOT_OK(AddValidatedTableToUniverseReplication(universe, producer_table, consumer_table,
                                                       cdc::kInvalidSchemaVersion,
                                                       cdc::kInvalidSchemaVersion,
                                                       colocated_schema_versions));
  return CreateCdcStreamsIfReplicationValidated(universe, table_bootstrap_ids);
}

void CatalogManager::GetTableSchemaCallback(
    const xcluster::ReplicationGroupId& replication_group_id,
    const std::shared_ptr<client::YBTableInfo>& producer_info,
    const SetupReplicationInfo& setup_info, const Status& s) {
  // First get the universe.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << replication_group_id;
      return;
    }
  }

  string action = "getting schema for table";
  auto status = s;
  if (status.ok()) {
    action = "validating table schema and creating CDC stream";
    status = ValidateTableAndCreateCdcStreams(universe, producer_info, setup_info);
  }

  if (!status.ok()) {
    LOG(ERROR) << "Error " << action << ". Universe: " << replication_group_id
               << ", Table: " << producer_info->table_id << ": " << status;
    MarkUniverseReplicationFailed(universe, status);
  }
}

Status CatalogManager::ValidateTableAndCreateCdcStreams(
    scoped_refptr<UniverseReplicationInfo> universe,
    const std::shared_ptr<client::YBTableInfo>& producer_info,
      const SetupReplicationInfo& setup_info) {
  auto l = universe->LockForWrite();
  if (producer_info->table_name.namespace_name() == master::kSystemNamespaceName) {
    auto status = STATUS(IllegalState, "Cannot replicate system tables.");
    MarkUniverseReplicationFailed(status, &l, universe);
    return status;
  }
  RETURN_ACTION_NOT_OK(
      sys_catalog_->Upsert(leader_ready_term(), universe),
      "updating system tables in universe replication");
  l.Commit();

  GetTableSchemaResponsePB consumer_schema;
  RETURN_NOT_OK(ValidateTableSchemaForXCluster(*producer_info, setup_info, &consumer_schema));

  // If Bootstrap Id is passed in then it must be provided for all tables.
  const auto& producer_bootstrap_ids = setup_info.table_bootstrap_ids;
  SCHECK(
      producer_bootstrap_ids.empty() || producer_bootstrap_ids.contains(producer_info->table_id),
      NotFound,
      Format("Bootstrap id not found for table $0", producer_info->table_name.ToString()));

  RETURN_NOT_OK(
      IsBootstrapRequiredOnProducer(universe, producer_info->table_id, producer_bootstrap_ids));

  SchemaVersion producer_schema_version = producer_info->schema.version();
  SchemaVersion consumer_schema_version = consumer_schema.version();
  ColocationSchemaVersions colocated_schema_versions;
  RETURN_NOT_OK(AddValidatedTableToUniverseReplication(
      universe, producer_info->table_id, consumer_schema.identifier().table_id(),
      producer_schema_version, consumer_schema_version, colocated_schema_versions));

  return CreateCdcStreamsIfReplicationValidated(universe, producer_bootstrap_ids);
}

void CatalogManager::GetTablegroupSchemaCallback(
    const xcluster::ReplicationGroupId& replication_group_id,
    const std::shared_ptr<std::vector<client::YBTableInfo>>& infos,
    const TablegroupId& producer_tablegroup_id, const SetupReplicationInfo& setup_info,
    const Status& s) {
  // First get the universe.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << replication_group_id;
      return;
    }
  }

  auto status =
      GetTablegroupSchemaCallbackInternal(universe, *infos, producer_tablegroup_id, setup_info, s);
  if (!status.ok()) {
    std::ostringstream oss;
    for (size_t i = 0; i < infos->size(); ++i) {
      oss << ((i == 0) ? "" : ", ") << (*infos)[i].table_id;
    }
    LOG(ERROR) << "Error processing for tables: [ " << oss.str()
               << " ] for xCluster replication group " << replication_group_id << ": " << status;
    MarkUniverseReplicationFailed(universe, status);
  }
}

Status CatalogManager::GetTablegroupSchemaCallbackInternal(
    scoped_refptr<UniverseReplicationInfo>& universe, const std::vector<client::YBTableInfo>& infos,
    const TablegroupId& producer_tablegroup_id, const SetupReplicationInfo& setup_info,
    const Status& s) {
  RETURN_NOT_OK(s);

  SCHECK(!infos.empty(), IllegalState, Format("Tablegroup $0 is empty", producer_tablegroup_id));

  // validated_consumer_tables contains the table IDs corresponding to that
  // from the producer tables.
  std::unordered_set<TableId> validated_consumer_tables;
  ColocationSchemaVersions colocated_schema_versions;
  colocated_schema_versions.reserve(infos.size());
  for (const auto& info : infos) {
    // Validate each of the member table in the tablegroup.
    GetTableSchemaResponsePB resp;
    RETURN_NOT_OK(ValidateTableSchemaForXCluster(info, setup_info, &resp));

    colocated_schema_versions.emplace_back(
        resp.schema().colocated_table_id().colocation_id(), info.schema.version(), resp.version());
    validated_consumer_tables.insert(resp.identifier().table_id());
  }

  // Get the consumer tablegroup ID. Since this call is expensive (one needs to reverse lookup
  // the tablegroup ID from table ID), we only do this call once and do validation afterward.
  TablegroupId consumer_tablegroup_id;
  // Starting Colocation GA, colocated databases create implicit underlying tablegroups.
  bool colocated_database;
  {
    SharedLock lock(mutex_);
    const auto* tablegroup = tablegroup_manager_->FindByTable(*validated_consumer_tables.begin());
    SCHECK(
        tablegroup, IllegalState,
        Format("No consumer tablegroup found for producer tablegroup: $0", producer_tablegroup_id));

    consumer_tablegroup_id = tablegroup->id();

    auto ns = FindPtrOrNull(namespace_ids_map_, tablegroup->database_id());
    SCHECK(
        ns, IllegalState,
        Format("Could not find namespace by namespace id $0", tablegroup->database_id()));
    colocated_database = ns->colocated();
  }

  // tables_in_consumer_tablegroup are the tables listed within the consumer_tablegroup_id.
  // We need validated_consumer_tables and tables_in_consumer_tablegroup to be identical.
  std::unordered_set<TableId> tables_in_consumer_tablegroup;
  {
    GetTablegroupSchemaRequestPB req;
    GetTablegroupSchemaResponsePB resp;
    req.mutable_tablegroup()->set_id(consumer_tablegroup_id);
    auto status = GetTablegroupSchema(&req, &resp);
    if (status.ok() && resp.has_error()) {
      status = StatusFromPB(resp.error().status());
    }
    RETURN_NOT_OK_PREPEND(
        status,
        Format("Error when getting consumer tablegroup schema: $0", consumer_tablegroup_id));

    for (const auto& info : resp.get_table_schema_response_pbs()) {
      tables_in_consumer_tablegroup.insert(info.identifier().table_id());
    }
  }

  if (validated_consumer_tables != tables_in_consumer_tablegroup) {
    return STATUS(
        IllegalState,
        Format(
            "Mismatch between tables associated with producer tablegroup $0 and "
            "tables in consumer tablegroup $1: ($2) vs ($3).",
            producer_tablegroup_id, consumer_tablegroup_id, AsString(validated_consumer_tables),
            AsString(tables_in_consumer_tablegroup)));
  }

  RETURN_NOT_OK_PREPEND(
      IsBootstrapRequiredOnProducer(
          universe, producer_tablegroup_id, setup_info.table_bootstrap_ids),
      Format(
          "Found error while checking if bootstrap is required for table $0",
          producer_tablegroup_id));

  TableId producer_parent_table_id;
  TableId consumer_parent_table_id;
  if (colocated_database) {
    producer_parent_table_id = GetColocationParentTableId(producer_tablegroup_id);
    consumer_parent_table_id = GetColocationParentTableId(consumer_tablegroup_id);
  } else {
    producer_parent_table_id = GetTablegroupParentTableId(producer_tablegroup_id);
    consumer_parent_table_id = GetTablegroupParentTableId(consumer_tablegroup_id);
  }

  {
    SharedLock lock(mutex_);
    SCHECK(
        !xcluster_consumer_table_stream_ids_map_.contains(consumer_parent_table_id), IllegalState,
        "N:1 replication topology not supported");
  }

  RETURN_NOT_OK(AddValidatedTableAndCreateCdcStreams(
      universe, setup_info.table_bootstrap_ids, producer_parent_table_id, consumer_parent_table_id,
      colocated_schema_versions));
  return Status::OK();
}

void CatalogManager::GetColocatedTabletSchemaCallback(
    const xcluster::ReplicationGroupId& replication_group_id,
    const std::shared_ptr<std::vector<client::YBTableInfo>>& infos,
    const SetupReplicationInfo& setup_info, const Status& s) {
  // First get the universe.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << replication_group_id;
      return;
    }
  }

  if (!s.ok()) {
    MarkUniverseReplicationFailed(universe, s);
    std::ostringstream oss;
    for (size_t i = 0; i < infos->size(); ++i) {
      oss << ((i == 0) ? "" : ", ") << (*infos)[i].table_id;
    }
    LOG(ERROR) << "Error getting schema for tables: [ " << oss.str() << " ]: " << s;
    return;
  }

  if (infos->empty()) {
    LOG(WARNING) << "Received empty list of tables to validate: " << s;
    return;
  }

  // Validate table schemas.
  std::unordered_set<TableId> producer_parent_table_ids;
  std::unordered_set<TableId> consumer_parent_table_ids;
  ColocationSchemaVersions colocated_schema_versions;
  colocated_schema_versions.reserve(infos->size());
  for (const auto& info : *infos) {
    // Verify that we have a colocated table.
    if (!info.colocated) {
      MarkUniverseReplicationFailed(
          universe,
          STATUS(InvalidArgument, Format("Received non-colocated table: $0", info.table_id)));
      LOG(ERROR) << "Received non-colocated table: " << info.table_id;
      return;
    }
    // Validate each table, and get the parent colocated table id for the consumer.
    GetTableSchemaResponsePB resp;
    Status table_status = ValidateTableSchemaForXCluster(info, setup_info, &resp);
    if (!table_status.ok()) {
      MarkUniverseReplicationFailed(universe, table_status);
      LOG(ERROR) << "Found error while validating table schema for table " << info.table_id << ": "
                 << table_status;
      return;
    }
    // Store the parent table ids.
    producer_parent_table_ids.insert(GetColocatedDbParentTableId(info.table_name.namespace_id()));
    consumer_parent_table_ids.insert(
        GetColocatedDbParentTableId(resp.identifier().namespace_().id()));
    colocated_schema_versions.emplace_back(
        resp.schema().colocated_table_id().colocation_id(), info.schema.version(), resp.version());
  }

  // Verify that we only found one producer and one consumer colocated parent table id.
  if (producer_parent_table_ids.size() != 1) {
    auto message = Format(
        "Found incorrect number of producer colocated parent table ids. "
        "Expected 1, but found: $0",
        AsString(producer_parent_table_ids));
    MarkUniverseReplicationFailed(universe, STATUS(InvalidArgument, message));
    LOG(ERROR) << message;
    return;
  }
  if (consumer_parent_table_ids.size() != 1) {
    auto message = Format(
        "Found incorrect number of consumer colocated parent table ids. "
        "Expected 1, but found: $0",
        AsString(consumer_parent_table_ids));
    MarkUniverseReplicationFailed(universe, STATUS(InvalidArgument, message));
    LOG(ERROR) << message;
    return;
  }

  {
    SharedLock lock(mutex_);
    if (xcluster_consumer_table_stream_ids_map_.contains(*consumer_parent_table_ids.begin())) {
      std::string message = "N:1 replication topology not supported";
      MarkUniverseReplicationFailed(universe, STATUS(IllegalState, message));
      LOG(ERROR) << message;
      return;
    }
  }

  Status status = IsBootstrapRequiredOnProducer(
      universe, *producer_parent_table_ids.begin(), setup_info.table_bootstrap_ids);
  if (!status.ok()) {
    MarkUniverseReplicationFailed(universe, status);
    LOG(ERROR) << "Found error while checking if bootstrap is required for table "
               << *producer_parent_table_ids.begin() << ": " << status;
  }

  status = AddValidatedTableAndCreateCdcStreams(
      universe,
      setup_info.table_bootstrap_ids,
      *producer_parent_table_ids.begin(),
      *consumer_parent_table_ids.begin(),
      colocated_schema_versions);

  if (!status.ok()) {
    LOG(ERROR) << "Found error while adding validated table to system catalog: "
               << *producer_parent_table_ids.begin() << ": " << status;
    return;
  }
}

void CatalogManager::GetCDCStreamCallback(
    const xrepl::StreamId& bootstrap_id, std::shared_ptr<TableId> table_id,
    std::shared_ptr<std::unordered_map<std::string, std::string>> options,
    const xcluster::ReplicationGroupId& replication_group_id, const TableId& table,
    std::shared_ptr<XClusterRpcTasks> xcluster_rpc, const Status& s,
    std::shared_ptr<StreamUpdateInfos> stream_update_infos,
    std::shared_ptr<std::mutex> update_infos_lock) {
  if (!s.ok()) {
    LOG(ERROR) << "Unable to find bootstrap id " << bootstrap_id;
    AddCDCStreamToUniverseAndInitConsumer(replication_group_id, table, s);
    return;
  }

  if (*table_id != table) {
    const Status invalid_bootstrap_id_status = STATUS_FORMAT(
        InvalidArgument, "Invalid bootstrap id for table $0. Bootstrap id $1 belongs to table $2",
        table, bootstrap_id, *table_id);
    LOG(ERROR) << invalid_bootstrap_id_status;
    AddCDCStreamToUniverseAndInitConsumer(replication_group_id, table, invalid_bootstrap_id_status);
    return;
  }

  scoped_refptr<UniverseReplicationInfo> original_universe;
  {
    SharedLock lock(mutex_);
    original_universe = FindPtrOrNull(
        universe_replication_map_, xcluster::GetOriginalReplicationGroupId(replication_group_id));
  }

  if (original_universe == nullptr) {
    LOG(ERROR) << "Universe not found: " << replication_group_id;
    return;
  }

  cdc::StreamModeTransactional transactional(original_universe->LockForRead()->pb.transactional());

  // todo check options
  {
    std::lock_guard lock(*update_infos_lock);
    stream_update_infos->push_back({bootstrap_id, *table_id, *options});
  }

  const auto update_xrepl_stream_func = [&]() -> Status {
    // Extra callback on universe setup success - update the producer to let it know that
    // the bootstrapping is complete. This callback will only be called once among all
    // the GetCDCStreamCallback calls, and we update all streams in batch at once.

    std::vector<xrepl::StreamId> update_bootstrap_ids;
    std::vector<SysCDCStreamEntryPB> update_entries;
    {
      std::lock_guard lock(*update_infos_lock);

      for (const auto& [update_bootstrap_id, update_table_id, update_options] :
           *stream_update_infos) {
        SysCDCStreamEntryPB new_entry;
        new_entry.add_table_id(update_table_id);
        new_entry.mutable_options()->Reserve(narrow_cast<int>(update_options.size()));
        for (const auto& [key, value] : update_options) {
          if (key == cdc::kStreamState) {
            // We will set state explicitly.
            continue;
          }
          auto new_option = new_entry.add_options();
          new_option->set_key(key);
          new_option->set_value(value);
        }
        new_entry.set_state(master::SysCDCStreamEntryPB::ACTIVE);
        new_entry.set_transactional(transactional);

        update_bootstrap_ids.push_back(update_bootstrap_id);
        update_entries.push_back(new_entry);
      }
    }

    RETURN_NOT_OK_PREPEND(
        xcluster_rpc->client()->UpdateCDCStream(update_bootstrap_ids, update_entries),
        "Unable to update xrepl stream options on source universe");

    {
      std::lock_guard lock(*update_infos_lock);
      stream_update_infos->clear();
    }
    return Status::OK();
  };

  AddCDCStreamToUniverseAndInitConsumer(
      replication_group_id, table, bootstrap_id, update_xrepl_stream_func);
}

void CatalogManager::AddCDCStreamToUniverseAndInitConsumer(
    const xcluster::ReplicationGroupId& replication_group_id, const TableId& table_id,
    const Result<xrepl::StreamId>& stream_id, std::function<Status()> on_success_cb) {
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << replication_group_id;
      return;
    }
  }

  Status s;
  if (!stream_id.ok()) {
    s = std::move(stream_id).status();
  } else {
    s = AddCDCStreamToUniverseAndInitConsumerInternal(
        universe, table_id, *stream_id, std::move(on_success_cb));
  }

  if (!s.ok()) {
    MarkUniverseReplicationFailed(universe, s);
  }
}

Status CatalogManager::AddCDCStreamToUniverseAndInitConsumerInternal(
    scoped_refptr<UniverseReplicationInfo> universe, const TableId& table_id,
    const xrepl::StreamId& stream_id, std::function<Status()> on_success_cb) {
  bool merge_alter = false;
  bool validated_all_tables = false;
  std::vector<XClusterConsumerStreamInfo> consumer_info;
  {
    auto l = universe->LockForWrite();
    if (l->is_deleted_or_failed()) {
      // Nothing to do if universe is being deleted.
      return Status::OK();
    }

    auto map = l.mutable_data()->pb.mutable_table_streams();
    (*map)[table_id] = stream_id.ToString();

    // This functions as a barrier: waiting for the last RPC call from GetTableSchemaCallback.
    if (l.mutable_data()->pb.table_streams_size() == l->pb.tables_size()) {
      // All tables successfully validated! Register CDC consumers & start replication.
      validated_all_tables = true;
      LOG(INFO) << "Registering CDC consumers for universe " << universe->id();

      consumer_info.reserve(l->pb.tables_size());
      std::set<TableId> consumer_table_ids;
      for (const auto& [producer_table_id, consumer_table_id] : l->pb.validated_tables()) {
        consumer_table_ids.insert(consumer_table_id);

        XClusterConsumerStreamInfo info;
        info.producer_table_id = producer_table_id;
        info.consumer_table_id = consumer_table_id;
        info.stream_id = VERIFY_RESULT(xrepl::StreamId::FromString((*map)[producer_table_id]));
        consumer_info.push_back(info);
      }

      if (l->IsDbScoped()) {
        std::vector<NamespaceId> consumer_namespace_ids;
        for (const auto& ns_info : l->pb.db_scoped_info().namespace_infos()) {
          consumer_namespace_ids.push_back(ns_info.consumer_namespace_id());
        }
        RETURN_NOT_OK(ValidateTableListForDbScopedReplication(
            *universe, consumer_namespace_ids, consumer_table_ids, *this));
      }

      std::vector<HostPort> hp;
      HostPortsFromPBs(l->pb.producer_master_addresses(), &hp);
      auto xcluster_rpc_tasks =
          VERIFY_RESULT(universe->GetOrCreateXClusterRpcTasks(l->pb.producer_master_addresses()));
      RETURN_NOT_OK(InitXClusterConsumer(
          consumer_info, HostPort::ToCommaSeparatedString(hp), *universe.get(),
          xcluster_rpc_tasks));

      if (xcluster::IsAlterReplicationGroupId(universe->ReplicationGroupId())) {
        // Don't enable ALTER universes, merge them into the main universe instead.
        // on_success_cb will be invoked in MergeUniverseReplication.
        merge_alter = true;
      } else {
        l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::ACTIVE);
        if (on_success_cb) {
          // Before updating, run any callbacks on success.
          RETURN_NOT_OK(on_success_cb());
        }
      }
    }

    // Update sys_catalog with new producer table id info.
    RETURN_NOT_OK(sys_catalog_->Upsert(leader_ready_term(), universe));

    l.Commit();
  }

  if (!validated_all_tables) {
    return Status::OK();
  }

  auto final_id = xcluster::GetOriginalReplicationGroupId(universe->ReplicationGroupId());
  // If this is an 'alter', merge back into primary command now that setup is a success.
  if (merge_alter) {
    RETURN_NOT_OK(MergeUniverseReplication(universe, final_id, std::move(on_success_cb)));
  }
  // Update the in-memory cache of consumer tables.
  LockGuard lock(mutex_);
  for (const auto& info : consumer_info) {
    const auto& c_table_id = info.consumer_table_id;
    const auto& c_stream_id = info.stream_id;
    xcluster_consumer_table_stream_ids_map_[c_table_id].emplace(final_id, c_stream_id);
  }

  return Status::OK();
}

/*
 * UpdateXClusterConsumerOnTabletSplit updates the consumer -> producer tablet mapping after a local
 * tablet split.
 */
Status CatalogManager::UpdateXClusterConsumerOnTabletSplit(
    const TableId& consumer_table_id, const SplitTabletIds& split_tablet_ids) {
  // Check if this table is consuming a stream.
  auto stream_ids = GetXClusterConsumerStreamIdsForTable(consumer_table_id);
  if (stream_ids.empty()) {
    return Status::OK();
  }

  auto consumer_tablet_keys = VERIFY_RESULT(GetTableKeyRanges(consumer_table_id));
  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForWrite();
  for (const auto& [replication_group_id, stream_id] : stream_ids) {
    // Fetch the stream entry so we can update the mappings.
    auto replication_group_map =
        l.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    auto producer_entry = FindOrNull(*replication_group_map, replication_group_id.ToString());
    // If we can't find the entries, then the stream has been deleted.
    if (!producer_entry) {
      LOG(WARNING) << "Unable to find the producer entry for universe " << replication_group_id;
      continue;
    }
    auto stream_entry = FindOrNull(*producer_entry->mutable_stream_map(), stream_id.ToString());
    if (!stream_entry) {
      LOG(WARNING) << "Unable to find the producer entry for universe " << replication_group_id
                   << ", stream " << stream_id;
      continue;
    }
    DCHECK(stream_entry->consumer_table_id() == consumer_table_id);

    RETURN_NOT_OK(
        UpdateTabletMappingOnConsumerSplit(consumer_tablet_keys, split_tablet_ids, stream_entry));
  }

  // Also bump the cluster_config_ version so that changes are propagated to tservers.
  l.mutable_data()->pb.set_version(l.mutable_data()->pb.version() + 1);

  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
      "Updating cluster config in sys-catalog"));
  l.Commit();

  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();

  return Status::OK();
}

Status CatalogManager::UpdateCDCProducerOnTabletSplit(
    const TableId& producer_table_id, const SplitTabletIds& split_tablet_ids) {
  std::vector<CDCStreamInfoPtr> streams;
  std::unordered_set<xrepl::StreamId> cdcsdk_stream_ids;
  std::vector<cdc::CDCStateTableEntry> entries;
  for (const auto stream_type : {cdc::XCLUSTER, cdc::CDCSDK}) {
    if (stream_type == cdc::CDCSDK) {
      const auto table_info = GetTableInfo(producer_table_id);
      if (table_info) {
        // Skip adding children tablet entries in cdc state if the table is an index or a mat view.
        // These tables, if present in CDC stream, are anyway going to be removed by a bg thread.
        // This check ensures even if there is a race condition where a tablet of a non-eligible
        // table splits and concurrently we are removing such tables from stream, the child tables
        // do not get added.
        {
          SharedLock lock(mutex_);
          if (!IsTableEligibleForCDCSDKStream(table_info, std::nullopt)) {
            LOG(INFO) << "Skipping adding children tablets to cdc state for table "
                      << producer_table_id << " as it is not meant to part of a CDC stream";
            continue;
          }
        }
      }
    }

    {
      SharedLock lock(mutex_);
      streams = GetXReplStreamsForTable(producer_table_id, stream_type);
    }

    TEST_SYNC_POINT("UpdateCDCProducerOnTabletSplit::FindStreamsForAddingChildEntriesComplete");

    for (const auto& stream : streams) {
      if (stream_type == cdc::CDCSDK) {
        cdcsdk_stream_ids.insert(stream->StreamId());
      }

      auto last_active_time = GetCurrentTimeMicros();

      std::optional<cdc::CDCStateTableEntry> parent_entry_opt;
      if (stream_type == cdc::CDCSDK) {
         parent_entry_opt = VERIFY_RESULT(cdc_state_table_->TryFetchEntry(
          {split_tablet_ids.source, stream->StreamId()},
          cdc::CDCStateTableEntrySelector().IncludeActiveTime().IncludeCDCSDKSafeTime()));
        DCHECK(parent_entry_opt);
      }

      // In the case of a Consistent Snapshot Stream, set the active_time of the children tablets
      // to the corresponding value in the parent tablet.
      // This will allow to establish that a child tablet is of interest to a stream
      // iff the parent tablet is also of interest to the stream.
      // Thus, retention barriers, inherited from the parent tablet, can be released
      // on the children tablets also if not of interest to the stream
      if (stream->IsConsistentSnapshotStream()) {
        LOG_WITH_FUNC(INFO) << "Copy active time from parent to child tablets"
                            << " Tablets involved: " << split_tablet_ids.ToString()
                            << " Consistent Snapshot StreamId: " << stream->StreamId();
        DCHECK(parent_entry_opt->active_time);
        if (parent_entry_opt && parent_entry_opt->active_time) {
            last_active_time = *parent_entry_opt->active_time;
        } else {
            LOG_WITH_FUNC(WARNING)
                << Format("Did not find $0 value in the cdc state table",
                          parent_entry_opt ? "active_time" : "row")
                << " for parent tablet: " << split_tablet_ids.source
                << " and stream: " << stream->StreamId();
        }
      }

      // Insert children entries into cdc_state now, set the opid to 0.0 and the timestamp to
      // NULL. When we process the parent's SPLIT_OP in GetChanges, we will update the opid to
      // the SPLIT_OP so that the children pollers continue from the next records. When we process
      // the first GetChanges for the children, then their timestamp value will be set. We use
      // this information to know that the children has been polled for. Once both children have
      // been polled for, then we can delete the parent tablet via the bg task
      // DoProcessXClusterParentTabletDeletion.
      for (const auto& child_tablet_id :
           {split_tablet_ids.children.first, split_tablet_ids.children.second}) {
        cdc::CDCStateTableEntry entry(child_tablet_id, stream->StreamId());
        entry.checkpoint = OpId().Min();

        if (stream_type == cdc::CDCSDK) {
          entry.active_time = last_active_time;
          DCHECK(parent_entry_opt->cdc_sdk_safe_time);
          if (parent_entry_opt && parent_entry_opt->cdc_sdk_safe_time) {
            entry.cdc_sdk_safe_time = *parent_entry_opt->cdc_sdk_safe_time;
          } else {
            LOG_WITH_FUNC(WARNING) << Format(
                                          "Did not find $0 value in the cdc state table",
                                          parent_entry_opt ? "cdc_sdk_safe_time" : "row")
                                   << " for parent tablet: " << split_tablet_ids.source
                                   << " and stream: " << stream->StreamId();
            entry.cdc_sdk_safe_time = last_active_time;
          }
        }

        entries.push_back(std::move(entry));
      }
    }
  }

  RETURN_NOT_OK(cdc_state_table_->InsertEntries(entries));

  TEST_SYNC_POINT("UpdateCDCProducerOnTabletSplit::AddChildEntriesComplete");

  TEST_SYNC_POINT("UpdateCDCProducerOnTabletSplit::ReVerifyStreamForAddingChildEntries");

  // Re-fetch all CDCSDK streams for the table and confirm the above inserted entries belong to one
  // of those streams. If not, update them and set the checkpoint to max. This is to handle race
  // condition where the table being removed from the stream splits simultaneously.
  if (!entries.empty() && !cdcsdk_stream_ids.empty()) {
    RETURN_NOT_OK(
        ReVerifyChildrenEntriesOnTabletSplit(producer_table_id, entries, cdcsdk_stream_ids));
  }

  return Status::OK();
}

Status CatalogManager::ReVerifyChildrenEntriesOnTabletSplit(
    const TableId& producer_table_id, const std::vector<cdc::CDCStateTableEntry>& entries,
    const std::unordered_set<xrepl::StreamId>& cdcsdk_stream_ids) {
  std::vector<CDCStreamInfoPtr> streams;
  {
    SharedLock lock(mutex_);
    streams = GetXReplStreamsForTable(producer_table_id, cdc::CDCSDK);
  }

  std::unordered_set<xrepl::StreamId> refetched_cdcsdk_stream_ids;
  for (const auto& stream : streams) {
    refetched_cdcsdk_stream_ids.insert(stream->StreamId());
  }

  std::vector<cdc::CDCStateTableEntry> entries_to_update;
  for (const auto& entry : entries) {
    auto stream_id = entry.key.stream_id;
    // Update the entries whose streams were not received on re-fetch.
    if (cdcsdk_stream_ids.contains(stream_id) && !refetched_cdcsdk_stream_ids.contains(stream_id)) {
      cdc::CDCStateTableEntry update_entry = entry;
      update_entry.checkpoint = OpId::Max();
      entries_to_update.emplace_back(std::move(update_entry));
    }
  }

  if (!entries_to_update.empty()) {
    LOG(INFO) << "Updating the following state table entries to max checkpoint as their table "
                 "is being/has been removed from the stream - "
              << AsString(entries_to_update);
    RETURN_NOT_OK(cdc_state_table_->UpdateEntries(entries_to_update));
  }

  return Status::OK();
}

Status CatalogManager::InitXClusterConsumer(
    const std::vector<XClusterConsumerStreamInfo>& consumer_info, const std::string& master_addrs,
    UniverseReplicationInfo& replication_info,
    std::shared_ptr<XClusterRpcTasks> xcluster_rpc_tasks) {
  auto universe_l = replication_info.LockForRead();
  auto schema_version_mappings = universe_l->pb.schema_version_mappings();

  // Get the tablets in the consumer table.
  cdc::ProducerEntryPB producer_entry;

  if (FLAGS_enable_xcluster_auto_flag_validation) {
    auto compatible_auto_flag_config_version = VERIFY_RESULT(
        GetAutoFlagConfigVersionIfCompatible(replication_info, master_->GetAutoFlagsConfig()));
    producer_entry.set_compatible_auto_flag_config_version(compatible_auto_flag_config_version);
    producer_entry.set_validated_auto_flags_config_version(compatible_auto_flag_config_version);
  }

  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForWrite();
  auto* consumer_registry = l.mutable_data()->pb.mutable_consumer_registry();
  auto transactional = universe_l->pb.transactional();
  if (!xcluster::IsAlterReplicationGroupId(replication_info.ReplicationGroupId())) {
    if (universe_l->IsDbScoped()) {
      DCHECK(transactional);
    }
  }

  for (const auto& stream_info : consumer_info) {
    auto consumer_tablet_keys = VERIFY_RESULT(GetTableKeyRanges(stream_info.consumer_table_id));
    auto schema_version = VERIFY_RESULT(GetTableSchemaVersion(stream_info.consumer_table_id));

    cdc::StreamEntryPB stream_entry;
    // Get producer tablets and map them to the consumer tablets
    RETURN_NOT_OK(InitXClusterStream(
        stream_info.producer_table_id, stream_info.consumer_table_id, consumer_tablet_keys,
        &stream_entry, xcluster_rpc_tasks));
    // Set the validated consumer schema version
    auto* producer_schema_pb = stream_entry.mutable_producer_schema();
    producer_schema_pb->set_last_compatible_consumer_schema_version(schema_version);
    auto* schema_versions = stream_entry.mutable_schema_versions();
    auto mapping = FindOrNull(schema_version_mappings, stream_info.producer_table_id);
    SCHECK(mapping, NotFound, Format("No schema mapping for $0", stream_info.producer_table_id));
    if (IsColocationParentTableId(stream_info.consumer_table_id)) {
      // Get all the child tables and add their mappings
      auto& colocated_schema_versions_pb = *stream_entry.mutable_colocated_schema_versions();
      for (const auto& colocated_entry : mapping->colocated_schema_versions()) {
        auto colocation_id = colocated_entry.colocation_id();
        colocated_schema_versions_pb[colocation_id].set_current_producer_schema_version(
            colocated_entry.schema_version_mapping().producer_schema_version());
        colocated_schema_versions_pb[colocation_id].set_current_consumer_schema_version(
            colocated_entry.schema_version_mapping().consumer_schema_version());
      }
    } else {
      schema_versions->set_current_producer_schema_version(
        mapping->schema_version_mapping().producer_schema_version());
      schema_versions->set_current_consumer_schema_version(
        mapping->schema_version_mapping().consumer_schema_version());
    }

    // Mark this stream as special if it is for the ddl_queue table.
    auto table_info = GetTableInfo(stream_info.consumer_table_id);
    stream_entry.set_is_ddl_queue_table(
        table_info->GetTableType() == PGSQL_TABLE_TYPE &&
        table_info->name() == xcluster::kDDLQueueTableName &&
        table_info->pgschema_name() == xcluster::kDDLQueuePgSchemaName);

    (*producer_entry.mutable_stream_map())[stream_info.stream_id.ToString()] =
        std::move(stream_entry);
  }

  // Log the Network topology of the Producer Cluster
  auto master_addrs_list = StringSplit(master_addrs, ',');
  producer_entry.mutable_master_addrs()->Reserve(narrow_cast<int>(master_addrs_list.size()));
  for (const auto& addr : master_addrs_list) {
    auto hp = VERIFY_RESULT(HostPort::FromString(addr, 0));
    HostPortToPB(hp, producer_entry.add_master_addrs());
  }

  auto* replication_group_map = consumer_registry->mutable_producer_map();
  SCHECK_EQ(
      replication_group_map->count(replication_info.id()), 0, InvalidArgument,
      "Already created a consumer for this universe");

  // TServers will use the ClusterConfig to create CDC Consumers for applicable local tablets.
  (*replication_group_map)[replication_info.id()] = std::move(producer_entry);

  l.mutable_data()->pb.set_version(l.mutable_data()->pb.version() + 1);
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
      "updating cluster config in sys-catalog"));

  SyncXClusterConsumerReplicationStatusMap(
      replication_info.ReplicationGroupId(), *replication_group_map);
  l.Commit();

  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();

  return Status::OK();
}

Status CatalogManager::MergeUniverseReplication(
    scoped_refptr<UniverseReplicationInfo> universe, xcluster::ReplicationGroupId original_id,
    std::function<Status()> on_success_cb) {
  // Merge back into primary command now that setup is a success.
  LOG(INFO) << "Merging CDC universe: " << universe->id() << " into " << original_id;

  SCHECK(
      !FLAGS_TEST_fail_universe_replication_merge, IllegalState,
      "TEST_fail_universe_replication_merge");

  scoped_refptr<UniverseReplicationInfo> original_universe;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    original_universe = FindPtrOrNull(universe_replication_map_, original_id);
    if (original_universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << original_id;
      return Status::OK();
    }
  }

  {
    auto cluster_config = ClusterConfig();
    // Acquire Locks in order of Original Universe, Cluster Config, New Universe
    auto original_lock = original_universe->LockForWrite();
    auto alter_lock = universe->LockForWrite();
    auto cl = cluster_config->LockForWrite();

    // Merge Cluster Config for TServers.
    auto* consumer_registry = cl.mutable_data()->pb.mutable_consumer_registry();
    auto pm = consumer_registry->mutable_producer_map();
    auto original_producer_entry = pm->find(original_universe->id());
    auto alter_producer_entry = pm->find(universe->id());
    if (original_producer_entry != pm->end() && alter_producer_entry != pm->end()) {
      // Merge the Tables from the Alter into the original.
      auto as = alter_producer_entry->second.stream_map();
      original_producer_entry->second.mutable_stream_map()->insert(as.begin(), as.end());
      // Delete the Alter
      pm->erase(alter_producer_entry);
    } else {
      LOG(WARNING) << "Could not find both universes in Cluster Config: " << universe->id();
    }
    cl.mutable_data()->pb.set_version(cl.mutable_data()->pb.version() + 1);

    // Merge Master Config on Consumer. (no need for Producer changes, since it uses stream_id)
    // Merge Table->StreamID mapping.
    auto& alter_pb = alter_lock.mutable_data()->pb;
    auto& original_pb = original_lock.mutable_data()->pb;

    auto* alter_tables = alter_pb.mutable_tables();
    original_pb.mutable_tables()->MergeFrom(*alter_tables);
    alter_tables->Clear();
    auto* alter_table_streams = alter_pb.mutable_table_streams();
    original_pb.mutable_table_streams()->insert(
        alter_table_streams->begin(), alter_table_streams->end());
    alter_table_streams->clear();
    auto* alter_validated_tables = alter_pb.mutable_validated_tables();
    original_pb.mutable_validated_tables()->insert(
        alter_validated_tables->begin(), alter_validated_tables->end());
    alter_validated_tables->clear();
    if (alter_lock.mutable_data()->IsDbScoped()) {
      auto* alter_namespace_info = alter_pb.mutable_db_scoped_info()->mutable_namespace_infos();
      original_pb.mutable_db_scoped_info()->mutable_namespace_infos()->MergeFrom(
          *alter_namespace_info);
      alter_namespace_info->Clear();
    }

    alter_pb.set_state(SysUniverseReplicationEntryPB::DELETED);

    if (PREDICT_FALSE(FLAGS_TEST_exit_unfinished_merging)) {
      return Status::OK();
    }

    if (on_success_cb) {
      RETURN_NOT_OK(on_success_cb());
    }

    {
      // Need both these updates to be atomic.
      auto w = sys_catalog_->NewWriter(leader_ready_term());
      auto s = w->Mutate<true>(
          QLWriteRequestPB::QL_STMT_UPDATE,
          original_universe.get(),
          universe.get(),
          cluster_config.get());
      s = CheckStatus(
          sys_catalog_->SyncWrite(w.get()),
          "Updating universe replication entries and cluster config in sys-catalog");
    }

    SyncXClusterConsumerReplicationStatusMap(original_universe->ReplicationGroupId(), *pm);
    SyncXClusterConsumerReplicationStatusMap(universe->ReplicationGroupId(), *pm);

    alter_lock.Commit();
    cl.Commit();
    original_lock.Commit();
  }

  // Add alter temp universe to GC.
  MarkUniverseForCleanup(universe->ReplicationGroupId());

  LOG(INFO) << "Done with Merging " << universe->id() << " into " << original_universe->id();

  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();

  return Status::OK();
}

Status CatalogManager::DeleteUniverseReplication(
    const xcluster::ReplicationGroupId& replication_group_id, bool ignore_errors,
    bool skip_producer_stream_deletion, DeleteUniverseReplicationResponsePB* resp) {
  scoped_refptr<UniverseReplicationInfo> ri;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    ri = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (ri == nullptr) {
      return STATUS(
          NotFound, "Universe replication info does not exist", replication_group_id,
          MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  {
    auto l = ri->LockForWrite();
    l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DELETING);
    Status s = sys_catalog_->Upsert(leader_ready_term(), ri);
    RETURN_NOT_OK(
        CheckLeaderStatus(s, "Updating delete universe replication info into sys-catalog"));
    TRACE("Wrote universe replication info to sys-catalog");
    l.Commit();
  }

  auto l = ri->LockForWrite();
  l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DELETED);

  // We can skip the deletion of individual streams for DB Scoped replication since deletion of the
  // outbound replication group will clean it up.
  if (l->IsDbScoped()) {
    skip_producer_stream_deletion = true;
  }

  // Delete subscribers on the Consumer Registry (removes from TServers).
  LOG(INFO) << "Deleting subscribers for producer " << replication_group_id;
  {
    auto cluster_config = ClusterConfig();
    auto cl = cluster_config->LockForWrite();
    auto* consumer_registry = cl.mutable_data()->pb.mutable_consumer_registry();
    auto replication_group_map = consumer_registry->mutable_producer_map();
    auto it = replication_group_map->find(replication_group_id.ToString());
    if (it != replication_group_map->end()) {
      replication_group_map->erase(it);
      cl.mutable_data()->pb.set_version(cl.mutable_data()->pb.version() + 1);
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
          "updating cluster config in sys-catalog"));

      SyncXClusterConsumerReplicationStatusMap(replication_group_id, *replication_group_map);
      cl.Commit();
    }
  }

  // Delete CDC stream config on the Producer.
  if (!l->pb.table_streams().empty() && !skip_producer_stream_deletion) {
    auto result = ri->GetOrCreateXClusterRpcTasks(l->pb.producer_master_addresses());
    if (!result.ok()) {
      LOG(WARNING) << "Unable to create cdc rpc task. CDC streams won't be deleted: " << result;
    } else {
      auto xcluster_rpc = *result;
      vector<xrepl::StreamId> streams;
      std::unordered_map<xrepl::StreamId, TableId> stream_to_producer_table_id;
      for (const auto& [table_id, stream_id_str] : l->pb.table_streams()) {
        auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(stream_id_str));
        streams.emplace_back(stream_id);
        stream_to_producer_table_id.emplace(stream_id, table_id);
      }

      DeleteCDCStreamResponsePB delete_cdc_stream_resp;
      // Set force_delete=true since we are deleting active xCluster streams.
      // Since we are deleting universe replication, we should be ok with
      // streams not existing on the other side, so we pass in ignore_errors
      bool ignore_missing_streams = false;
      auto s = xcluster_rpc->client()->DeleteCDCStream(
          streams,
          true, /* force_delete */
          true /* ignore_errors */,
          &delete_cdc_stream_resp);

      if (delete_cdc_stream_resp.not_found_stream_ids().size() > 0) {
        std::vector<std::string> missing_streams;
        missing_streams.reserve(delete_cdc_stream_resp.not_found_stream_ids().size());
        for (const auto& stream_id : delete_cdc_stream_resp.not_found_stream_ids()) {
          missing_streams.emplace_back(Format(
              "$0 (table_id: $1)", stream_id,
              stream_to_producer_table_id[VERIFY_RESULT(xrepl::StreamId::FromString(stream_id))]));
        }
        auto message =
            Format("Could not find the following streams: $0.", AsString(missing_streams));

        if (s.ok()) {
          // Returned but did not find some streams, so still need to warn the user about those.
          ignore_missing_streams = true;
          s = STATUS(NotFound, message);
        } else {
          s = s.CloneAndPrepend(message);
        }
      }
      RETURN_NOT_OK(ReturnErrorOrAddWarning(s, ignore_errors | ignore_missing_streams, resp));
    }
  }

  if (PREDICT_FALSE(FLAGS_TEST_exit_unfinished_deleting)) {
    // Exit for texting services
    return Status::OK();
  }

  // Delete universe in the Universe Config.
  RETURN_NOT_OK(
      ReturnErrorOrAddWarning(DeleteUniverseReplicationUnlocked(ri), ignore_errors, resp));
  l.Commit();
  LOG(INFO) << "Processed delete universe replication of " << ri->ToString();

  // Run the safe time task as it may need to perform cleanups of it own
  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();

  return Status::OK();
}

Status CatalogManager::DeleteUniverseReplication(
    const DeleteUniverseReplicationRequestPB* req,
    DeleteUniverseReplicationResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteUniverseReplication request from " << RequestorString(rpc) << ": "
            << req->ShortDebugString();

  if (!req->has_replication_group_id()) {
    return STATUS(
        InvalidArgument, "Producer universe ID required", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  RETURN_NOT_OK(ValidateUniverseUUID(req, *this));

  RETURN_NOT_OK(DeleteUniverseReplication(
      xcluster::ReplicationGroupId(req->replication_group_id()), req->ignore_errors(),
      req->skip_producer_stream_deletion(), resp));
  LOG(INFO) << "Successfully completed DeleteUniverseReplication request from "
            << RequestorString(rpc);
  return Status::OK();
}

Status CatalogManager::DeleteUniverseReplicationUnlocked(
    scoped_refptr<UniverseReplicationInfo> universe) {
  // Assumes that caller has locked universe.
  RETURN_ACTION_NOT_OK(
      sys_catalog_->Delete(leader_ready_term(), universe),
      Format("updating sys-catalog, replication_group_id: $0", universe->id()));

  // Remove it from the map.
  LockGuard lock(mutex_);
  if (universe_replication_map_.erase(universe->ReplicationGroupId()) < 1) {
    LOG(WARNING) << "Failed to remove replication info from map: replication_group_id: "
                 << universe->id();
  }
  // If replication is at namespace-level, also remove from the namespace-level map.
  namespace_replication_map_.erase(universe->ReplicationGroupId());
  // Also update the mapping of consumer tables.
  for (const auto& table : universe->metadata().state().pb.validated_tables()) {
    if (xcluster_consumer_table_stream_ids_map_[table.second].erase(
            universe->ReplicationGroupId()) < 1) {
      LOG(WARNING) << "Failed to remove consumer table from mapping. "
                   << "table_id: " << table.second << ": replication_group_id: " << universe->id();
    }
    if (xcluster_consumer_table_stream_ids_map_[table.second].empty()) {
      xcluster_consumer_table_stream_ids_map_.erase(table.second);
    }
  }
  return Status::OK();
}

Status CatalogManager::ChangeXClusterRole(
    const ChangeXClusterRoleRequestPB* req,
    ChangeXClusterRoleResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing ChangeXClusterRole request from " << RequestorString(rpc) << ": "
            << req->ShortDebugString();
  return Status::OK();
}

Status CatalogManager::BootstrapProducer(
    const BootstrapProducerRequestPB* req,
    BootstrapProducerResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing BootstrapProducer request from " << RequestorString(rpc) << ": "
            << req->ShortDebugString();

  const bool pg_database_type = req->db_type() == YQL_DATABASE_PGSQL;
  SCHECK(
      pg_database_type || req->db_type() == YQL_DATABASE_CQL, InvalidArgument,
      "Invalid database type");
  SCHECK_PB_FIELDS_NOT_EMPTY(*req, namespace_name);
  SCHECK_GT(req->table_name_size(), 0, InvalidArgument, "No tables specified");
  if (pg_database_type) {
    SCHECK_EQ(
        req->pg_schema_name_size(), req->table_name_size(), InvalidArgument,
        "Number of tables and number of pg schemas must match");
  } else {
    SCHECK_EQ(
        req->pg_schema_name_size(), 0, InvalidArgument,
        "Pg Schema does not apply to CQL databases");
  }

  NamespaceIdentifierPB ns_id;
  ns_id.set_database_type(req->db_type());
  ns_id.set_name(req->namespace_name());
  auto ns = VERIFY_RESULT(FindNamespace(ns_id));
  auto all_tables = VERIFY_RESULT(GetTablesEligibleForXClusterReplication(*this, ns->id()));

  cdc::BootstrapProducerRequestPB bootstrap_req;
  master::TSDescriptor* ts = nullptr;
  for (int i = 0; i < req->table_name_size(); i++) {
    string pg_schema_name = pg_database_type ? req->pg_schema_name(i) : "";

    auto table_info_itr = std::find_if(
        all_tables.begin(), all_tables.end(),
        [&table_name = req->table_name(i), &pg_schema_name](const TableInfoPtr& table_info) {
          return table_info->name() == table_name && table_info->pgschema_name() == pg_schema_name;
        });
    SCHECK(
        table_info_itr != all_tables.end(), NotFound, Format("Table $0.$1$2 not found"),
        req->namespace_name(), (pg_schema_name.empty() ? "" : pg_schema_name + "."),
        req->table_name(i));

    auto& table_info = *table_info_itr;
    bootstrap_req.add_table_ids(table_info->id());
    resp->add_table_ids(table_info->id());

    // Pick a valid tserver to bootstrap from.
    if (!ts) {
      ts = VERIFY_RESULT(table_info->GetTablets().front()->GetLeader());
    }
  }
  SCHECK(ts, IllegalState, "No valid tserver found to bootstrap from");

  std::shared_ptr<cdc::CDCServiceProxy> proxy;
  RETURN_NOT_OK(ts->GetProxy(&proxy));

  cdc::BootstrapProducerResponsePB bootstrap_resp;
  rpc::RpcController bootstrap_rpc;
  bootstrap_rpc.set_deadline(rpc->GetClientDeadline());

  RETURN_NOT_OK(proxy->BootstrapProducer(bootstrap_req, &bootstrap_resp, &bootstrap_rpc));
  if (bootstrap_resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(bootstrap_resp.error().status()));
  }

  resp->mutable_bootstrap_ids()->Swap(bootstrap_resp.mutable_cdc_bootstrap_ids());
  if (bootstrap_resp.has_bootstrap_time()) {
    resp->set_bootstrap_time(bootstrap_resp.bootstrap_time());
  }

  return Status::OK();
}

Status CatalogManager::SetUniverseReplicationInfoEnabled(
    const xcluster::ReplicationGroupId& replication_group_id, bool is_enabled) {
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);

    universe = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (universe == nullptr) {
      return STATUS(
          NotFound, "Could not find CDC producer universe", replication_group_id,
          MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  // Update the Master's Universe Config with the new state.
  {
    auto l = universe->LockForWrite();
    if (l->pb.state() != SysUniverseReplicationEntryPB::DISABLED &&
        l->pb.state() != SysUniverseReplicationEntryPB::ACTIVE) {
      return STATUS(
          InvalidArgument,
          Format(
              "Universe Replication in invalid state: $0. Retry or Delete.",
              SysUniverseReplicationEntryPB::State_Name(l->pb.state())),
          replication_group_id,
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }
    if (is_enabled) {
      l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::ACTIVE);
    } else {
      // DISABLE.
      l.mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DISABLED);
    }
    RETURN_NOT_OK(CheckStatus(
        sys_catalog_->Upsert(leader_ready_term(), universe),
        "updating universe replication info in sys-catalog"));
    l.Commit();
  }
  return Status::OK();
}

Status CatalogManager::SetConsumerRegistryEnabled(
    const xcluster::ReplicationGroupId& replication_group_id, bool is_enabled,
    ClusterConfigInfo::WriteLock* l) {
  // Modify the Consumer Registry, which will fan out this info to all TServers on heartbeat.
  {
    auto replication_group_map =
        l->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    {
      auto it = replication_group_map->find(replication_group_id.ToString());
      if (it == replication_group_map->end()) {
        LOG(WARNING) << "Valid Producer Universe not in Consumer Registry: "
                     << replication_group_id;
        return STATUS(
            NotFound, "Could not find CDC producer universe", replication_group_id,
            MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
      }
      (*it).second.set_disable_stream(!is_enabled);
    }
  }
  return Status::OK();
}

Status CatalogManager::SetUniverseReplicationEnabled(
    const SetUniverseReplicationEnabledRequestPB* req,
    SetUniverseReplicationEnabledResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing SetUniverseReplicationEnabled request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  // Sanity Checking Cluster State and Input.
  if (!req->has_replication_group_id()) {
    return STATUS(
        InvalidArgument, "Producer universe ID must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }
  if (!req->has_is_enabled()) {
    return STATUS(
        InvalidArgument, "Must explicitly set whether to enable", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  const auto is_enabled = req->is_enabled();
  // When updating the cluster config, make sure that the change to the user replication and
  // system replication commit atomically by using the same lock.
  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForWrite();
  RETURN_NOT_OK(SetConsumerRegistryEnabled(
      xcluster::ReplicationGroupId(req->replication_group_id()), is_enabled, &l));
  l.mutable_data()->pb.set_version(l.mutable_data()->pb.version() + 1);
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
      "updating cluster config in sys-catalog"));
  l.Commit();

  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();

  return Status::OK();
}

Status CatalogManager::AlterUniverseReplication(
    const AlterUniverseReplicationRequestPB* req, AlterUniverseReplicationResponsePB* resp,
    rpc::RpcContext* rpc, const LeaderEpoch& epoch) {
  LOG(INFO) << "Servicing AlterUniverseReplication request from " << RequestorString(rpc) << ": "
            << req->ShortDebugString();

  SCHECK_PB_FIELDS_NOT_EMPTY(*req, replication_group_id);

  RETURN_NOT_OK(ValidateUniverseUUID(req, *this));

  auto replication_group_id = xcluster::ReplicationGroupId(req->replication_group_id());
  auto original_ri = GetUniverseReplication(replication_group_id);
  SCHECK_EC_FORMAT(
      original_ri, NotFound, MasterError(MasterErrorPB::OBJECT_NOT_FOUND),
      "Could not find xCluster replication group $0", replication_group_id);

  // Currently, config options are mutually exclusive to simplify transactionality.
  int config_count = (req->producer_master_addresses_size() > 0 ? 1 : 0) +
                     (req->producer_table_ids_to_remove_size() > 0 ? 1 : 0) +
                     (req->producer_table_ids_to_add_size() > 0 ? 1 : 0) +
                     (req->has_new_replication_group_id() ? 1 : 0) +
                     (!req->producer_namespace_id_to_remove().empty() ? 1 : 0);
  SCHECK_EC_FORMAT(
      config_count == 1, InvalidArgument, MasterError(MasterErrorPB::INVALID_REQUEST),
      "Only 1 Alter operation per request currently supported: $0", req->ShortDebugString());

  if (req->producer_master_addresses_size() > 0) {
    return UpdateProducerAddress(original_ri, req);
  }

  if (req->has_producer_namespace_id_to_remove()) {
    return RemoveNamespaceFromReplicationGroup(
        original_ri, req->producer_namespace_id_to_remove(), *this, epoch);
  }

  if (req->producer_table_ids_to_remove_size() > 0) {
    std::vector<TableId> table_ids(
        req->producer_table_ids_to_remove().begin(), req->producer_table_ids_to_remove().end());
    return master::RemoveTablesFromReplicationGroup(original_ri, table_ids, *this, epoch);
  }

  if (req->producer_table_ids_to_add_size() > 0) {
    RETURN_NOT_OK(AddTablesToReplication(original_ri, req, resp, rpc));
    xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();
    return Status::OK();
  }

  if (req->has_new_replication_group_id()) {
    return RenameUniverseReplication(original_ri, req);
  }

  return Status::OK();
}

Status CatalogManager::UpdateProducerAddress(
    scoped_refptr<UniverseReplicationInfo> universe, const AlterUniverseReplicationRequestPB* req) {
  CHECK_GT(req->producer_master_addresses_size(), 0);

  // TODO: Verify the input. Setup an RPC Task, ListTables, ensure same.

  {
    // 1a. Persistent Config: Update the Universe Config for Master.
    auto l = universe->LockForWrite();
    l.mutable_data()->pb.mutable_producer_master_addresses()->CopyFrom(
        req->producer_master_addresses());

    // 1b. Persistent Config: Update the Consumer Registry (updates TServers)
    auto cluster_config = ClusterConfig();
    auto cl = cluster_config->LockForWrite();
    auto replication_group_map =
        cl.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    auto it = replication_group_map->find(req->replication_group_id());
    if (it == replication_group_map->end()) {
      LOG(WARNING) << "Valid Producer Universe not in Consumer Registry: "
                   << req->replication_group_id();
      return STATUS(
          NotFound, "Could not find CDC producer universe", req->ShortDebugString(),
          MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
    it->second.mutable_master_addrs()->CopyFrom(req->producer_master_addresses());
    cl.mutable_data()->pb.set_version(cl.mutable_data()->pb.version() + 1);

    {
      // Need both these updates to be atomic.
      auto w = sys_catalog_->NewWriter(leader_ready_term());
      RETURN_NOT_OK(w->Mutate<true>(
          QLWriteRequestPB::QL_STMT_UPDATE, universe.get(), cluster_config.get()));
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->SyncWrite(w.get()),
          "Updating universe replication info and cluster config in sys-catalog"));
    }
    l.Commit();
    cl.Commit();
  }

  // 2. Memory Update: Change xcluster_rpc_tasks (Master cache)
  {
    auto result = universe->GetOrCreateXClusterRpcTasks(req->producer_master_addresses());
    if (!result.ok()) {
      return result.status();
    }
  }

  return Status::OK();
}

Status CatalogManager::AddTablesToReplication(
    scoped_refptr<UniverseReplicationInfo> universe, const AlterUniverseReplicationRequestPB* req,
    AlterUniverseReplicationResponsePB* resp, rpc::RpcContext* rpc) {
  SCHECK_GT(req->producer_table_ids_to_add_size(), 0, InvalidArgument, "No tables specified");

  if (universe->IsDbScoped()) {
    // We either add the entire namespace at once, or one table at a time as they get created.
    if (req->has_producer_namespace_to_add()) {
      SCHECK(
          !req->producer_namespace_to_add().id().empty(), InvalidArgument, "Invalid Namespace Id");
      SCHECK(
          !req->producer_namespace_to_add().name().empty(), InvalidArgument,
          "Invalid Namespace name");
      SCHECK_EQ(
          req->producer_namespace_to_add().database_type(), YQLDatabase::YQL_DATABASE_PGSQL,
          InvalidArgument, "Invalid Namespace database_type");
    } else {
      SCHECK_EQ(
          req->producer_table_ids_to_add_size(), 1, InvalidArgument,
          "When adding more than table to a DB scoped replication the namespace info must also be "
          "provided");
    }
  } else {
    SCHECK(
        !req->has_producer_namespace_to_add(), InvalidArgument,
        "Cannot add namespaces to non DB scoped replication");
  }

  xcluster::ReplicationGroupId alter_replication_group_id(xcluster::GetAlterReplicationGroupId(
      xcluster::ReplicationGroupId(req->replication_group_id())));

  // If user passed in bootstrap ids, check that there is a bootstrap id for every table.
  SCHECK(
      req->producer_bootstrap_ids_to_add_size() == 0 ||
          req->producer_table_ids_to_add_size() == req->producer_bootstrap_ids_to_add().size(),
      InvalidArgument, "Number of bootstrap ids must be equal to number of tables",
      req->ShortDebugString());

  // Verify no 'alter' command running.
  scoped_refptr<UniverseReplicationInfo> alter_ri;
  {
    SharedLock lock(mutex_);
    alter_ri = FindPtrOrNull(universe_replication_map_, alter_replication_group_id);
  }
  {
    if (alter_ri != nullptr) {
      LOG(INFO) << "Found " << alter_replication_group_id << "... Removing";
      if (alter_ri->LockForRead()->is_deleted_or_failed()) {
        // Delete previous Alter if it's completed but failed.
        master::DeleteUniverseReplicationRequestPB delete_req;
        delete_req.set_replication_group_id(alter_ri->id());
        master::DeleteUniverseReplicationResponsePB delete_resp;
        Status s = DeleteUniverseReplication(&delete_req, &delete_resp, rpc);
        if (!s.ok()) {
          if (delete_resp.has_error()) {
            resp->mutable_error()->Swap(delete_resp.mutable_error());
            return s;
          }
          return SetupError(resp->mutable_error(), s);
        }
      } else {
        return STATUS(
            InvalidArgument, "Alter for CDC producer currently running", req->ShortDebugString(),
            MasterError(MasterErrorPB::INVALID_REQUEST));
      }
    }
  }

  // Map each table id to its corresponding bootstrap id.
  std::unordered_map<TableId, std::string> table_id_to_bootstrap_id;
  if (req->producer_bootstrap_ids_to_add().size() > 0) {
    for (int i = 0; i < req->producer_table_ids_to_add().size(); i++) {
      table_id_to_bootstrap_id[req->producer_table_ids_to_add(i)] =
          req->producer_bootstrap_ids_to_add(i);
    }

    // Ensure that table ids are unique. We need to do this here even though
    // the same check is performed by SetupUniverseReplication because
    // duplicate table ids can cause a bootstrap id entry in table_id_to_bootstrap_id
    // to be overwritten.
    if (table_id_to_bootstrap_id.size() !=
        implicit_cast<size_t>(req->producer_table_ids_to_add().size())) {
      return STATUS(
          InvalidArgument,
          "When providing bootstrap ids, "
          "the list of tables must be unique",
          req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
    }
  }

  // Only add new tables.  Ignore tables that are currently being replicated.
  auto tid_iter = req->producer_table_ids_to_add();
  std::unordered_set<string> new_tables(tid_iter.begin(), tid_iter.end());
  auto original_universe_l = universe->LockForRead();
  auto& original_universe_pb = original_universe_l->pb;

  for (const auto& table_id : original_universe_pb.tables()) {
    new_tables.erase(table_id);
  }
  if (new_tables.empty()) {
    return STATUS(
        InvalidArgument, "CDC producer already contains all requested tables",
        req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  // 1. create an ALTER table request that mirrors the original 'setup_replication'.
  master::SetupUniverseReplicationRequestPB setup_req;
  master::SetupUniverseReplicationResponsePB setup_resp;
  setup_req.set_replication_group_id(alter_replication_group_id.ToString());
  setup_req.mutable_producer_master_addresses()->CopyFrom(
      original_universe_pb.producer_master_addresses());
  setup_req.set_transactional(original_universe_pb.transactional());

  if (req->has_producer_namespace_to_add()) {
    *setup_req.add_producer_namespaces() = req->producer_namespace_to_add();
  }

  for (const auto& table_id : new_tables) {
    setup_req.add_producer_table_ids(table_id);

    // Add bootstrap id to request if it exists.
    auto bootstrap_id = FindOrNull(table_id_to_bootstrap_id, table_id);
    if (bootstrap_id) {
      setup_req.add_producer_bootstrap_ids(*bootstrap_id);
    }
  }

  // 2. run the 'setup_replication' pipeline on the ALTER Table
  Status s = SetupUniverseReplication(&setup_req, &setup_resp, rpc);
  if (!s.ok()) {
    if (setup_resp.has_error()) {
      resp->mutable_error()->Swap(setup_resp.mutable_error());
      return s;
    }
    return SetupError(resp->mutable_error(), s);
  }

  return Status::OK();
}

Status CatalogManager::RenameUniverseReplication(
    scoped_refptr<UniverseReplicationInfo> universe, const AlterUniverseReplicationRequestPB* req) {
  CHECK(req->has_new_replication_group_id());

  const xcluster::ReplicationGroupId old_replication_group_id(universe->id());
  const xcluster::ReplicationGroupId new_replication_group_id(req->new_replication_group_id());
  if (old_replication_group_id == new_replication_group_id) {
    return STATUS(
        InvalidArgument, "Old and new replication ids must be different", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  {
    LockGuard lock(mutex_);
    auto l = universe->LockForWrite();
    scoped_refptr<UniverseReplicationInfo> new_ri;

    // Assert that new_replication_name isn't already in use.
    if (FindPtrOrNull(universe_replication_map_, new_replication_group_id) != nullptr) {
      return STATUS(
          InvalidArgument, "New replication id is already in use", req->ShortDebugString(),
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }

    // Since the replication_group_id is used as the key, we need to create a new
    // UniverseReplicationInfo.
    new_ri = new UniverseReplicationInfo(new_replication_group_id);
    new_ri->mutable_metadata()->StartMutation();
    SysUniverseReplicationEntryPB* metadata = &new_ri->mutable_metadata()->mutable_dirty()->pb;
    metadata->CopyFrom(l->pb);
    metadata->set_replication_group_id(new_replication_group_id.ToString());

    // Also need to update internal maps.
    auto cluster_config = ClusterConfig();
    auto cl = cluster_config->LockForWrite();
    auto replication_group_map =
        cl.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    (*replication_group_map)[new_replication_group_id.ToString()] =
        std::move((*replication_group_map)[old_replication_group_id.ToString()]);
    replication_group_map->erase(old_replication_group_id.ToString());

    {
      // Need both these updates to be atomic.
      auto w = sys_catalog_->NewWriter(leader_ready_term());
      RETURN_NOT_OK(w->Mutate<true>(QLWriteRequestPB::QL_STMT_DELETE, universe.get()));
      RETURN_NOT_OK(
          w->Mutate<true>(QLWriteRequestPB::QL_STMT_UPDATE, new_ri.get(), cluster_config.get()));
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->SyncWrite(w.get()),
          "Updating universe replication info and cluster config in sys-catalog"));
    }
    new_ri->mutable_metadata()->CommitMutation();
    cl.Commit();

    // Update universe_replication_map after persistent data is saved.
    universe_replication_map_[new_replication_group_id] = new_ri;
    universe_replication_map_.erase(old_replication_group_id);
  }

  return Status::OK();
}

Status CatalogManager::GetUniverseReplication(
    const GetUniverseReplicationRequestPB* req, GetUniverseReplicationResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "GetUniverseReplication from " << RequestorString(rpc) << ": " << req->DebugString();

  if (!req->has_replication_group_id()) {
    return STATUS(
        InvalidArgument, "Producer universe ID must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);

    universe = FindPtrOrNull(
        universe_replication_map_, xcluster::ReplicationGroupId(req->replication_group_id()));
    if (universe == nullptr) {
      return STATUS(
          NotFound, "Could not find CDC producer universe", req->ShortDebugString(),
          MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  resp->mutable_entry()->CopyFrom(universe->LockForRead()->pb);
  return Status::OK();
}

/*
 * Checks if the universe replication setup has completed.
 * Returns Status::OK() if this call succeeds, and uses resp->done() to determine if the setup has
 * completed (either failed or succeeded). If the setup has failed, then resp->replication_error()
 * is also set. If it succeeds, replication_error() gets set to OK.
 */
Status CatalogManager::IsSetupUniverseReplicationDone(
    const IsSetupUniverseReplicationDoneRequestPB* req,
    IsSetupUniverseReplicationDoneResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "IsSetupUniverseReplicationDone from " << RequestorString(rpc) << ": "
            << req->DebugString();

  SCHECK_PB_FIELDS_NOT_EMPTY(*req, replication_group_id);

  auto is_operation_done = VERIFY_RESULT(master::IsSetupUniverseReplicationDone(
      xcluster::ReplicationGroupId(req->replication_group_id()), *this));

  resp->set_done(is_operation_done.done());
  StatusToPB(is_operation_done.status(), resp->mutable_replication_error());
  return Status::OK();
}

Status CatalogManager::IsSetupNamespaceReplicationWithBootstrapDone(
    const IsSetupNamespaceReplicationWithBootstrapDoneRequestPB* req,
    IsSetupNamespaceReplicationWithBootstrapDoneResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << Format(
      "IsSetupNamespaceReplicationWithBootstrapDone $0: $1", RequestorString(rpc),
      req->DebugString());

  SCHECK(req->has_replication_group_id(), InvalidArgument, "Replication group ID must be provided");
  const xcluster::ReplicationGroupId replication_group_id(req->replication_group_id());

  scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info;
  {
    SharedLock lock(mutex_);

    bootstrap_info = FindPtrOrNull(universe_replication_bootstrap_map_, replication_group_id);
    SCHECK(
        bootstrap_info != nullptr, NotFound,
        Format(
            "Could not find universe replication bootstrap $0", replication_group_id.ToString()));
  }

  // Terminal states are DONE or some failure state.
  {
    auto l = bootstrap_info->LockForRead();
    resp->set_state(l->state());

    if (l->is_done()) {
      resp->set_done(true);
      StatusToPB(Status::OK(), resp->mutable_bootstrap_error());
      return Status::OK();
    }

    if (l->is_deleted_or_failed()) {
      resp->set_done(true);

      if (!bootstrap_info->GetReplicationBootstrapErrorStatus().ok()) {
        StatusToPB(
            bootstrap_info->GetReplicationBootstrapErrorStatus(), resp->mutable_bootstrap_error());
      } else {
        LOG(WARNING) << "Did not find setup universe replication bootstrap error status.";
        StatusToPB(STATUS(InternalError, "unknown error"), resp->mutable_bootstrap_error());
      }

      // Add failed bootstrap to GC now that we've responded to the user.
      {
        LockGuard lock(mutex_);
        replication_bootstraps_to_clear_.push_back(bootstrap_info->ReplicationGroupId());
      }

      return Status::OK();
    }
  }

  // Not done yet.
  resp->set_done(false);
  return Status::OK();
}

Status CatalogManager::UpdateConsumerOnProducerSplit(
    const UpdateConsumerOnProducerSplitRequestPB* req,
    UpdateConsumerOnProducerSplitResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "UpdateConsumerOnProducerSplit from " << RequestorString(rpc) << ": "
            << req->DebugString();

  if (!req->has_replication_group_id()) {
    return STATUS(
        InvalidArgument, "Producer universe ID must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }
  if (!req->has_stream_id()) {
    return STATUS(
        InvalidArgument, "Stream ID must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }
  if (!req->has_producer_split_tablet_info()) {
    return STATUS(
        InvalidArgument, "Producer split tablet info must be provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForWrite();
  auto replication_group_map =
      l.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
  auto producer_entry = FindOrNull(*replication_group_map, req->replication_group_id());
  if (!producer_entry) {
    return STATUS_FORMAT(
        NotFound, "Unable to find the producer entry for universe $0", req->replication_group_id());
  }
  auto stream_entry = FindOrNull(*producer_entry->mutable_stream_map(), req->stream_id());
  if (!stream_entry) {
    return STATUS_FORMAT(
        NotFound, "Unable to find the stream entry for universe $0, stream $1",
        req->replication_group_id(), req->stream_id());
  }

  SplitTabletIds split_tablet_id{
      .source = req->producer_split_tablet_info().tablet_id(),
      .children = {
          req->producer_split_tablet_info().new_tablet1_id(),
          req->producer_split_tablet_info().new_tablet2_id()}};

  auto split_key = req->producer_split_tablet_info().split_partition_key();
  auto consumer_tablet_keys = VERIFY_RESULT(GetTableKeyRanges(stream_entry->consumer_table_id()));
  bool found_source = false, found_all_split_children = false;
  RETURN_NOT_OK(UpdateTabletMappingOnProducerSplit(
      consumer_tablet_keys, split_tablet_id, split_key, &found_source, &found_all_split_children,
      stream_entry));

  if (!found_source) {
    // Did not find the source tablet, but did find the children - means that we have already
    // processed this SPLIT_OP, so for idempotency, we can return OK.
    if (found_all_split_children) {
      LOG(INFO) << "Already processed this tablet split: " << req->DebugString();
      return Status::OK();
    }

    // When there are sequential SPLIT_OPs, we may try to reprocess an older SPLIT_OP. However, if
    // one or both of those children have also already been split and processed, then we'll end up
    // here (!found_source && !found_all_split_childs).
    // This is alright, we can log a warning, and then continue (to not block later records).
    LOG(WARNING) << "Unable to find matching source tablet "
                 << req->producer_split_tablet_info().tablet_id() << " for universe "
                 << req->replication_group_id() << " stream " << req->stream_id();

    return Status::OK();
  }

  // Also bump the cluster_config_ version so that changes are propagated to tservers (and new
  // pollers are created for the new tablets).
  l.mutable_data()->pb.set_version(l.mutable_data()->pb.version() + 1);

  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
      "Updating cluster config in sys-catalog"));

  SyncXClusterConsumerReplicationStatusMap(
      xcluster::ReplicationGroupId(req->replication_group_id()), *replication_group_map);
  l.Commit();

  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();

  return Status::OK();
}

// Related function: PlayChangeMetadataRequest() in tablet_bootstrap.cc.
Status CatalogManager::UpdateConsumerOnProducerMetadata(
    const UpdateConsumerOnProducerMetadataRequestPB* req,
    UpdateConsumerOnProducerMetadataResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG_WITH_FUNC(INFO) << " from " << RequestorString(rpc) << ": " << req->DebugString();

  if (PREDICT_FALSE(GetAtomicFlag(&FLAGS_xcluster_skip_schema_compatibility_checks_on_alter))) {
    resp->set_should_wait(false);
    return Status::OK();
  }

  const xcluster::ReplicationGroupId replication_group_id(req->replication_group_id());
  const auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));

  // Get corresponding local data for this stream.
  TableId consumer_table_id;
  scoped_refptr<TableInfo> table;
  {
    SharedLock lock(mutex_);
    auto iter = std::find_if(
        xcluster_consumer_table_stream_ids_map_.begin(),
        xcluster_consumer_table_stream_ids_map_.end(),
        [&replication_group_id, &stream_id](auto& id_map) {
          return ContainsKeyValuePair(id_map.second, replication_group_id, stream_id);
        });
    SCHECK(
        iter != xcluster_consumer_table_stream_ids_map_.end(), NotFound,
        Format("Unable to find the stream id $0", stream_id));
    consumer_table_id = iter->first;

    // The destination table should be found or created by now.
    table = tables_->FindTableOrNull(consumer_table_id);
  }
  SCHECK(table, NotFound, Format("Missing table id $0", consumer_table_id));

  // Use the stream ID to find ClusterConfig entry
  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForWrite();
  auto replication_group_map =
      l.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
  auto producer_entry = FindOrNull(*replication_group_map, replication_group_id.ToString());
  SCHECK(producer_entry, NotFound, Format("Missing replication group $0", replication_group_id));
  auto stream_entry = FindOrNull(*producer_entry->mutable_stream_map(), stream_id.ToString());
  SCHECK(
      stream_entry, NotFound,
      Format("Missing replication group $0, stream $1", replication_group_id, stream_id));
  auto schema_cached = stream_entry->mutable_producer_schema();
  // Clear out any cached schema version
  schema_cached->Clear();

  cdc::SchemaVersionsPB* schema_versions_pb = nullptr;
  bool schema_versions_updated = false;

  // TODO (#16557): Support remove_table_id() for colocated tables / tablegroups.
  if (IsColocationParentTableId(consumer_table_id) && req->colocation_id() != kColocationIdNotSet) {
    auto map = stream_entry->mutable_colocated_schema_versions();
    schema_versions_pb = FindOrNull(*map, req->colocation_id());
    if (nullptr == schema_versions_pb) {
      // If the colocation_id itself does not exist, it needs to be recorded in clusterconfig.
      // This is to handle the case where source-target schema version mapping is 0:0.
      schema_versions_updated = true;
      schema_versions_pb = &((*map)[req->colocation_id()]);
    }
  } else {
    schema_versions_pb = stream_entry->mutable_schema_versions();
  }

  SchemaVersion current_producer_schema_version =
      schema_versions_pb->current_producer_schema_version();
  SchemaVersion current_consumer_schema_version =
      schema_versions_pb->current_consumer_schema_version();
  SchemaVersion old_producer_schema_version =
      schema_versions_pb->old_producer_schema_version();
  SchemaVersion old_consumer_schema_version =
      schema_versions_pb->old_consumer_schema_version();

  // Incoming producer version is greater than anything we've seen before, update our cache.
  if (req->producer_schema_version() > current_producer_schema_version) {
    old_producer_schema_version = current_producer_schema_version;
    old_consumer_schema_version = current_consumer_schema_version;
    current_producer_schema_version = req->producer_schema_version();
    current_consumer_schema_version = req->consumer_schema_version();
    schema_versions_updated = true;
  } else if (req->producer_schema_version() < current_producer_schema_version) {
    // We are seeing an older schema version that we need to keep track of to handle old rows.
    if (req->producer_schema_version() > old_producer_schema_version) {
      old_producer_schema_version = req->producer_schema_version();
      old_consumer_schema_version = req->consumer_schema_version();
      schema_versions_updated = true;
    } else {
      // If we have already seen this producer schema version in the past, we can ignore it OR
      // We recieved an update from a different tablet, so consumer schema version should match
      // or we received a new consumer schema version than what was cached locally.
      DCHECK(req->producer_schema_version() < old_producer_schema_version ||
             req->consumer_schema_version() >= old_consumer_schema_version);
    }
  } else {
    // If we have already seen this producer schema version, then verify that the consumer schema
    // version matches what we saw from other tablets or we received a new one.
    // If we get an older schema version from the consumer, that's an indication that it
    // has not yet performed the ALTER and caught up to the latest schema version so fail the
    // request until it catches up to the latest schema version.
    SCHECK(req->consumer_schema_version() >= current_consumer_schema_version, InternalError,
        Format(
            "Received Older Consumer schema version $0 for replication group $1, table $2",
            req->consumer_schema_version(), replication_group_id, consumer_table_id));
  }

  schema_versions_pb->set_current_producer_schema_version(current_producer_schema_version);
  schema_versions_pb->set_current_consumer_schema_version(current_consumer_schema_version);
  schema_versions_pb->set_old_producer_schema_version(old_producer_schema_version);
  schema_versions_pb->set_old_consumer_schema_version(old_consumer_schema_version);

  if (schema_versions_updated) {
    // Bump the ClusterConfig version so we'll broadcast new schema versions.
    l.mutable_data()->pb.set_version(l.mutable_data()->pb.version() + 1);
    RETURN_NOT_OK(CheckStatus(sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
        "Updating cluster config in sys-catalog"));
    l.Commit();
  } else {
    // Make sure to release this lock, especially since we grab mutex_ again later.
    l.Unlock();
  }

  // Set the values for the response.
  auto resp_schema_versions = resp->mutable_schema_versions();
  resp_schema_versions->set_current_producer_schema_version(current_producer_schema_version);
  resp_schema_versions->set_current_consumer_schema_version(current_consumer_schema_version);
  resp_schema_versions->set_old_producer_schema_version(old_producer_schema_version);
  resp_schema_versions->set_old_consumer_schema_version(old_consumer_schema_version);

  LOG(INFO) << Format(
      "Updated the schema versions for table $0 with stream id $1, colocation id $2."
      "Current producer schema version:$3, current consumer schema version:$4 "
      "old producer schema version:$5, old consumer schema version:$6, replication group:$7",
      replication_group_id, stream_id, req->colocation_id(), current_producer_schema_version,
      current_consumer_schema_version, old_producer_schema_version, old_consumer_schema_version,
      replication_group_id);
  return Status::OK();
}

Status CatalogManager::WaitForReplicationDrain(
    const WaitForReplicationDrainRequestPB* req,
    WaitForReplicationDrainResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "WaitForReplicationDrain from " << RequestorString(rpc) << ": "
            << req->DebugString();
  if (req->stream_ids_size() == 0) {
    return STATUS(
        InvalidArgument, "No stream ID provided", req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }
  MicrosecondsInt64 target_time =
      req->has_target_time() ? req->target_time() : GetCurrentTimeMicros();
  if (!req->target_time()) {
    LOG(INFO) << "WaitForReplicationDrain: target_time unspecified. Default to " << target_time;
  }

  // Find all streams to check for replication drain.
  std::unordered_set<xrepl::StreamId> filter_stream_ids;
  for (auto& stream_id : req->stream_ids()) {
    filter_stream_ids.emplace(VERIFY_RESULT(xrepl::StreamId::FromString(stream_id)));
  }

  std::unordered_set<xrepl::StreamId> found_stream_ids;
  std::vector<CDCStreamInfoPtr> streams;
  {
    std::vector<CDCStreamInfoPtr> all_streams;
    GetAllCDCStreams(&all_streams);
    for (const auto& stream : all_streams) {
      if (!filter_stream_ids.contains(stream->StreamId())) {
        continue;
      }
      streams.push_back(stream);
      found_stream_ids.insert(stream->StreamId());
    }
  }

  // Verify that all specified stream_ids are found.
  std::ostringstream not_found_streams;
  for (const auto& stream_id : filter_stream_ids) {
    if (!found_stream_ids.contains(stream_id)) {
      not_found_streams << stream_id << ",";
    }
  }
  if (!not_found_streams.str().empty()) {
    string stream_ids = not_found_streams.str();
    stream_ids.pop_back();  // Remove the last comma.
    return STATUS(
        InvalidArgument, Format("Streams not found: $0", stream_ids), req->ShortDebugString(),
        MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  // Keep track of the drained (stream_id, tablet_id) tuples.
  std::unordered_set<StreamTabletIdPair, StreamTabletIdHash> drained_stream_tablet_ids;

  // Calculate deadline and interval for each CallReplicationDrain call to tservers.
  CoarseTimePoint deadline = rpc->GetClientDeadline();
  if (deadline == CoarseTimePoint::max()) {
    deadline = CoarseMonoClock::now() + MonoDelta::FromMilliseconds(FLAGS_master_rpc_timeout_ms);
  }
  auto timeout =
      MonoDelta::FromMilliseconds(GetAtomicFlag(&FLAGS_wait_replication_drain_retry_timeout_ms));

  while (true) {
    // 1. Construct the request to be sent to each tserver. Meanwhile, collect all tuples that
    //    are not marked as drained in previous iterations.
    std::unordered_set<StreamTabletIdPair, StreamTabletIdHash> undrained_stream_tablet_ids;
    std::unordered_map<std::shared_ptr<cdc::CDCServiceProxy>, cdc::CheckReplicationDrainRequestPB>
        proxy_to_request;
    for (const auto& stream : streams) {
      for (const auto& table_id : stream->table_id()) {
        auto table_info = VERIFY_RESULT(FindTableById(table_id));
        RSTATUS_DCHECK(table_info != nullptr, NotFound, "Table ID not found: " + table_id);

        for (const auto& tablet : table_info->GetTablets()) {
          // (1) If tuple is marked as drained in a previous iteration, skip it.
          // (2) Otherwise, check if it is drained in the current iteration.
          if (drained_stream_tablet_ids.contains({stream->StreamId(), tablet->id()})) {
            continue;
          }
          undrained_stream_tablet_ids.insert({stream->StreamId(), tablet->id()});

          // Update the relevant request. Skip if relevant tserver/proxy is not ready yet.
          auto ts_result = tablet->GetLeader();
          if (ts_result.ok()) {
            std::shared_ptr<cdc::CDCServiceProxy> proxy;
            auto s = (*ts_result)->GetProxy(&proxy);
            if (s.ok()) {
              auto& tablet_req = proxy_to_request[proxy];
              auto stream_info = tablet_req.add_stream_info();
              stream_info->set_stream_id(stream->id());
              stream_info->set_tablet_id(tablet->id());
            }
          }
        }
      }
    }

    // For testing tserver leadership changes.
    TEST_PAUSE_IF_FLAG(TEST_hang_wait_replication_drain);

    // 2. Call CheckReplicationDrain on each tserver.
    for (auto& proxy_request : proxy_to_request) {
      if (deadline - CoarseMonoClock::Now() <= timeout) {
        break;  // Too close to deadline.
      }
      auto& cdc_service = proxy_request.first;
      auto& tablet_req = proxy_request.second;
      tablet_req.set_target_time(target_time);
      cdc::CheckReplicationDrainResponsePB tablet_resp;
      rpc::RpcController tablet_rpc;
      tablet_rpc.set_timeout(timeout);

      Status s = cdc_service->CheckReplicationDrain(tablet_req, &tablet_resp, &tablet_rpc);
      if (!s.ok()) {
        LOG(WARNING) << "CheckReplicationDrain responded with non-ok status: " << s;
      } else if (tablet_resp.has_error()) {
        LOG(WARNING) << "CheckReplicationDrain responded with error: "
                     << tablet_resp.error().DebugString();
      } else {
        // Update the two lists of (stream ID, tablet ID) pairs.
        for (const auto& stream_info : tablet_resp.drained_stream_info()) {
          auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(stream_info.stream_id()));
          undrained_stream_tablet_ids.erase({stream_id, stream_info.tablet_id()});
          drained_stream_tablet_ids.insert({stream_id, stream_info.tablet_id()});
        }
      }
    }

    // 3. Check if all current undrained tuples are marked as drained, or it is too close
    //    to deadline. If so, prepare the response and terminate the loop.
    if (undrained_stream_tablet_ids.empty() || deadline - CoarseMonoClock::Now() <= timeout * 2) {
      std::ostringstream output_stream;
      output_stream << "WaitForReplicationDrain from " << RequestorString(rpc) << " finished.";
      if (!undrained_stream_tablet_ids.empty()) {
        output_stream << " Found undrained streams:";
      }

      for (const auto& [stream_id, table_id] : undrained_stream_tablet_ids) {
        output_stream << "\n\tStream: " << stream_id << ", Tablet: " << table_id;
        auto undrained_stream_info = resp->add_undrained_stream_info();
        undrained_stream_info->set_stream_id(stream_id.ToString());
        undrained_stream_info->set_tablet_id(table_id);
      }
      LOG(INFO) << output_stream.str();
      break;
    }
    SleepFor(timeout);
  }

  return Status::OK();
}

Status CatalogManager::SetupNSUniverseReplication(
    const SetupNSUniverseReplicationRequestPB* req,
    SetupNSUniverseReplicationResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "SetupNSUniverseReplication from " << RequestorString(rpc) << ": "
            << req->DebugString();

  SCHECK(
      req->has_replication_group_id() && !req->replication_group_id().empty(), InvalidArgument,
      "Producer universe ID must be provided");
  SCHECK(
      req->has_producer_ns_name() && !req->producer_ns_name().empty(), InvalidArgument,
      "Producer universe namespace name must be provided");
  SCHECK(
      req->has_producer_ns_type(), InvalidArgument,
      "Producer universe namespace type must be provided");
  SCHECK(
      req->producer_master_addresses_size() > 0, InvalidArgument,
      "Producer master address must be provided");

  std::string ns_name = req->producer_ns_name();
  YQLDatabase ns_type = req->producer_ns_type();
  switch (ns_type) {
    case YQLDatabase::YQL_DATABASE_CQL:
      break;
    case YQLDatabase::YQL_DATABASE_PGSQL:
      return STATUS(
          InvalidArgument, "YSQL not currently supported for namespace-level replication setup");
    default:
      return STATUS(InvalidArgument, Format("Unrecognized namespace type: $0", ns_type));
  }

  // 1. Find all producer tables with a name-matching consumer table. Ensure that no
  //    bootstrapping is required for these producer tables.
  std::vector<TableId> producer_tables;
  NamespaceIdentifierPB producer_namespace;
  NamespaceIdentifierPB consumer_namespace;
  // namespace_id will be filled in XClusterFindProducerConsumerOverlap.
  producer_namespace.set_name(ns_name);
  producer_namespace.set_database_type(ns_type);
  consumer_namespace.set_name(ns_name);
  consumer_namespace.set_database_type(ns_type);
  size_t num_non_matched_consumer_tables = 0;
  {
    std::vector<HostPort> hp;
    HostPortsFromPBs(req->producer_master_addresses(), &hp);
    std::string producer_addrs = HostPort::ToCommaSeparatedString(hp);
    auto xcluster_rpc = VERIFY_RESULT(XClusterRpcTasks::CreateWithMasterAddrs(
        xcluster::ReplicationGroupId(req->replication_group_id()), producer_addrs));
    producer_tables = VERIFY_RESULT(XClusterFindProducerConsumerOverlap(
        xcluster_rpc, &producer_namespace, &consumer_namespace, &num_non_matched_consumer_tables));

    // TODO: Remove this check after NS-level bootstrap is implemented.
    auto bootstrap_required =
        VERIFY_RESULT(xcluster_rpc->client()->IsBootstrapRequired(producer_tables));
    SCHECK(
        !bootstrap_required, IllegalState,
        Format("Producer tables under namespace $0 require bootstrapping.", ns_name));
  }
  SCHECK(
      !producer_tables.empty(), NotFound,
      Format(
          "No producer tables under namespace $0 can be set up for replication. Please make "
          "sure that there are at least one pair of (producer, consumer) table with matching "
          "name and schema in order to initialize the namespace-level replication.",
          ns_name));

  // 2. Setup universe replication for these producer tables.
  {
    SetupUniverseReplicationRequestPB setup_req;
    SetupUniverseReplicationResponsePB setup_resp;
    setup_req.set_replication_group_id(req->replication_group_id());
    setup_req.mutable_producer_master_addresses()->CopyFrom(req->producer_master_addresses());
    for (const auto& tid : producer_tables) {
      setup_req.add_producer_table_ids(tid);
    }
    auto s = SetupUniverseReplication(&setup_req, &setup_resp, rpc);
    if (!s.ok()) {
      if (setup_resp.has_error()) {
        resp->mutable_error()->Swap(setup_resp.mutable_error());
        return s;
      }
      return SetupError(resp->mutable_error(), s);
    }
  }

  // 3. Wait for the universe replication setup to finish.
  // TODO: Put all the following code in an async task to avoid this expensive wait.
  CoarseTimePoint deadline = rpc->GetClientDeadline();
  auto s = xcluster_manager_->WaitForSetupUniverseReplicationToFinish(
      xcluster::ReplicationGroupId(req->replication_group_id()), deadline);
  if (!s.ok()) {
    return SetupError(resp->mutable_error(), s);
  }

  // 4. Update the persisted data.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");
    universe = FindPtrOrNull(
        universe_replication_map_, xcluster::ReplicationGroupId(req->replication_group_id()));
    if (universe == nullptr) {
      return STATUS(
          NotFound, "Could not find universe after SetupUniverseReplication",
          req->ShortDebugString(), MasterError(MasterErrorPB::UNKNOWN_ERROR));
    }
  }
  auto l = universe->LockForWrite();
  l.mutable_data()->pb.set_is_ns_replication(true);
  l.mutable_data()->pb.mutable_producer_namespace()->CopyFrom(producer_namespace);
  l.mutable_data()->pb.mutable_consumer_namespace()->CopyFrom(consumer_namespace);
  l.Commit();

  // 5. Initialize in-memory entry and start the periodic task.
  {
    LockGuard lock(mutex_);
    auto& metadata =
        namespace_replication_map_[xcluster::ReplicationGroupId(req->replication_group_id())];
    if (num_non_matched_consumer_tables > 0) {
      // Start the periodic sync immediately.
      metadata.next_add_table_task_time =
          CoarseMonoClock::Now() +
          MonoDelta::FromSeconds(GetAtomicFlag(&FLAGS_ns_replication_sync_retry_secs));
    } else {
      // Delay the sync since there are currently no non-replicated consumer tables.
      metadata.next_add_table_task_time =
          CoarseMonoClock::Now() +
          MonoDelta::FromSeconds(GetAtomicFlag(&FLAGS_ns_replication_sync_backoff_secs));
    }
  }
  namespace_replication_enabled_.store(true, std::memory_order_release);

  return Status::OK();
}

// Sync xcluster_consumer_replication_error_map_ with the streams we have in our producer_map.
void CatalogManager::SyncXClusterConsumerReplicationStatusMap(
    const xcluster::ReplicationGroupId& replication_group_id,
    const google::protobuf::Map<std::string, cdc::ProducerEntryPB>& producer_map) {
  std::lock_guard lock(xcluster_consumer_replication_error_map_mutex_);

  if (producer_map.count(replication_group_id.ToString()) == 0) {
    // Replication group has been deleted.
    xcluster_consumer_replication_error_map_.erase(replication_group_id);
    return;
  }

  auto& producer_entry = producer_map.at(replication_group_id.ToString());

  for (auto& [_, stream_map] : producer_entry.stream_map()) {
    std::unordered_set<TabletId> all_producer_tablet_ids;
    for (auto& [_, producer_tablet_ids] : stream_map.consumer_producer_tablet_map()) {
      all_producer_tablet_ids.insert(
          producer_tablet_ids.tablets().begin(), producer_tablet_ids.tablets().end());
    }

    if (all_producer_tablet_ids.empty()) {
      if (xcluster_consumer_replication_error_map_.contains(replication_group_id)) {
        xcluster_consumer_replication_error_map_.at(replication_group_id)
            .erase(stream_map.consumer_table_id());
      }
      continue;
    }

    auto& consumer_error_map =
        xcluster_consumer_replication_error_map_[replication_group_id]
                                                [stream_map.consumer_table_id()];
    // Remove entries that are no longer part of replication.
    std::erase_if(consumer_error_map, [&all_producer_tablet_ids](const auto& entry) {
      return !all_producer_tablet_ids.contains(entry.first);
    });

    // Add new entries.
    for (const auto& producer_tablet_id : all_producer_tablet_ids) {
      if (!consumer_error_map.contains(producer_tablet_id)) {
        // Default to UNINITIALIZED error. Once the Pollers send the status, this will be updated.
        consumer_error_map[producer_tablet_id].error =
            ReplicationErrorPb::REPLICATION_ERROR_UNINITIALIZED;
      }
    }
  }
}

void CatalogManager::StoreXClusterConsumerReplicationStatus(
    const XClusterConsumerReplicationStatusPB& consumer_replication_status) {
  const auto& replication_group_id = consumer_replication_status.replication_group_id();

  std::lock_guard lock(xcluster_consumer_replication_error_map_mutex_);
  // Heartbeats can report stale entries. So we skip anything that is not in
  // xcluster_consumer_replication_error_map_.

  auto* replication_error_map = FindOrNull(
      xcluster_consumer_replication_error_map_, xcluster::ReplicationGroupId(replication_group_id));
  if (!replication_error_map) {
    VLOG_WITH_FUNC(2) << "Skipping deleted replication group " << replication_group_id;
    return;
  }

  for (const auto& table_status : consumer_replication_status.table_status()) {
    const auto& consumer_table_id = table_status.consumer_table_id();
    auto* consumer_table_map = FindOrNull(*replication_error_map, consumer_table_id);
    if (!consumer_table_map) {
      VLOG_WITH_FUNC(2) << "Skipping removed table " << consumer_table_id
                        << " in replication group " << replication_group_id;
      continue;
    }

    for (const auto& stream_tablet_status : table_status.stream_tablet_status()) {
      const auto& producer_tablet_id = stream_tablet_status.producer_tablet_id();
      auto* tablet_status_map = FindOrNull(*consumer_table_map, producer_tablet_id);
      if (!tablet_status_map) {
        VLOG_WITH_FUNC(2) << "Skipping removed tablet " << producer_tablet_id
                          << " in replication group " << replication_group_id;
        continue;
      }

      // Get status from highest term only. When consumer leaders move we may get stale status
      // from older leaders.
      if (tablet_status_map->consumer_term <= stream_tablet_status.consumer_term()) {
        DCHECK_NE(
            stream_tablet_status.error(), ReplicationErrorPb::REPLICATION_ERROR_UNINITIALIZED);
        tablet_status_map->consumer_term = stream_tablet_status.consumer_term();
        tablet_status_map->error = stream_tablet_status.error();
        VLOG_WITH_FUNC(2) << "Storing error for replication group: " << replication_group_id
                          << ", consumer table: " << consumer_table_id
                          << ", tablet: " << producer_tablet_id
                          << ", term: " << stream_tablet_status.consumer_term()
                          << ", error: " << ReplicationErrorPb_Name(stream_tablet_status.error());
      } else {
        VLOG_WITH_FUNC(2) << "Skipping stale error for  replication group: " << replication_group_id
                          << ", consumer table: " << consumer_table_id
                          << ", tablet: " << producer_tablet_id
                          << ", term: " << stream_tablet_status.consumer_term();
      }
    }
  }
}

Status CatalogManager::GetReplicationStatus(
    const GetReplicationStatusRequestPB* req, GetReplicationStatusResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "GetReplicationStatus from " << RequestorString(rpc) << ": " << req->DebugString();

  SharedLock lock(mutex_);
  yb::SharedLock l(xcluster_consumer_replication_error_map_mutex_);

  // If the 'replication_group_id' is given, only populate the status for the streams in that
  // ReplicationGroup. Otherwise, populate all the status for all groups.
  if (!req->replication_group_id().empty()) {
    return PopulateReplicationGroupErrors(
        xcluster::ReplicationGroupId(req->replication_group_id()), resp);
  }

  for (const auto& [replication_id, _] : xcluster_consumer_replication_error_map_) {
    RETURN_NOT_OK(PopulateReplicationGroupErrors(replication_id, resp));
  }

  return Status::OK();
}

PgReplicaIdentity GetReplicaIdentityFromRecordType(std::string record_type_name) {
  cdc::CDCRecordType record_type = cdc::CDCRecordType::CHANGE;
  CDCRecordType_Parse(record_type_name, &record_type);
  switch (record_type) {
    case cdc::CDCRecordType::ALL: FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::PG_FULL: return PgReplicaIdentity::FULL;
    case cdc::CDCRecordType::PG_DEFAULT: return PgReplicaIdentity::DEFAULT;
    case cdc::CDCRecordType::PG_NOTHING: return PgReplicaIdentity::NOTHING;
    case cdc::CDCRecordType::PG_CHANGE_OLD_NEW: FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::FULL_ROW_NEW_IMAGE: FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::MODIFIED_COLUMNS_OLD_AND_NEW_IMAGES:
      LOG(WARNING) << "The record type of the older stream does not have a corresponding replica "
                      "identity. Going forward with replica identity CHANGE.";
      FALLTHROUGH_INTENDED;
    case cdc::CDCRecordType::CHANGE: return PgReplicaIdentity::CHANGE;
    // This case should never be reached.
    default: return PgReplicaIdentity::CHANGE;
  }
}

Status CatalogManager::YsqlBackfillReplicationSlotNameToCDCSDKStream(
    const YsqlBackfillReplicationSlotNameToCDCSDKStreamRequestPB* req,
    YsqlBackfillReplicationSlotNameToCDCSDKStreamResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing YsqlBackfillReplicationSlotNameToCDCSDKStream request from "
            << RequestorString(rpc) << ": " << req->ShortDebugString();

  if (!FLAGS_ysql_yb_enable_replication_commands ||
      !FLAGS_ysql_yb_enable_replica_identity ||
      !FLAGS_enable_backfilling_cdc_stream_with_replication_slot) {
    RETURN_INVALID_REQUEST_STATUS("Backfilling replication slot name is disabled");
  }

  if (!req->has_stream_id() || !req->has_cdcsdk_ysql_replication_slot_name()) {
    RETURN_INVALID_REQUEST_STATUS(
        "Both CDC Stream ID and Replication slot name must be provided");
  }

  RETURN_NOT_OK(ReplicationSlotValidateName(req->cdcsdk_ysql_replication_slot_name()));

  auto replication_slot_name = ReplicationSlotName(req->cdcsdk_ysql_replication_slot_name());
  auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));

  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  }

  if (stream == nullptr || stream->LockForRead()->is_deleting()) {
    return STATUS(
        NotFound, "Could not find CDC stream", MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  auto namespace_id = stream->LockForRead()->namespace_id();
  auto ns = VERIFY_RESULT(FindNamespaceById(namespace_id));

  if (ns->database_type() != YQLDatabase::YQL_DATABASE_PGSQL) {
    RETURN_INVALID_REQUEST_STATUS(
        "Only CDCSDK streams created on PGSQL namespaces can have a replication slot name");
  }

  if (!stream->GetCdcsdkYsqlReplicationSlotName().empty()) {
    RETURN_INVALID_REQUEST_STATUS(
        "Cannot update the replication slot name of a CDCSDK stream");
  }

  LOG_WITH_FUNC(INFO) << "Valid request. Updating the replication slot name";
  {
    LockGuard lock(mutex_);

    if (cdcsdk_replication_slots_to_stream_map_.contains(replication_slot_name)) {
      return STATUS(
          AlreadyPresent, "A CDC stream with the replication slot name already exists",
          MasterError(MasterErrorPB::OBJECT_ALREADY_PRESENT));
    }

    auto stream_lock = stream->LockForWrite();
    auto& pb = stream_lock.mutable_data()->pb;

    pb.set_cdcsdk_ysql_replication_slot_name(req->cdcsdk_ysql_replication_slot_name());
    cdcsdk_replication_slots_to_stream_map_.insert_or_assign(replication_slot_name, stream_id);

    PgReplicaIdentity replica_identity;
    bool has_record_type =  false;
    for (const auto& option : pb.options()) {
      if (option.key() == cdc::kRecordType) {
        // Check if record type is a valid replica identity, if not assign replica
        // identity CHANGE.
        replica_identity = GetReplicaIdentityFromRecordType(option.value());
        has_record_type = true;
        break;
      }
    }
    // This should never happen.
    RSTATUS_DCHECK(
        has_record_type, NotFound, Format("Option record_type not present in stream $0"),
        stream_id);
    for(auto table_id : pb.table_id()) {
       pb.mutable_replica_identity_map()->insert({table_id, replica_identity});
    }

    // TODO(#22249): Set the plugin name for streams upgraded from older clusters.

    stream_lock.Commit();
  }

  return Status::OK();
}

Status CatalogManager::DisableDynamicTableAdditionOnCDCSDKStream(
    const DisableDynamicTableAdditionOnCDCSDKStreamRequestPB* req,
    DisableDynamicTableAdditionOnCDCSDKStreamResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DisableDynamicTableAdditionOnCDCSDKStream request from "
            << RequestorString(rpc) << ": " << req->ShortDebugString();

  if (!req->has_stream_id()) {
    RETURN_INVALID_REQUEST_STATUS("CDC Stream ID must be provided");
  }

  if (!FLAGS_cdcsdk_enable_dynamic_tables_disable_option) {
    RETURN_INVALID_REQUEST_STATUS(
        "Disabling addition of dynamic tables to CDC stream is disallowed in the middle of an "
        "upgrade. Finalize the upgrade and try again");
  }

  auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));

  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  }

  RETURN_NOT_OK(ValidateStreamForTableRemoval(stream));

  if (stream->IsDynamicTableAdditionDisabled()) {
    return STATUS(AlreadyPresent, "Dynamic table addition already disabled on the CDC stream");
  }

  // Disable dynamic table addition by setting the stream metadata field to true.
  {
    auto stream_lock = stream->LockForWrite();
    auto& pb = stream_lock.mutable_data()->pb;

    pb.set_cdcsdk_disable_dynamic_table_addition(true);

    RETURN_ACTION_NOT_OK(
        sys_catalog_->Upsert(leader_ready_term(), stream), "Updating CDC stream in system catalog");

    stream_lock.Commit();
  }

  LOG_WITH_FUNC(INFO) << "Successfully disabled dynamic table addition on CDC stream: "
                      << stream_id;

  return Status::OK();
}

Status CatalogManager::RemoveUserTableFromCDCSDKStream(
    const RemoveUserTableFromCDCSDKStreamRequestPB* req,
    RemoveUserTableFromCDCSDKStreamResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing RemoveUserTableFromCDCSDKStream request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  if (!req->has_stream_id() || !req->has_table_id()) {
    RETURN_INVALID_REQUEST_STATUS("Both CDC Stream ID and table ID must be provided");
  }

  if (!FLAGS_cdcsdk_enable_dynamic_table_addition_with_table_cleanup) {
    RETURN_INVALID_REQUEST_STATUS(
        "Removal of user table from CDC stream is disallowed in the middle of an "
        "upgrade. Finalize the upgrade and try again");
  }

  auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));
  auto table_id = req->table_id();

  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  }

  RETURN_NOT_OK(ValidateStreamForTableRemoval(stream));

  auto stream_ns_id = stream->LockForRead()->namespace_id();

  scoped_refptr<TableInfo> table;
  {
    SharedLock lock(mutex_);
    table = tables_->FindTableOrNull(table_id);
  }

  RETURN_NOT_OK(ValidateTableForRemovalFromCDCSDKStream(table, /* check_for_ineligibility */ true));

  auto table_ns_id = table->LockForRead()->namespace_id();
  if (table_ns_id != stream_ns_id) {
    RETURN_INVALID_REQUEST_STATUS("Stream and Table are not under the same namespace");
  }

  // Add to the 'cdcsdk_unprocessed_unqualified_tables_to_streams_' map which will be further
  // processed by the catalog manager bg thread.
  RETURN_NOT_OK(AddTableForRemovalFromCDCSDKStream({table_id}, stream));

  LOG_WITH_FUNC(INFO) << "Successfully added table " << table_id
                      << " to unqualified list for CDC stream: " << stream_id;

  return Status::OK();
}

Status CatalogManager::ValidateAndSyncCDCStateEntriesForCDCSDKStream(
    const ValidateAndSyncCDCStateEntriesForCDCSDKStreamRequestPB* req,
    ValidateAndSyncCDCStateEntriesForCDCSDKStreamResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing ValidateAndSyncCDCStateEntriesForCDCSDKStream request from "
            << RequestorString(rpc) << ": " << req->ShortDebugString();

  if (!req->has_stream_id()) {
    RETURN_INVALID_REQUEST_STATUS("CDC Stream ID must be provided");
  }

  auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));
  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  }

  RETURN_NOT_OK(ValidateStreamForTableRemoval(stream));

  std::unordered_set<TableId> tables_in_stream_metadata;
  {
    auto stream_lock = stream->LockForRead();
    tables_in_stream_metadata.reserve(stream_lock->table_id().size());
    for (const auto& table_id : stream_lock->table_id()) {
      tables_in_stream_metadata.insert(table_id);
    }
  }

  auto updated_state_table_entries =
      VERIFY_RESULT(SyncCDCStateTableEntries(stream_id, tables_in_stream_metadata));

  for (const auto& entry : updated_state_table_entries) {
    resp->add_updated_tablet_entries(entry.key.tablet_id);
  }

  LOG_WITH_FUNC(INFO)
      << "Successfully validated and synced cdc state table entries for CDC stream: " << stream_id;

  return Status::OK();
}

Status CatalogManager::RemoveTablesFromCDCSDKStream(
    const RemoveTablesFromCDCSDKStreamRequestPB* req, RemoveTablesFromCDCSDKStreamResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing RemoveTablesFromCDCSDKStream request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  if (!req->has_stream_id()) {
    RETURN_INVALID_REQUEST_STATUS(
        "Stream ID is requirred for removing tables from CDCSDK stream");
  }

  auto stream_id = VERIFY_RESULT(xrepl::StreamId::FromString(req->stream_id()));

  const auto& table_ids = req->table_ids();
  if (table_ids.empty()) {
    RETURN_INVALID_REQUEST_STATUS("No Table ID provided for removal from CDCSDK stream");
  }

  CDCStreamInfoPtr stream;
  {
    SharedLock lock(mutex_);
    stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  }

  RETURN_NOT_OK(ValidateStreamForTableRemoval(stream));

  std::unordered_set<TableId> valid_tables_for_removal;
  Status status;
  for (const auto& table_id : table_ids) {
    scoped_refptr<TableInfo> table;
    {
      SharedLock lock(mutex_);
      table = tables_->FindTableOrNull(table_id);
    }

    status = ValidateTableForRemovalFromCDCSDKStream(table, /* check_for_ineligibility */ true);
    if (!status.ok()) {
      // No need to return the non-ok status to the caller (Update Peers and Metrics), since it will
      // be retried if the state table entry is found again in next iteration.
      LOG(WARNING) << "Could not remove table: " << table_id << " from stream: " << req->stream_id()
                   << " : " << status.ToString();
      continue;
    }

    valid_tables_for_removal.insert(table_id);
  }

  // Add to the 'cdcsdk_unprocessed_unqualified_tables_to_streams_' map which will be further
  // processed by the catalog manager bg thread.
  RETURN_NOT_OK(AddTableForRemovalFromCDCSDKStream(valid_tables_for_removal, stream));

  if (!valid_tables_for_removal.empty()) {
    LOG_WITH_FUNC(INFO) << "Successfully added table " << AsString(valid_tables_for_removal)
                        << " to unqualified list for CDC stream: " << stream_id;
  }

  return Status::OK();
}

std::vector<SysUniverseReplicationEntryPB>
CatalogManager::GetAllXClusterUniverseReplicationInfos() {
  SharedLock lock(mutex_);
  std::vector<SysUniverseReplicationEntryPB> result;
  for (const auto& [_, universe_info] : universe_replication_map_) {
    auto l = universe_info->LockForRead();
    result.push_back(l->pb);
  }

  return result;
}

// Validate that the given replication slot name is valid.
// This function is a duplicate of the ReplicationSlotValidateName function from
// src/postgres/src/backend/replication/slot.c
Status CatalogManager::ReplicationSlotValidateName(const std::string& replication_slot_name) {
  if (replication_slot_name.empty()) {
    RETURN_INVALID_REQUEST_STATUS("Replication slot name cannot be empty");
  }

  // The 64 comes from the NAMEDATALEN constant in YSQL.
  if (replication_slot_name.size() >= 64) {
    RETURN_INVALID_REQUEST_STATUS("Replication slot name length must be < 64");
  }

  for (auto c : replication_slot_name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '_'))) {
      RETURN_INVALID_REQUEST_STATUS(
          "Replication slot names may only contain lower case letters, numbers, and the underscore "
          "character.");
    }
  }

  return Status::OK();
}

Status CatalogManager::TEST_CDCSDKFailCreateStreamRequestIfNeeded(const std::string& sync_point) {
  bool fail_create_cdc_stream_request = false;
  TEST_SYNC_POINT_CALLBACK(sync_point, &fail_create_cdc_stream_request);
  if (fail_create_cdc_stream_request) {
    return STATUS_FORMAT(Aborted, "Test failure for sync point $0.", sync_point);
  }
  return Status::OK();
}

Status CatalogManager::PopulateReplicationGroupErrors(
    const xcluster::ReplicationGroupId& replication_group_id,
    GetReplicationStatusResponsePB* resp) const {
  auto* replication_error_map =
      FindOrNull(xcluster_consumer_replication_error_map_, replication_group_id);
  SCHECK(
      replication_error_map, NotFound, "Could not find replication group $0",
      replication_group_id.ToString());

  for (const auto& [consumer_table_id, tablet_error_map] : *replication_error_map) {
    if (!xcluster_consumer_table_stream_ids_map_.contains(consumer_table_id) ||
        !xcluster_consumer_table_stream_ids_map_.at(consumer_table_id)
             .contains(replication_group_id)) {
      // This is not expected. The two maps should be kept in sync.
      LOG(DFATAL) << "xcluster_consumer_replication_error_map_ contains consumer table "
                  << consumer_table_id << " in replication group " << replication_group_id
                  << " but xcluster_consumer_table_stream_ids_map_ does not.";
      continue;
    }

    // Map from error to list of producer tablet IDs/Pollers reporting them.
    std::unordered_map<ReplicationErrorPb, std::vector<TabletId>> errors;
    for (const auto& [tablet_id, error_info] : tablet_error_map) {
      errors[error_info.error].push_back(tablet_id);
    }

    if (errors.empty()) {
      continue;
    }

    auto resp_status = resp->add_statuses();
    resp_status->set_table_id(consumer_table_id);
    const auto& stream_id =
        xcluster_consumer_table_stream_ids_map_.at(consumer_table_id).at(replication_group_id);
    resp_status->set_stream_id(stream_id.ToString());
    for (const auto& [error_pb, tablet_ids] : errors) {
      if (error_pb == ReplicationErrorPb::REPLICATION_OK) {
        // Do not report healthy tablets.
        continue;
      }

      auto* resp_error = resp_status->add_errors();
      resp_error->set_error(error_pb);
      // Only include the first 20 tablet IDs to limit response size. VLOG(4) will log all tablet to
      // the log.
      resp_error->set_error_detail(
          Format("Producer Tablet IDs: $0", JoinStringsLimitCount(tablet_ids, ",", 20)));
      if (VLOG_IS_ON(4)) {
        VLOG(4) << "Replication error " << ReplicationErrorPb_Name(error_pb)
                << " for ReplicationGroup: " << replication_group_id << ", stream id: " << stream_id
                << ", consumer table: " << consumer_table_id << ", producer tablet IDs:";
        for (const auto& tablet_id : tablet_ids) {
          VLOG(4) << tablet_id;
        }
      }
    }
  }

  return Status::OK();
}

bool CatalogManager::IsTablePartOfXRepl(const TableId& table_id) const {
  return IsTablePartOfXClusterUnlocked(table_id) || IsTablePartOfCDCSDK(table_id);
}

bool CatalogManager::IsTableXClusterConsumer(const TableId& table_id) const {
  SharedLock lock(mutex_);
  return IsTableXClusterConsumerUnlocked(table_id);
}

bool CatalogManager::IsTableXClusterConsumerUnlocked(const TableId& table_id) const {
  auto* stream_ids = FindOrNull(xcluster_consumer_table_stream_ids_map_, table_id);
  return stream_ids && !stream_ids->empty();
}

bool CatalogManager::IsTablePartOfCDCSDK(
    const TableId& table_id, bool require_replication_slot) const {
  DCHECK(xrepl_maps_loaded_);
  auto* stream_ids = FindOrNull(cdcsdk_tables_to_stream_map_, table_id);
  if (stream_ids) {
    for (const auto& stream_id : *stream_ids) {
      auto stream_info = FindPtrOrNull(cdc_stream_map_, stream_id);
      if (stream_info) {
        auto s = stream_info->LockForRead();
        if (!s->is_deleting() &&
            (!require_replication_slot || s->pb.has_cdcsdk_ysql_replication_slot_name())) {
          VLOG(1) << "Found an active CDCSDK stream: " << stream_id << " for table: " << table_id
                  << (require_replication_slot
                          ? " with replication slot: " + s->pb.cdcsdk_ysql_replication_slot_name()
                          : "");
          return true;
        }
      }
    }
  }

  return false;
}

std::unordered_set<xrepl::StreamId> CatalogManager::GetCDCSDKStreamsForTable(
    const TableId& table_id) const {
  SharedLock lock(mutex_);
  DCHECK(xrepl_maps_loaded_);
  auto table_ids = FindOrNull(cdcsdk_tables_to_stream_map_, table_id);
  if (!table_ids) {
    return {};
  }
  return *table_ids;
}

bool CatalogManager::IsTablePartOfXCluster(const TableId& table_id) const {
  SharedLock lock(mutex_);
  return IsTablePartOfXClusterUnlocked(table_id);
}

bool CatalogManager::IsTablePartOfXClusterUnlocked(const TableId& table_id) const {
  return xcluster_manager_->IsTableReplicated(table_id) ||
         IsTableXClusterConsumerUnlocked(table_id);
}

Status CatalogManager::ValidateNewSchemaWithCdc(
    const TableInfo& table_info, const Schema& consumer_schema) const {
  // Check if this table is consuming a stream.
  auto stream_ids = GetXClusterConsumerStreamIdsForTable(table_info.id());
  if (stream_ids.empty()) {
    return Status::OK();
  }

  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForRead();
  for (const auto& [replication_group_id, stream_id] : stream_ids) {
    // Fetch the stream entry to get Schema information.
    auto& replication_group_map = l.data().pb.consumer_registry().producer_map();
    auto producer_entry = FindOrNull(replication_group_map, replication_group_id.ToString());
    SCHECK(producer_entry, NotFound, Format("Missing universe $0", replication_group_id));
    auto stream_entry = FindOrNull(producer_entry->stream_map(), stream_id.ToString());
    SCHECK(stream_entry, NotFound, Format("Missing stream $0:$1", replication_group_id, stream_id));

    // If we are halted on a Schema update as a Consumer...
    auto& producer_schema_pb = stream_entry->producer_schema();
    if (producer_schema_pb.has_pending_schema()) {
      // Compare our new schema to the Producer's pending schema.
      Schema producer_schema;
      RETURN_NOT_OK(SchemaFromPB(producer_schema_pb.pending_schema(), &producer_schema));

      // This new schema should allow us to consume data for the Producer's next schema.
      // If we instead diverge, we will be unable to consume any more of the Producer's data.
      bool can_apply = consumer_schema.EquivalentForDataCopy(producer_schema);
      SCHECK(
          can_apply, IllegalState,
          Format(
              "New Schema not compatible with XCluster Producer Schema:\n new={$0}\n producer={$1}",
              consumer_schema.ToString(), producer_schema.ToString()));
    }
  }

  return Status::OK();
}

Status CatalogManager::ResumeXClusterConsumerAfterNewSchema(
    const TableInfo& table_info, SchemaVersion consumer_schema_version) {
  if (PREDICT_FALSE(!GetAtomicFlag(&FLAGS_xcluster_wait_on_ddl_alter))) {
    return Status::OK();
  }

  // Verify that this table is consuming a stream.
  auto stream_ids = GetXClusterConsumerStreamIdsForTable(table_info.id());
  if (stream_ids.empty()) {
    return Status::OK();
  }

  bool found_schema = false, resuming_replication = false;

  // Now that we've applied the new schema: find pending replication, clear state, resume.
  auto cluster_config = ClusterConfig();
  auto l = cluster_config->LockForWrite();
  for (const auto& [replication_group_id, stream_id] : stream_ids) {
    // Fetch the stream entry to get Schema information.
    auto replication_group_map =
        l.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    auto producer_entry = FindOrNull(*replication_group_map, replication_group_id.ToString());
    if (!producer_entry) {
      continue;
    }
    auto stream_entry = FindOrNull(*producer_entry->mutable_stream_map(), stream_id.ToString());
    if (!stream_entry) {
      continue;
    }

    auto producer_schema_pb = stream_entry->mutable_producer_schema();
    if (producer_schema_pb->has_pending_schema()) {
      found_schema = true;
      Schema consumer_schema, producer_schema;
      RETURN_NOT_OK(table_info.GetSchema(&consumer_schema));
      RETURN_NOT_OK(SchemaFromPB(producer_schema_pb->pending_schema(), &producer_schema));
      if (consumer_schema.EquivalentForDataCopy(producer_schema)) {
        resuming_replication = true;
        auto pending_version = producer_schema_pb->pending_schema_version();
        LOG(INFO) << "Consumer schema @ version " << consumer_schema_version
                  << " is now data copy compatible with Producer: " << stream_id
                  << " @ schema version " << pending_version;
        // Clear meta we use to track progress on receiving all WAL entries with old schema.
        producer_schema_pb->set_validated_schema_version(
            std::max(producer_schema_pb->validated_schema_version(), pending_version));
        producer_schema_pb->set_last_compatible_consumer_schema_version(consumer_schema_version);
        producer_schema_pb->clear_pending_schema();
        // Bump the ClusterConfig version so we'll broadcast new schema version & resume operation.
        l.mutable_data()->pb.set_version(l.mutable_data()->pb.version() + 1);
      } else {
        LOG(INFO) << "Consumer schema not compatible for data copy of next Producer schema.";
      }
    }
  }

  if (resuming_replication) {
    RETURN_NOT_OK(CheckStatus(
        sys_catalog_->Upsert(leader_ready_term(), cluster_config.get()),
        "updating cluster config after Schema for CDC"));
    l.Commit();
    LOG(INFO) << "Resuming Replication on " << table_info.id() << " after Consumer ALTER.";
  } else if (!found_schema) {
    LOG(INFO) << "No pending schema change from Producer.";
  }

  return Status::OK();
}

void CatalogManager::RunXReplBgTasks(const LeaderEpoch& epoch) {
  if (!FLAGS_TEST_cdcsdk_disable_deleted_stream_cleanup) {
    WARN_NOT_OK(CleanUpDeletedXReplStreams(epoch), "Failed Cleaning Deleted XRepl Streams");
  }

  // Clean up Failed Universes on the Consumer.
  WARN_NOT_OK(ClearFailedUniverse(), "Failed Clearing Failed Universe");

  // Clean up Failed Replication Bootstrap on the Consumer.
  WARN_NOT_OK(ClearFailedReplicationBootstrap(), "Failed Clearing Failed Replication Bootstrap");

  if (!FLAGS_TEST_cdcsdk_disable_drop_table_cleanup) {
    WARN_NOT_OK(CleanUpCDCSDKStreamsMetadata(epoch), "Failed Cleanup CDCSDK Streams Metadata");
  }

  // Restart xCluster and CDCSDK parent tablet deletion bg task.
  StartXReplParentTabletDeletionTaskIfStopped();

  // Run periodic task for namespace-level replications.
  ScheduleXClusterNSReplicationAddTableTask();

  WARN_NOT_OK(
      XClusterProcessPendingSchemaChanges(epoch),
      "Failed processing xCluster Pending Schema Changes");

  WARN_NOT_OK(
      XClusterRefreshLocalAutoFlagConfig(epoch), "Failed refreshing local AutoFlags config");
}

Status CatalogManager::XClusterProcessPendingSchemaChanges(const LeaderEpoch& epoch) {
  if (PREDICT_FALSE(!GetAtomicFlag(&FLAGS_xcluster_wait_on_ddl_alter))) {
    // See if any Streams are waiting on a pending_schema.
    bool found_pending_schema = false;
    auto cluster_config = ClusterConfig();
    auto cl = cluster_config->LockForWrite();
    auto replication_group_map =
        cl.mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    // For each user entry.
    for (auto& replication_group_id_and_entry : *replication_group_map) {
      // For each CDC stream in that Universe.
      for (auto& stream_id_and_entry :
           *replication_group_id_and_entry.second.mutable_stream_map()) {
        auto& stream_entry = stream_id_and_entry.second;
        if (stream_entry.has_producer_schema() &&
            stream_entry.producer_schema().has_pending_schema()) {
          // Force resume this stream.
          auto schema = stream_entry.mutable_producer_schema();
          schema->set_validated_schema_version(
              std::max(schema->validated_schema_version(), schema->pending_schema_version()));
          schema->clear_pending_schema();

          found_pending_schema = true;
          LOG(INFO) << "Force Resume Consumer schema: " << stream_id_and_entry.first
                    << " @ schema version " << schema->pending_schema_version();
        }
      }
    }

    if (found_pending_schema) {
      // Bump the ClusterConfig version so we'll broadcast new schema version & resume operation.
      cl.mutable_data()->pb.set_version(cl.mutable_data()->pb.version() + 1);
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->Upsert(epoch.leader_term, cluster_config.get()),
          "updating cluster config after Schema for CDC"));
      cl.Commit();
    }
  }

  return Status::OK();
}

Status CatalogManager::ClearFailedUniverse() {
  // Delete a single failed universe from universes_to_clear_.
  if (PREDICT_FALSE(FLAGS_disable_universe_gc)) {
    return Status::OK();
  }

  xcluster::ReplicationGroupId replication_group_id;
  {
    LockGuard lock(mutex_);

    if (universes_to_clear_.empty()) {
      return Status::OK();
    }
    // Get the first universe.  Only try once to avoid failure loops.
    replication_group_id = universes_to_clear_.front();
    universes_to_clear_.pop_front();
  }

  GetUniverseReplicationRequestPB universe_req;
  GetUniverseReplicationResponsePB universe_resp;
  universe_req.set_replication_group_id(replication_group_id.ToString());

  RETURN_NOT_OK(GetUniverseReplication(&universe_req, &universe_resp, /* RpcContext */ nullptr));

  DeleteUniverseReplicationRequestPB req;
  DeleteUniverseReplicationResponsePB resp;
  req.set_replication_group_id(replication_group_id.ToString());
  req.set_ignore_errors(true);

  RETURN_NOT_OK(DeleteUniverseReplication(&req, &resp, /* RpcContext */ nullptr));

  return Status::OK();
}

Status CatalogManager::DoClearFailedReplicationBootstrap(
    const CleanupFailedReplicationBootstrapInfo& info) {
  const auto& [
    state,
    xcluster_rpc_task,
    bootstrap_ids,
    old_snapshot_id,
    new_snapshot_id,
    namespace_map,
    type_map,
    tables_data,
    epoch
  ] = info;

  Status s = Status::OK();
  switch (state) {
    case SysUniverseReplicationBootstrapEntryPB_State_SETUP_REPLICATION:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_RESTORE_SNAPSHOT:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_TRANSFER_SNAPSHOT:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_CREATE_CONSUMER_SNAPSHOT: {
      if (!new_snapshot_id.IsNil()) {
        auto deadline = CoarseMonoClock::Now() + 30s;
        s = snapshot_coordinator_.Delete(new_snapshot_id, leader_ready_term(), deadline);
        if (!s.ok()) {
          LOG(WARNING) << Format("Failed to delete snapshot on consumer on status: $0", s);
        }
      }
    }
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_IMPORT_SNAPSHOT:
      DeleteNewSnapshotObjects(namespace_map, type_map, tables_data, epoch);
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_CREATE_PRODUCER_SNAPSHOT: {
      if (!old_snapshot_id.IsNil()) {
        DeleteSnapshotResponsePB resp;
        s = xcluster_rpc_task->client()->DeleteSnapshot(old_snapshot_id, &resp);
        if (!s.ok()) {
          LOG(WARNING) << Format(
              "Failed to send delete snapshot request to producer on status: $0", s);
        }
        if (resp.has_error()) {
          LOG(WARNING) << Format(
              "Failed to delete snapshot on producer with error: $0", resp.error());
        }
      }
    }
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_BOOTSTRAP_PRODUCER: {
      DeleteCDCStreamResponsePB resp;
      s = xcluster_rpc_task->client()->DeleteCDCStream(
          bootstrap_ids, /* force_delete = */ true, /* ignore_failures = */ false, &resp);
      if (!s.ok()) {
        LOG(WARNING) << Format(
            "Failed to send delete CDC streams request to producer on status: $0", s);
      }
      if (resp.has_error()) {
        LOG(WARNING) << Format(
            "Failed to delete CDC streams on producer with error: $0", resp.error());
      }
    }
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_INITIALIZING:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_DONE:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_FAILED:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_DELETED:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_DELETED_ERROR:
      FALLTHROUGH_INTENDED;
    case SysUniverseReplicationBootstrapEntryPB_State_DELETING:
      break;
  }
  return s;
}

Status CatalogManager::ClearFailedReplicationBootstrap() {
  xcluster::ReplicationGroupId replication_id;
  {
    LockGuard lock(mutex_);

    if (replication_bootstraps_to_clear_.empty()) {
      return Status::OK();
    }
    // Get the first bootstrap.  Only try once to avoid failure loops.
    replication_id = replication_bootstraps_to_clear_.front();
    replication_bootstraps_to_clear_.pop_front();
  }

  // First get the universe.
  scoped_refptr<UniverseReplicationBootstrapInfo> bootstrap_info;
  {
    SharedLock lock(mutex_);
    TRACE("Acquired catalog manager lock");

    bootstrap_info = FindPtrOrNull(universe_replication_bootstrap_map_, replication_id);
    if (bootstrap_info == nullptr) {
      auto error_msg =
          Format("UniverseReplicationBootstrap not found: $0", replication_id.ToString());
      LOG(ERROR) << error_msg;
      return STATUS(NotFound, error_msg);
    }
  }

  // Retrieve information required to cleanup replication bootstrap.
  CleanupFailedReplicationBootstrapInfo info;

  {
    auto l = bootstrap_info->LockForRead();
    info.state = l->failed_on();
    info.epoch = l->epoch();
    info.old_snapshot_id = l->old_snapshot_id();
    info.new_snapshot_id = l->new_snapshot_id();
    info.xcluster_rpc_task = VERIFY_RESULT(
        bootstrap_info->GetOrCreateXClusterRpcTasks(l->pb.producer_master_addresses()));

    for (const auto& entry : l->pb.table_bootstrap_ids()) {
      info.bootstrap_ids.emplace_back(VERIFY_RESULT(xrepl::StreamIdFromString(entry.second)));
    }

    l->set_into_namespace_map(&info.namespace_map);
    l->set_into_tables_data(&info.tables_data);
    l->set_into_ud_type_map(&info.type_map);
  }

  // Set sys catalog state to be DELETING.
  {
    auto l = bootstrap_info->LockForWrite();
    l.mutable_data()->pb.set_state(SysUniverseReplicationBootstrapEntryPB::DELETING);
    Status s = sys_catalog_->Upsert(leader_ready_term(), bootstrap_info);
    RETURN_NOT_OK(
        CheckLeaderStatus(s, "Updating delete universe replication info into sys-catalog"));
    TRACE("Wrote universe replication bootstrap info to sys-catalog");
    l.Commit();
  }

  // Start cleanup.
  auto l = bootstrap_info->LockForWrite();
  l.mutable_data()->pb.set_state(SysUniverseReplicationBootstrapEntryPB::DELETED);

  // Cleanup any objects created during the bootstrap process.
  WARN_NOT_OK(
      DoClearFailedReplicationBootstrap(info),
      "Failed to delete newly created objects in replication bootstrap");

  // Try to delete from sys catalog.
  RETURN_ACTION_NOT_OK(
      sys_catalog_->Delete(leader_ready_term(), bootstrap_info),
      Format("updating sys-catalog, replication_group_id: $0", bootstrap_info->id()));

  // Remove it from the map.
  LockGuard lock(mutex_);
  if (universe_replication_bootstrap_map_.erase(bootstrap_info->ReplicationGroupId()) < 1) {
    LOG(WARNING) << "Failed to remove replication info from map: replication_group_id: "
                 << bootstrap_info->id();
  }

  TRACE("Wrote universe replication bootstrap info to sys-catalog");
  l.Commit();

  return Status::OK();
}

void CatalogManager::StartXReplParentTabletDeletionTaskIfStopped() {
  if (GetAtomicFlag(&FLAGS_cdc_parent_tablet_deletion_task_retry_secs) <= 0) {
    // Task is disabled.
    return;
  }
  const bool is_already_running = xrepl_parent_tablet_deletion_task_running_.exchange(true);
  if (!is_already_running) {
    ScheduleXReplParentTabletDeletionTask();
  }
}

void CatalogManager::ScheduleXReplParentTabletDeletionTask() {
  int wait_time = GetAtomicFlag(&FLAGS_cdc_parent_tablet_deletion_task_retry_secs);
  if (wait_time <= 0) {
    // Task has been disabled.
    xrepl_parent_tablet_deletion_task_running_ = false;
    return;
  }

  // Submit to run async in diff thread pool, since this involves accessing cdc_state.
  cdc_parent_tablet_deletion_task_.Schedule(
      [this](const Status& status) {
        Status s = background_tasks_thread_pool_->SubmitFunc(
            std::bind(&CatalogManager::ProcessXReplParentTabletDeletionPeriodically, this));
        if (!s.IsOk()) {
          // Failed to submit task to the thread pool. Mark that the task is now no longer running.
          LOG(WARNING) << "Failed to schedule: ProcessXReplParentTabletDeletionPeriodically";
          xrepl_parent_tablet_deletion_task_running_ = false;
        }
      },
      wait_time * 1s);
}

void CatalogManager::ProcessXReplParentTabletDeletionPeriodically() {
  if (!CheckIsLeaderAndReady().IsOk()) {
    xrepl_parent_tablet_deletion_task_running_ = false;
    return;
  }
  WARN_NOT_OK(DoProcessCDCSDKTabletDeletion(), "Failed to run DoProcessCDCSdkTabletDeletion.");
  WARN_NOT_OK(
      xcluster_manager_->DoProcessHiddenTablets(),
      "Failed to run xCluster DoProcessHiddenTablets.");

  // Schedule the next iteration of the task.
  ScheduleXReplParentTabletDeletionTask();
}

Status CatalogManager::DoProcessCDCSDKTabletDeletion() {
  std::unordered_map<TabletId, HiddenReplicationParentTabletInfo> hidden_tablets;
  {
    SharedLock lock(mutex_);
    if (retained_by_cdcsdk_.empty()) {
      return Status::OK();
    }
    hidden_tablets = retained_by_cdcsdk_;
  }

  std::unordered_set<TabletId> tablets_to_delete;
  std::vector<cdc::CDCStateTableEntry> entries_to_update;
  std::vector<cdc::CDCStateTableKey> entries_to_delete;

  // Check cdc_state table to see if the children tablets are being polled.
  for (auto& [tablet_id, hidden_tablet] : hidden_tablets) {
    // If our parent tablet is still around, need to process that one first.
    const auto& parent_tablet_id = hidden_tablet.parent_tablet_id_;
    if (!parent_tablet_id.empty() && hidden_tablets.contains(parent_tablet_id)) {
      continue;
    }

    // For each hidden tablet, check if for each stream we have an entry in the mapping for them.
    const auto stream_ids = GetCDCSDKStreamsForTable(hidden_tablet.table_id_);

    size_t count_tablet_streams_to_delete = 0;
    size_t count_streams_already_deleted = 0;

    for (const auto& stream_id : stream_ids) {
      // Check parent entry, if it doesn't exist, then it was already deleted.
      // If the entry for the tablet does not exist, then we can go ahead with deletion of the
      // tablet.
      auto entry_opt = VERIFY_RESULT(cdc_state_table_->TryFetchEntry(
          {tablet_id, stream_id},
          cdc::CDCStateTableEntrySelector().IncludeCheckpoint().IncludeLastReplicationTime()));

      // This means we already deleted the entry for this stream in a previous iteration.
      if (!entry_opt) {
        VLOG(2) << "Did not find an entry corresponding to the tablet: " << tablet_id
                << ", and stream: " << stream_id << ", in the cdc_state table";
        ++count_streams_already_deleted;
        continue;
      }

      // We check if there is any stream where the CDCSDK client has started streaming from the
      // hidden tablet, if not we can delete the tablet. There are two ways to verify that the
      // client has not started streaming:
      // 1. The checkpoint is -1.-1 (which is the case when a stream is bootstrapped)
      // 2. The checkpoint is 0.0 and 'CdcLastReplicationTime' is Null (when the tablet was a
      // result of a tablet split, and was added to the cdc_state table when the tablet split is
      // initiated.)
      if (entry_opt->checkpoint) {
        auto& checkpoint = *entry_opt->checkpoint;

        if (checkpoint == OpId::Invalid() ||
            (checkpoint == OpId::Min() && !entry_opt->last_replication_time)) {
          VLOG(2) << "The stream: " << stream_id << ", is not active for tablet: " << tablet_id;
          count_tablet_streams_to_delete++;
          continue;
        }
      }

      // This means there was an active stream for the source tablet. In which case if we see
      // that all children tablet entries have started streaming, we can delete the parent
      // tablet.
      bool found_all_children = true;
      for (auto& child_tablet_id : hidden_tablet.split_tablets_) {
        auto entry_opt = VERIFY_RESULT(cdc_state_table_->TryFetchEntry(
            {child_tablet_id, stream_id},
            cdc::CDCStateTableEntrySelector().IncludeLastReplicationTime()));

        // Check CdcLastReplicationTime to ensure that there has been a poll for this tablet, or if
        // the split has been reported.
        if (!entry_opt || !entry_opt->last_replication_time) {
          VLOG(2) << "The stream: " << stream_id
                  << ", has not started polling for the child tablet: " << child_tablet_id
                  << ".Hence we will not delete the hidden parent tablet: " << tablet_id;
          found_all_children = false;
          break;
        }
      }
      if (found_all_children) {
        LOG(INFO) << "Deleting tablet " << tablet_id << " from stream " << stream_id
                  << ". Reason: Consumer finished processing parent tablet after split.";

        // Also delete the parent tablet from cdc_state for all completed streams.
        entries_to_delete.emplace_back(cdc::CDCStateTableKey{tablet_id, stream_id});
        count_tablet_streams_to_delete++;
      }
    }

    if (count_tablet_streams_to_delete + count_streams_already_deleted == stream_ids.size()) {
      tablets_to_delete.insert(tablet_id);
    }
  }

  Status s = cdc_state_table_->UpdateEntries(entries_to_update);
  if (!s.ok()) {
    LOG(ERROR) << "Unable to flush operations to update cdc streams: " << s;
    return s.CloneAndPrepend("Error updating cdc stream rows from cdc_state table");
  }

  s = cdc_state_table_->DeleteEntries(entries_to_delete);
  if (!s.ok()) {
    LOG(ERROR) << "Unable to flush operations to delete cdc streams: " << s;
    return s.CloneAndPrepend("Error deleting cdc stream rows from cdc_state table");
  }

  // Delete tablets from retained_by_cdcsdk_, CleanupHiddenTablets will do the
  // actual tablet deletion.
  {
    LockGuard lock(mutex_);
    for (const auto& tablet_id : tablets_to_delete) {
      retained_by_cdcsdk_.erase(tablet_id);
    }
  }

  return Status::OK();
}

std::shared_ptr<cdc::CDCServiceProxy> CatalogManager::GetCDCServiceProxy(RemoteTabletServer* ts) {
  auto ybclient = master_->cdc_state_client_future().get();
  auto hostport = HostPortFromPB(ts->DesiredHostPort(ybclient->cloud_info()));
  DCHECK(!hostport.host().empty());

  auto cdc_service = std::make_shared<cdc::CDCServiceProxy>(&ybclient->proxy_cache(), hostport);

  return cdc_service;
}

void CatalogManager::SetCDCServiceEnabled() { cdc_enabled_.store(true, std::memory_order_release); }

void CatalogManager::ScheduleXClusterNSReplicationAddTableTask() {
  if (!namespace_replication_enabled_.load(std::memory_order_acquire)) {
    return;
  }

  LockGuard lock(mutex_);
  for (auto& map_entry : namespace_replication_map_) {
    auto& metadata = map_entry.second;
    if (CoarseMonoClock::Now() <= metadata.next_add_table_task_time) {
      continue;
    }
    // Enqueue the async add table task, which involves syncing with producer and adding
    // tables to the existing replication.
    const auto& replication_group_id = map_entry.first;
    CoarseTimePoint deadline = CoarseMonoClock::Now() + MonoDelta::FromSeconds(60);
    auto s = background_tasks_thread_pool_->SubmitFunc(std::bind(
        &CatalogManager::XClusterAddTableToNSReplication, this, replication_group_id, deadline));
    if (!s.ok()) {
      // By not setting next_add_table_task_time, this enforces the task to be resheduled the
      // next time the background thread runs.
      LOG(WARNING) << "Failed to schedule: XClusterAddTableToNSReplication";
    } else {
      // Prevent new tasks from being scheduled when the current task is running.
      metadata.next_add_table_task_time = deadline;
    }
  }
}

void CatalogManager::XClusterAddTableToNSReplication(
    const xcluster::ReplicationGroupId& replication_group_id, CoarseTimePoint deadline) {
  // TODO: In ScopeExit, find a way to report non-OK task_status to user.
  bool has_non_replicated_consumer_table = true;
  Status task_status = Status::OK();
  auto scope_exit = ScopeExit([&, this] {
    LockGuard lock(mutex_);
    auto ns_replication_info = FindOrNull(namespace_replication_map_, replication_group_id);

    // Only update metadata if we are the most recent task for this universe.
    if (ns_replication_info && ns_replication_info->next_add_table_task_time == deadline) {
      auto& metadata = *ns_replication_info;
      // a. If there are error, emit to prometheus (TODO) and force another round of syncing.
      //    When there are too many consecutive errors, stop the task for a long period.
      // b. Else if there is non-replicated consumer table, force another round of syncing.
      // c. Else, stop the task temporarily.
      if (!task_status.ok()) {
        metadata.num_accumulated_errors++;
        if (metadata.num_accumulated_errors == 5) {
          metadata.num_accumulated_errors = 0;
          metadata.next_add_table_task_time =
              CoarseMonoClock::now() +
              MonoDelta::FromSeconds(GetAtomicFlag(&FLAGS_ns_replication_sync_error_backoff_secs));
        } else {
          metadata.next_add_table_task_time =
              CoarseMonoClock::now() +
              MonoDelta::FromSeconds(GetAtomicFlag(&FLAGS_ns_replication_sync_retry_secs));
        }
      } else {
        metadata.num_accumulated_errors = 0;
        metadata.next_add_table_task_time =
            CoarseMonoClock::now() +
            MonoDelta::FromSeconds(
                has_non_replicated_consumer_table
                    ? GetAtomicFlag(&FLAGS_ns_replication_sync_retry_secs)
                    : GetAtomicFlag(&FLAGS_ns_replication_sync_backoff_secs));
      }
    }
  });

  if (deadline - CoarseMonoClock::Now() <= 1ms || !CheckIsLeaderAndReady().ok()) {
    return;
  }

  // 1. Sync with producer to find new producer tables that can be added to the current
  //    replication, and verify that these tables do not require bootstrapping.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    SharedLock lock(mutex_);
    universe = FindPtrOrNull(universe_replication_map_, replication_group_id);
    if (universe == nullptr) {
      task_status = STATUS(NotFound, "Universe not found", replication_group_id);
      LOG_WITH_FUNC(WARNING) << task_status;
      return;
    }
  }
  std::vector<TableId> tables_to_add;
  task_status = XClusterNSReplicationSyncWithProducer(
      universe, &tables_to_add, &has_non_replicated_consumer_table);
  if (!task_status.ok()) {
    LOG_WITH_FUNC(WARNING) << "Error finding producer tables to add to universe " << universe->id()
                           << " : " << task_status;
    return;
  }
  if (tables_to_add.empty()) {
    return;
  }

  // 2. Run AlterUniverseReplication to add the new tables to the current replication.
  AlterUniverseReplicationRequestPB alter_req;
  AlterUniverseReplicationResponsePB alter_resp;
  alter_req.set_replication_group_id(replication_group_id.ToString());
  for (const auto& table : tables_to_add) {
    alter_req.add_producer_table_ids_to_add(table);
  }

  task_status = AlterUniverseReplication(
      &alter_req, &alter_resp, /* RpcContext */ nullptr, GetLeaderEpochInternal());
  if (task_status.ok() && alter_resp.has_error()) {
    task_status = StatusFromPB(alter_resp.error().status());
  }
  if (!task_status.ok()) {
    LOG_WITH_FUNC(WARNING) << "Unable to add producer tables to namespace-level replication: "
                           << task_status;
    return;
  }

  // 3. Wait for AlterUniverseReplication to finish.
  task_status = xcluster_manager_->WaitForSetupUniverseReplicationToFinish(
      xcluster::GetAlterReplicationGroupId(replication_group_id), deadline);
  if (!task_status.ok()) {
    LOG_WITH_FUNC(WARNING) << "Error while waiting for AlterUniverseReplication on "
                           << replication_group_id << " to complete: " << task_status;
    return;
  }
  LOG_WITH_FUNC(INFO) << "Tables added to namespace-level replication " << universe->id() << " : "
                      << alter_req.ShortDebugString();
}

Status CatalogManager::XClusterNSReplicationSyncWithProducer(
    scoped_refptr<UniverseReplicationInfo> universe,
    std::vector<TableId>* producer_tables_to_add,
    bool* has_non_replicated_consumer_table) {
  auto l = universe->LockForRead();
  size_t num_non_matched_consumer_tables = 0;

  // 1. Find producer tables with a name-matching consumer table.
  auto xcluster_rpc =
      VERIFY_RESULT(universe->GetOrCreateXClusterRpcTasks(l->pb.producer_master_addresses()));
  auto producer_namespace = l->pb.producer_namespace();
  auto consumer_namespace = l->pb.consumer_namespace();

  auto producer_tables = VERIFY_RESULT(XClusterFindProducerConsumerOverlap(
      xcluster_rpc, &producer_namespace, &consumer_namespace, &num_non_matched_consumer_tables));

  // 2. Filter out producer tables that are already in the replication.
  for (const auto& tid : producer_tables) {
    if (ContainsKey(l->pb.validated_tables(), tid)) {
      continue;
    }
    producer_tables_to_add->push_back(tid);
  }

  // 3. If all consumer tables have a name-matching producer tables, and there is no additional
  //    producer table to add to the replication, this means that all consumer tables are
  //    currently replicated and we can stop the periodic sync temporarily.
  *has_non_replicated_consumer_table =
      num_non_matched_consumer_tables > 0 || !producer_tables_to_add->empty();

  // 4. Finally, verify that all producer tables to be added do not require bootstrapping.
  // TODO: Remove this check after NS-level bootstrap is implemented.
  if (!producer_tables_to_add->empty()) {
    auto bootstrap_required =
        VERIFY_RESULT(xcluster_rpc->client()->IsBootstrapRequired(*producer_tables_to_add));
    if (bootstrap_required) {
      std::ostringstream ptable_stream;
      for (const auto& ptable : *producer_tables_to_add) {
        ptable_stream << ptable << ",";
      }
      std::string ptable_str = ptable_stream.str();
      ptable_str.pop_back();  // Remove the last comma.
      return STATUS(
          IllegalState,
          Format(
              "Producer tables [$0] require bootstrapping, which is not currently "
              "supported by the namespace-level replication setup.",
              ptable_str));
    }
  }
  return Status::OK();
}

Result<std::vector<TableId>> CatalogManager::XClusterFindProducerConsumerOverlap(
    std::shared_ptr<XClusterRpcTasks> producer_xcluster_rpc,
    NamespaceIdentifierPB* producer_namespace, NamespaceIdentifierPB* consumer_namespace,
    size_t* num_non_matched_consumer_tables) {
  // TODO: Add support for colocated (parent) tables. Currently they are not supported because
  // parent colocated tables are system tables and are therefore excluded by ListUserTables.
  SCHECK(producer_xcluster_rpc != nullptr, InternalError, "Producer CDC RPC is null");

  // 1. Find all producer tables. Also record the producer namespace ID.
  auto producer_tables = VERIFY_RESULT(producer_xcluster_rpc->client()->ListUserTables(
      *producer_namespace, true /* include_indexes */));
  SCHECK(
      !producer_tables.empty(), NotFound,
      "No producer table found under namespace " + producer_namespace->ShortDebugString());

  if (!producer_tables.empty()) {
    producer_namespace->set_id(producer_tables[0].namespace_id());
  }

  // 2. Find all consumer tables. Only collect the table names as we are doing name matching.
  //    Also record the consumer namespace ID.
  std::unordered_set<std::string> consumer_tables;
  {
    ListTablesRequestPB list_req;
    ListTablesResponsePB list_resp;
    list_req.add_relation_type_filter(USER_TABLE_RELATION);
    list_req.add_relation_type_filter(INDEX_TABLE_RELATION);
    list_req.mutable_namespace_()->CopyFrom(*consumer_namespace);

    auto s = ListTables(&list_req, &list_resp);
    std::ostringstream error_stream;
    if (!s.ok() || list_resp.has_error()) {
      error_stream << (!s.ok() ? s.ToString() : list_resp.error().status().message());
    }
    SCHECK(
        list_resp.tables_size() > 0, NotFound,
        Format(
            "No consumer table found under namespace $0. Error: $1",
            consumer_namespace->ShortDebugString(), error_stream.str()));
    for (const auto& table : list_resp.tables()) {
      auto table_name = Format(
          "$0.$1.$2",
          table.namespace_().name(),
          table.pgschema_name(),  // Empty for YCQL tables.
          table.name());
      consumer_tables.insert(table_name);
    }
    consumer_namespace->set_id(list_resp.tables(0).namespace_().id());
  }

  // 3. Find producer tables with a name-matching consumer table.
  std::vector<TableId> overlap_tables;
  for (const auto& table : producer_tables) {
    auto table_name = Format(
        "$0.$1.$2",
        table.namespace_name(),
        table.pgschema_name(),  // Empty for YCQL tables.
        table.table_name());
    if (consumer_tables.contains(table_name)) {
      overlap_tables.push_back(table.table_id());
      consumer_tables.erase(table_name);
    }
  }

  // 4. Count the number of consumer tables without a name-matching producer table.
  *num_non_matched_consumer_tables = consumer_tables.size();

  return overlap_tables;
}

Result<scoped_refptr<TableInfo>> CatalogManager::GetTableById(const TableId& table_id) const {
  return FindTableById(table_id);
}

Status CatalogManager::FillHeartbeatResponseCDC(
    const SysClusterConfigEntryPB& cluster_config,
    const TSHeartbeatRequestPB* req,
    TSHeartbeatResponsePB* resp) {
  if (cdc_enabled_.load(std::memory_order_acquire)) {
    resp->set_xcluster_enabled_on_producer(true);
  }

  if (cluster_config.has_consumer_registry()) {
    if (req->cluster_config_version() < cluster_config.version()) {
      const auto& consumer_registry = cluster_config.consumer_registry();
      resp->set_cluster_config_version(cluster_config.version());
      *resp->mutable_consumer_registry() = consumer_registry;
    }
  }

  RETURN_NOT_OK(xcluster_manager_->FillHeartbeatResponse(*req, resp));

  return Status::OK();
}

std::unordered_map<TableId, CatalogManager::XClusterConsumerTableStreamIds>
CatalogManager::GetXClusterConsumerTableStreams() const {
  SharedLock lock(mutex_);
  return xcluster_consumer_table_stream_ids_map_;
}

CatalogManager::XClusterConsumerTableStreamIds CatalogManager::GetXClusterConsumerStreamIdsForTable(
    const TableId& table_id) const {
  SharedLock lock(mutex_);
  auto* stream_ids = FindOrNull(xcluster_consumer_table_stream_ids_map_, table_id);
  if (!stream_ids) {
    return {};
  }

  return *stream_ids;
}

void CatalogManager::ClearXClusterConsumerTableStreams(
    const xcluster::ReplicationGroupId& replication_group_id,
    const std::set<TableId>& tables_to_clear) {
  LockGuard lock(mutex_);
  for (const auto& table_id : tables_to_clear) {
    if (xcluster_consumer_table_stream_ids_map_[table_id].erase(replication_group_id) < 1) {
      LOG(WARNING) << "Failed to remove consumer table from mapping. " << "table_id: " << table_id
                   << ": replication_group_id: " << replication_group_id;
    }
    if (xcluster_consumer_table_stream_ids_map_[table_id].empty()) {
      xcluster_consumer_table_stream_ids_map_.erase(table_id);
    }
  }
}

bool CatalogManager::CDCSDKShouldRetainHiddenTablet(const TabletId& tablet_id) {
  SharedLock read_lock(mutex_);
  return retained_by_cdcsdk_.contains(tablet_id);
}

Status CatalogManager::BumpVersionAndStoreClusterConfig(
    ClusterConfigInfo* cluster_config, ClusterConfigInfo::WriteLock* l) {
  l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->Upsert(leader_ready_term(), cluster_config),
      "updating cluster config in sys-catalog"));
  l->Commit();

  xcluster_manager_->CreateXClusterSafeTimeTableAndStartService();
  return Status::OK();
}

Status CatalogManager::ValidateTableSchemaForXCluster(
    const client::YBTableInfo& info, const SetupReplicationInfo& setup_info,
    GetTableSchemaResponsePB* resp) {
  bool is_ysql_table = info.table_type == client::YBTableType::PGSQL_TABLE_TYPE;
  if (setup_info.transactional && !GetAtomicFlag(&FLAGS_TEST_allow_ycql_transactional_xcluster) &&
      !is_ysql_table) {
    return STATUS_FORMAT(
        NotSupported, "Transactional replication is not supported for non-YSQL tables: $0",
        info.table_name.ToString());
  }

  // Get corresponding table schema on local universe.
  GetTableSchemaRequestPB req;

  auto* table = req.mutable_table();
  table->set_table_name(info.table_name.table_name());
  table->mutable_namespace_()->set_name(info.table_name.namespace_name());
  table->mutable_namespace_()->set_database_type(
      GetDatabaseTypeForTable(client::ClientToPBTableType(info.table_type)));

  // Since YSQL tables are not present in table map, we first need to list tables to get the table
  // ID and then get table schema.
  // Remove this once table maps are fixed for YSQL.
  ListTablesRequestPB list_req;
  ListTablesResponsePB list_resp;

  list_req.set_name_filter(info.table_name.table_name());
  Status status = ListTables(&list_req, &list_resp);
  SCHECK(
      status.ok() && !list_resp.has_error(), NotFound,
      Format("Error while listing table: $0", status.ToString()));

  const auto& source_schema = client::internal::GetSchema(info.schema);
  for (const auto& t : list_resp.tables()) {
    // Check that table name and namespace both match.
    if (t.name() != info.table_name.table_name() ||
        t.namespace_().name() != info.table_name.namespace_name()) {
      continue;
    }

    // Check that schema name matches for YSQL tables, if the field is empty, fill in that
    // information during GetTableSchema call later.
    bool has_valid_pgschema_name = !t.pgschema_name().empty();
    if (is_ysql_table && has_valid_pgschema_name &&
        t.pgschema_name() != source_schema.SchemaName()) {
      continue;
    }

    // Get the table schema.
    table->set_table_id(t.id());
    status = GetTableSchema(&req, resp);
    SCHECK(
        status.ok() && !resp->has_error(), NotFound,
        Format("Error while getting table schema: $0", status.ToString()));

    // Double-check schema name here if the previous check was skipped.
    if (is_ysql_table && !has_valid_pgschema_name) {
      std::string target_schema_name = resp->schema().pgschema_name();
      if (target_schema_name != source_schema.SchemaName()) {
        table->clear_table_id();
        continue;
      }
    }

    // Verify that the table on the target side supports replication.
    if (is_ysql_table && t.has_relation_type() && t.relation_type() == MATVIEW_TABLE_RELATION) {
      return STATUS_FORMAT(
          NotSupported, "Replication is not supported for materialized view: $0",
          info.table_name.ToString());
    }

    Schema consumer_schema;
    auto result = SchemaFromPB(resp->schema(), &consumer_schema);

    // We now have a table match. Validate the schema.
    SCHECK(
        result.ok() && consumer_schema.EquivalentForDataCopy(source_schema), IllegalState,
        Format(
            "Source and target schemas don't match: "
            "Source: $0, Target: $1, Source schema: $2, Target schema: $3",
            info.table_id, resp->identifier().table_id(), info.schema.ToString(),
            resp->schema().DebugString()));
    break;
  }

  SCHECK(
      table->has_table_id(), NotFound,
      Format(
          "Could not find matching table for $0$1", info.table_name.ToString(),
          (is_ysql_table ? " pgschema_name: " + source_schema.SchemaName() : "")));

  // Still need to make map of table id to resp table id (to add to validated map)
  // For colocated tables, only add the parent table since we only added the parent table to the
  // original pb (we use the number of tables in the pb to determine when validation is done).
  if (info.colocated) {
    // We require that colocated tables have the same colocation ID.
    //
    // Backward compatibility: tables created prior to #7378 use YSQL table OID as a colocation ID.
    auto source_clc_id = info.schema.has_colocation_id()
                             ? info.schema.colocation_id()
                             : CHECK_RESULT(GetPgsqlTableOid(info.table_id));
    auto target_clc_id = (resp->schema().has_colocated_table_id() &&
                          resp->schema().colocated_table_id().has_colocation_id())
                             ? resp->schema().colocated_table_id().colocation_id()
                             : CHECK_RESULT(GetPgsqlTableOid(resp->identifier().table_id()));
    SCHECK(
        source_clc_id == target_clc_id, IllegalState,
        Format(
            "Source and target colocation IDs don't match for colocated table: "
            "Source: $0, Target: $1, Source colocation ID: $2, Target colocation ID: $3",
            info.table_id, resp->identifier().table_id(), source_clc_id, target_clc_id));
  }

  {
    SharedLock lock(mutex_);
    if (xcluster_consumer_table_stream_ids_map_.contains(table->table_id())) {
      return STATUS(IllegalState, "N:1 replication topology not supported");
    }
  }

  return Status::OK();
}

std::unordered_set<xrepl::StreamId> CatalogManager::GetAllXReplStreamIds() const {
  SharedLock l(mutex_);
  std::unordered_set<xrepl::StreamId> result;
  for (const auto& [stream_id, _] : cdc_stream_map_) {
    result.insert(stream_id);
  }

  return result;
}

Status CatalogManager::XClusterReportNewAutoFlagConfigVersion(
    const XClusterReportNewAutoFlagConfigVersionRequestPB* req,
    XClusterReportNewAutoFlagConfigVersionResponsePB* resp, rpc::RpcContext* rpc,
    const LeaderEpoch& epoch) {
  LOG_WITH_FUNC(INFO) << " from " << RequestorString(rpc) << ": " << req->DebugString();

  const xcluster::ReplicationGroupId replication_group_id(req->replication_group_id());
  const auto new_version = req->auto_flag_config_version();

  // Verify that there is an existing Universe config
  scoped_refptr<UniverseReplicationInfo> replication_info;
  {
    SharedLock lock(mutex_);
    replication_info = FindPtrOrNull(universe_replication_map_, replication_group_id);
    SCHECK(
        replication_info, NotFound, "Missing replication group $0",
        replication_group_id.ToString());
  }

  auto cluster_config = ClusterConfig();

  return RefreshAutoFlagConfigVersion(
      *sys_catalog_, *replication_info, *cluster_config.get(), new_version,
      [master = master_]() { return master->GetAutoFlagsConfig(); }, epoch);
}

void CatalogManager::NotifyAutoFlagsConfigChanged() {
  xcluster_auto_flags_revalidation_needed_ = true;
}

Status CatalogManager::XClusterRefreshLocalAutoFlagConfig(const LeaderEpoch& epoch) {
  if (!xcluster_auto_flags_revalidation_needed_) {
    return Status::OK();
  }

  auto se = ScopeExit([this] { xcluster_auto_flags_revalidation_needed_ = true; });
  xcluster_auto_flags_revalidation_needed_ = false;

  std::vector<xcluster::ReplicationGroupId> replication_group_ids;
  bool update_failed = false;
  {
    SharedLock lock(mutex_);
    for (const auto& [replication_group_id, _] : universe_replication_map_) {
      replication_group_ids.push_back(replication_group_id);
    }
  }

  if (replication_group_ids.empty()) {
    return Status::OK();
  }

  const auto local_auto_flags_config = master_->GetAutoFlagsConfig();
  auto cluster_config = ClusterConfig();

  for (const auto& replication_group_id : replication_group_ids) {
    scoped_refptr<UniverseReplicationInfo> replication_info;
    {
      SharedLock lock(mutex_);
      replication_info = FindPtrOrNull(universe_replication_map_, replication_group_id);
    }
    if (!replication_info) {
      // Replication group was deleted before we could process it.
      continue;
    }

    auto status = HandleLocalAutoFlagsConfigChange(
        *sys_catalog_, *replication_info, *cluster_config.get(), local_auto_flags_config, epoch);
    if (!status.ok()) {
      LOG(WARNING) << "Failed to handle local AutoFlags config change for replication group "
                   << replication_group_id << ": " << status;
      update_failed = true;
    }
  }

  SCHECK(!update_failed, IllegalState, "Failed to handle local AutoFlags config change");

  se.Cancel();

  return Status::OK();
}

scoped_refptr<UniverseReplicationInfo> CatalogManager::GetUniverseReplication(
    const xcluster::ReplicationGroupId& replication_group_id) {
  SharedLock lock(mutex_);
  TRACE("Acquired catalog manager lock");
  return FindPtrOrNull(universe_replication_map_, replication_group_id);
}

std::vector<scoped_refptr<UniverseReplicationInfo>> CatalogManager::GetAllUniverseReplications()
    const {
  std::vector<scoped_refptr<UniverseReplicationInfo>> result;
  SharedLock lock(mutex_);
  for (const auto& [_, universe] : universe_replication_map_) {
    result.emplace_back(universe);
  }
  return result;
}

void CatalogManager::MarkUniverseForCleanup(
    const xcluster::ReplicationGroupId& replication_group_id) {
  LockGuard lock(mutex_);
  universes_to_clear_.push_back(replication_group_id);
}

Status CatalogManager::CreateCdcStateTableIfNotFound(const LeaderEpoch& epoch) {
  RETURN_NOT_OK(CreateTableIfNotFound(
      cdc::CDCStateTable::GetNamespaceName(), cdc::CDCStateTable::GetTableName(),
      &cdc::CDCStateTable::GenerateCreateCdcStateTableRequest, epoch));

  TRACE("Created CDC state table");

  // Mark the cluster as CDC enabled now that we have triggered the CDC state table creation.
  SetCDCServiceEnabled();

  return Status::OK();
}

Result<scoped_refptr<CDCStreamInfo>> CatalogManager::InitNewXReplStream() {
  LockGuard lock(mutex_);
  TRACE("Acquired catalog manager lock");

  auto stream_id = GenerateNewXreplStreamId();
  auto stream = make_scoped_refptr<CDCStreamInfo>(stream_id);
  stream->mutable_metadata()->StartMutation();

  cdc_stream_map_[stream_id] = stream;

  return stream;
}

void CatalogManager::ReleaseAbandonedXReplStream(const xrepl::StreamId& stream_id) {
  LockGuard lock(mutex_);
  TRACE("Acquired catalog manager lock");

  cdc_stream_map_.erase(stream_id);
  RecoverXreplStreamId(stream_id);
}

void CatalogManager::CDCSDKPopulateDeleteRetainerInfoForTabletDrop(
    const TabletInfo& tablet_info, TabletDeleteRetainerInfo& delete_retainer) const {
  // For CDCSDK , the only time we try to delete a single tablet that is part of an
  // active stream is during tablet splitting, where we need to keep the parent tablet around until
  // we have replicated its SPLIT_OP record.
  {
    auto tablet_lock = tablet_info.LockForRead();
    if (tablet_lock->pb.split_tablet_ids_size() < 2) {
      return;
    }
  }
  delete_retainer.active_cdcsdk = IsTablePartOfCDCSDK(tablet_info.table()->id());
}

Status CatalogManager::UpdateCheckpointForTabletEntriesInCDCState(
    const xrepl::StreamId& stream_id, const std::unordered_set<TableId>& tables_in_stream_metadata,
    const TableInfoPtr& table_to_be_removed) {
  bool is_colocated_table = table_to_be_removed->IsColocatedUserTable();
  TabletInfos tablets = table_to_be_removed->GetTablets(IncludeInactive::kTrue);
  if (tablets.empty()) {
    return Status::OK();
  }

  std::vector<cdc::CDCStateTableEntry> entries_to_update;
  if (is_colocated_table) {
    DCHECK_EQ(tablets.size(), 1);
    for (const auto& tablet : tablets) {
      if (!tablet) {
        continue;
      }

      bool should_update_streaming_entry = true;
      for (const auto& table_id : tablet->GetTableIds()) {
        if (tables_in_stream_metadata.contains(table_id)) {
          should_update_streaming_entry = false;
          break;
        }
      }

      if (should_update_streaming_entry) {
        cdc::CDCStateTableEntry update_entry(tablet->tablet_id(), stream_id);
        update_entry.checkpoint = OpId::Max();
        entries_to_update.emplace_back(std::move(update_entry));
        LOG_WITH_FUNC(INFO)
            << "Setting checkpoint to OpId::Max() for cdc state table entry (tablet,stream) - "
            << tablet->tablet_id() << ", " << stream_id;
      }

      // Snapshot entries for colocated tables (containing the colocated table id) are not processed
      // by UpdatePeersAndMetrics, hence we delete them directly instead of setting the checkpoint
      // to max.
      cdc::CDCStateTableKey delete_entry(tablet->tablet_id(), stream_id, table_to_be_removed->id());
      LOG_WITH_FUNC(INFO) << "Deleting cdc state table entry (tablet, stream, table) - "
                          << delete_entry.ToString();
      RETURN_NOT_OK_PREPEND(
          cdc_state_table_->DeleteEntries({delete_entry}),
          "Error deleting entries from cdc_state table");
    }
  } else {
    for (const auto& tablet : tablets) {
      if (!tablet) {
        continue;
      }
      cdc::CDCStateTableEntry update_entry(tablet->tablet_id(), stream_id);
      update_entry.checkpoint = OpId::Max();
      entries_to_update.emplace_back(std::move(update_entry));
      LOG_WITH_FUNC(INFO)
          << "Setting checkpoint to OpId::Max() for cdc state table entry (tablet,stream) - "
          << tablet->tablet_id() << ", " << stream_id;
    }
  }

  if (!entries_to_update.empty()) {
    LOG_WITH_FUNC(INFO) << "Setting checkpoint to max for " << entries_to_update.size()
                        << " cdc state entries for CDC stream: " << stream_id;
    RETURN_NOT_OK_PREPEND(
        cdc_state_table_->UpdateEntries(entries_to_update),
        "Error setting checkpoint to OpId::Max() in cdc_state table");
  }

  return Status::OK();
}

Result<std::vector<cdc::CDCStateTableEntry>> CatalogManager::SyncCDCStateTableEntries(
    const xrepl::StreamId& stream_id,
    const std::unordered_set<TableId>& tables_in_stream_metadata) {
  // Scan all the rows of state table and get the TabletInfo for each of them.
  Status iteration_status;
  auto all_entry_keys =
      VERIFY_RESULT(cdc_state_table_->GetTableRange({} /* just key columns */, &iteration_status));
  std::vector<cdc::CDCStateTableEntry> entries_to_update;
  // Get all the tablet, stream pairs from cdc_state for the given stream.
  std::vector<TabletId> cdc_state_tablet_entries;
  for (const auto& entry_result : all_entry_keys) {
    RETURN_NOT_OK(entry_result);
    const auto& entry = *entry_result;

    if (entry.key.stream_id == stream_id) {
      // For updating the checkpoint, only consider entries that do not have a colocated table_id.
      if (entry.key.colocated_table_id.empty()) {
        cdc_state_tablet_entries.push_back(entry.key.tablet_id);
      }
    }
  }
  RETURN_NOT_OK(iteration_status);

  // Get the tablet info for state table entries of the stream.
  auto tablet_infos = GetTabletInfos(cdc_state_tablet_entries);

  for (const auto& tablet_info : tablet_infos) {
    // If the TabletInfo is not found for tablet_id of a particular state table entry, updating the
    // checkpoint wont have any effect as the physical tablet has been deleted. Even
    // UpdatePeersAndMetrics would not find this tablet while trying to move barriers. Therefore, we
    // can ignore this entry.
    if (!tablet_info) {
      continue;
    }

    bool should_update_entry = true;
    // The state table entry can only be updated if it belongs to none of the tables present in
    // stream metadata.
    for (const auto& table_id : tablet_info->GetTableIds()) {
      if (tables_in_stream_metadata.contains(table_id)) {
        should_update_entry = false;
        break;
      }
    }

    if (should_update_entry) {
      cdc::CDCStateTableEntry update_entry(tablet_info->tablet_id(), stream_id);
      update_entry.checkpoint = OpId::Max();
      entries_to_update.emplace_back(std::move(update_entry));
      LOG_WITH_FUNC(INFO)
          << "Setting checkpoint to OpId::Max() for cdc state table entry (tablet,stream) - "
          << tablet_info->tablet_id() << ", " << stream_id;
    }
  }

  if (!entries_to_update.empty()) {
    LOG_WITH_FUNC(INFO) << "Setting checkpoint to max for " << entries_to_update.size()
                        << " cdc state entries for CDC stream: " << stream_id;
    RETURN_NOT_OK_PREPEND(
        cdc_state_table_->UpdateEntries(entries_to_update),
        "Error setting checkpoint to OpId::Max() in cdc_state table");
  }

  return entries_to_update;
}

Status CatalogManager::RemoveTableFromCDCStreamMetadataAndMaps(
    const CDCStreamInfoPtr stream, const TableId table_id, const LeaderEpoch& epoch) {
  // Remove the table from the CDC stream metadata & cdcsdk_tables_to_stream_map_ and persist
  // the updated metadata.
  {
    auto ltm = stream->LockForWrite();
    bool need_to_update_stream = false;

    auto table_id_iter = std::find(ltm->table_id().begin(), ltm->table_id().end(), table_id);
    if (table_id_iter != ltm->table_id().end()) {
      need_to_update_stream = true;
      ltm.mutable_data()->pb.mutable_table_id()->erase(table_id_iter);
    }

    if (need_to_update_stream) {
      LOG_WITH_FUNC(INFO) << "Removing table " << table_id
                          << " from qualified table list of CDC stream " << stream->id();
      RETURN_ACTION_NOT_OK(
          sys_catalog_->Upsert(epoch, stream), "Updating CDC streams in system catalog");
    }

    ltm.Commit();

    if (need_to_update_stream) {
      {
        LockGuard lock(mutex_);
        cdcsdk_tables_to_stream_map_[table_id].erase(stream->StreamId());
      }
    }
  }

  return Status::OK();
}

}  // namespace master
}  // namespace yb
