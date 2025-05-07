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

#include "yb/integration-tests/cdcsdk_test_base.h"

#include <algorithm>
#include <utility>
#include <string>
#include <chrono>
#include <boost/assign.hpp>
#include <gtest/gtest.h>

#include "yb/cdc/cdc_service.h"
#include "yb/cdc/cdc_service.proxy.h"

#include "yb/client/client.h"
#include "yb/client/meta_cache.h"
#include "yb/client/schema.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/transaction.h"
#include "yb/client/yb_op.h"

#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
#include "yb/common/ql_value.h"

#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/integration-tests/mini_cluster.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/xcluster_consumer_registry_service.h"
#include "yb/master/master.h"
#include "yb/master/master_client.pb.h"
#include "yb/master/master_ddl.pb.h"
#include "yb/master/master_ddl.proxy.h"
#include "yb/master/master_replication.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/master/sys_catalog_initialization.h"

#include "yb/rpc/rpc_controller.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/status_format.h"
#include "yb/util/test_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

using std::string;

DECLARE_bool(ysql_enable_pack_full_row_update);
DECLARE_string(pgsql_proxy_bind_address);

namespace yb {
using client::YBClient;
using client::YBTableName;

namespace cdc {

void CDCSDKTestBase::TearDown() {
  YBTest::TearDown();

  LOG(INFO) << "Destroying cluster for CDCSDK";

  if (test_cluster()) {
    if (test_cluster_.pg_supervisor_) {
      test_cluster_.pg_supervisor_->Stop();
    }
    test_cluster_.mini_cluster_->Shutdown();
    test_cluster_.mini_cluster_.reset();
  }
  test_cluster_.client_.reset();
}

std::unique_ptr<CDCServiceProxy> CDCSDKTestBase::GetCdcProxy() {
  YBClient* client_ = test_client();
  const auto mini_server = test_cluster()->mini_tablet_servers().front();
  std::unique_ptr<CDCServiceProxy> proxy = std::make_unique<CDCServiceProxy>(
      &client_->proxy_cache(), HostPort::FromBoundEndpoint(mini_server->bound_rpc_addr()));
  return proxy;
}

// Create a test database to work on.
Status CDCSDKTestBase::CreateDatabase(
    Cluster* cluster,
    const std::string& namespace_name,
    bool colocated) {
  auto conn = VERIFY_RESULT(cluster->Connect());
  RETURN_NOT_OK(conn.ExecuteFormat(
      "CREATE DATABASE $0$1", namespace_name, colocated ? " with colocation = true" : ""));
  return Status::OK();
}

Status CDCSDKTestBase::DropDatabase(
    Cluster* cluster,
    const std::string& namespace_name) {
  auto conn = VERIFY_RESULT(cluster->Connect());
  return conn.ExecuteFormat("DROP DATABASE $0", namespace_name);
}

Status CDCSDKTestBase::InitPostgres(Cluster* cluster) {
  auto pg_ts = RandomElement(cluster->mini_cluster_->mini_tablet_servers());
  auto port = cluster->mini_cluster_->AllocateFreePort();
  pgwrapper::PgProcessConf pg_process_conf =
      VERIFY_RESULT(pgwrapper::PgProcessConf::CreateValidateAndRunInitDb(
          AsString(Endpoint(pg_ts->bound_rpc_addr().address(), port)),
          pg_ts->options()->fs_opts.data_paths.front() + "/pg_data",
          pg_ts->server()->GetSharedMemoryFd()));
  pg_process_conf.master_addresses = pg_ts->options()->master_addresses_flag;
  pg_process_conf.force_disable_log_file = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_pgsql_proxy_webserver_port) =
      cluster->mini_cluster_->AllocateFreePort();

  LOG(INFO) << "Starting PostgreSQL server listening on " << pg_process_conf.listen_addresses << ":"
            << pg_process_conf.pg_port << ", data: " << pg_process_conf.data_dir
            << ", pgsql webserver port: " << FLAGS_pgsql_proxy_webserver_port;
  cluster->pg_supervisor_ =
      std::make_unique<pgwrapper::PgSupervisor>(pg_process_conf, nullptr /* tserver */);
  RETURN_NOT_OK(cluster->pg_supervisor_->Start());

  cluster->pg_host_port_ = HostPort(pg_process_conf.listen_addresses, pg_process_conf.pg_port);
  return Status::OK();
}

Status CDCSDKTestBase::InitPostgres(
    Cluster* cluster, const size_t pg_ts_idx, uint16_t pg_port) {
  auto pg_ts = cluster->mini_cluster_->mini_tablet_server(pg_ts_idx);
  pgwrapper::PgProcessConf pg_process_conf =
      VERIFY_RESULT(pgwrapper::PgProcessConf::CreateValidateAndRunInitDb(
          AsString(Endpoint(pg_ts->bound_rpc_addr().address(), pg_port)),
          pg_ts->options()->fs_opts.data_paths.front() + "/pg_data",
          pg_ts->server()->GetSharedMemoryFd()));
  pg_process_conf.master_addresses = pg_ts->options()->master_addresses_flag;
  pg_process_conf.force_disable_log_file = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_pgsql_proxy_webserver_port) =
      cluster->mini_cluster_->AllocateFreePort();

  LOG(INFO) << "Starting PostgreSQL server listening on " << pg_process_conf.listen_addresses << ":"
            << pg_process_conf.pg_port << ", data: " << pg_process_conf.data_dir
            << ", pgsql webserver port: " << FLAGS_pgsql_proxy_webserver_port;
  cluster->pg_supervisor_ =
      std::make_unique<pgwrapper::PgSupervisor>(pg_process_conf, nullptr /* tserver */);
  RETURN_NOT_OK(cluster->pg_supervisor_->Start());

  cluster->pg_host_port_ = HostPort(pg_process_conf.listen_addresses, pg_process_conf.pg_port);
  return Status::OK();
}

// Set up a cluster with the specified parameters.
Status CDCSDKTestBase::SetUpWithParams(
    uint32_t replication_factor, uint32_t num_masters, bool colocated,
    bool cdc_populate_safepoint_record, bool set_pgsql_proxy_bind_address) {
  master::SetDefaultInitialSysCatalogSnapshotFlags();
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_ysql) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_master_auto_run_initdb) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_hide_pg_catalog_table_creation_logs) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_pggate_rpc_timeout_secs) = 120;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_replication_factor) = replication_factor;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_pack_full_row_update) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_cdc_populate_safepoint_record) = cdc_populate_safepoint_record;

  MiniClusterOptions opts;
  opts.num_masters = num_masters;
  opts.num_tablet_servers = replication_factor;
  opts.cluster_id = "cdcsdk_cluster";

  test_cluster_.mini_cluster_ = std::make_unique<MiniCluster>(opts);

  size_t pg_ts_idx = 0;
  uint16_t pg_port = 0;
  if (set_pgsql_proxy_bind_address) {
    // Randomly select the tserver index that will serve the postgres proxy.
    pg_ts_idx = RandomUniformInt<size_t>(0, opts.num_tablet_servers - 1);
    const std::string pg_addr = server::TEST_RpcAddress(pg_ts_idx + 1, server::Private::kTrue);
    // The 'pgsql_proxy_bind_address' flag must be set before starting the cluster. Each
    // tserver will store this address when it starts.
    pg_port = test_cluster_.mini_cluster_->AllocateFreePort();
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_pgsql_proxy_bind_address) = Format("$0:$1", pg_addr, pg_port);
  }

  RETURN_NOT_OK(test_cluster()->StartSync());
  RETURN_NOT_OK(test_cluster()->WaitForTabletServerCount(replication_factor));
  RETURN_NOT_OK(WaitForInitDb(test_cluster()));
  test_cluster_.client_ = VERIFY_RESULT(test_cluster()->CreateClient());
  if (set_pgsql_proxy_bind_address) {
    RETURN_NOT_OK(InitPostgres(&test_cluster_, pg_ts_idx, pg_port));
  } else {
    RETURN_NOT_OK(InitPostgres(&test_cluster_));
  }
  RETURN_NOT_OK(CreateDatabase(&test_cluster_, kNamespaceName, colocated));

  cdc_proxy_ = GetCdcProxy();

  LOG(INFO) << "Cluster created successfully for CDCSDK";
  return Status::OK();
}

Result<google::protobuf::RepeatedPtrField<master::TabletLocationsPB>>
    CDCSDKTestBase::SetUpWithOneTablet(
        uint32_t replication_factor, uint32_t num_masters, bool colocated) {

  RETURN_NOT_OK(SetUpWithParams(replication_factor, num_masters, colocated));
  auto table = VERIFY_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  RETURN_NOT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  SCHECK_EQ(tablets.size(), 1, InternalError, "Only 1 tablet was expected");

  return tablets;
}

Result<YBTableName> CDCSDKTestBase::GetTable(
    Cluster* cluster,
    const std::string& namespace_name,
    const std::string& table_name,
    bool verify_table_name,
    bool exclude_system_tables) {
  master::ListTablesRequestPB req;
  master::ListTablesResponsePB resp;

  req.set_name_filter(table_name);
  req.mutable_namespace_()->set_name(namespace_name);
  req.mutable_namespace_()->set_database_type(YQL_DATABASE_PGSQL);
  if (!exclude_system_tables) {
    req.set_exclude_system_tables(true);
    req.add_relation_type_filter(master::USER_TABLE_RELATION);
  }

  master::MasterDdlProxy master_proxy(
      &cluster->client_->proxy_cache(),
      VERIFY_RESULT(cluster->mini_cluster_->GetLeaderMasterBoundRpcAddr()));

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  RETURN_NOT_OK(master_proxy.ListTables(req, &resp, &rpc));
  if (resp.has_error()) {
    return STATUS(IllegalState, "Failed listing tables");
  }

  // Now need to find the table and return it.
  for (const auto& table : resp.tables()) {
    // If !verify_table_name, just return the first table.
    if (!verify_table_name ||
        (table.name() == table_name && table.namespace_().name() == namespace_name)) {
      YBTableName yb_table;
      yb_table.set_table_id(table.id());
      yb_table.set_namespace_id(table.namespace_().id());
      yb_table.set_table_name(table.name());
      return yb_table;
    }
  }
  return STATUS_FORMAT(
      IllegalState, "Unable to find table $0 in namespace $1", table_name, namespace_name);
}

Result<YBTableName> CDCSDKTestBase::CreateTable(
    Cluster* cluster,
    const std::string& namespace_name,
    const std::string& table_name,
    const uint32_t num_tablets,
    const bool add_primary_key,
    bool colocated,
    const int table_oid,
    const bool enum_value,
    const std::string& enum_suffix,
    const std::string& schema_name,
    uint32_t num_cols,
    const std::vector<string>& optional_cols_name) {
  auto conn = VERIFY_RESULT(cluster->ConnectToDB(namespace_name));

  if (enum_value) {
    if (schema_name != "public") {
      RETURN_NOT_OK(conn.ExecuteFormat("create schema $0;", schema_name));
    }
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE TYPE $0.$1$2 AS ENUM ('FIXED$3','PERCENTAGE$4');", schema_name,
        kEnumTypeName, enum_suffix, enum_suffix, enum_suffix));
  }

  std::string table_oid_string = "";
  if (table_oid > 0) {
    // Need to turn on session flag to allow for CREATE WITH table_oid.
    RETURN_NOT_OK(conn.Execute("set yb_enable_create_with_table_oid=true"));
    table_oid_string = Format("table_oid = $0,", table_oid);
  }

  if (!optional_cols_name.empty()) {
    std::stringstream columns_name;
    std::stringstream columns_value;
    string primary_key = add_primary_key ? "PRIMARY KEY" : "";
    string second_column_type =
        enum_value ? (schema_name + "." + "coupon_discount_type" + enum_suffix) : " int";
    columns_name << "( " << kKeyColumnName << " int " << primary_key << "," << kValueColumnName
                 << second_column_type;
    for (const auto& optional_col_name : optional_cols_name) {
      columns_name << " , " << optional_col_name << " int ";
    }
    columns_name << " )";
    columns_value << " )";
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE TABLE $0.$1 $2 WITH ($3colocated = $4) "
        "SPLIT INTO $5 TABLETS",
        schema_name, table_name + enum_suffix, columns_name.str(), table_oid_string, colocated,
        num_tablets));
  } else if (num_cols > 2) {
    std::stringstream statement_buff;
    statement_buff << "CREATE TABLE $0.$1(col1 int PRIMARY KEY, col2 int";
    std::string rem_statement(" ) WITH ($2colocated = $3) SPLIT INTO $4 TABLETS");
    for (uint32_t col_num = 3; col_num <= num_cols; ++col_num) {
      statement_buff << ", col" << col_num << " int";
    }
    std::string statement(statement_buff.str() + rem_statement);

    RETURN_NOT_OK(conn.ExecuteFormat(
        statement, schema_name, table_name, table_oid_string, colocated, num_tablets));
  } else if (colocated) {
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE TABLE $0.$1($2 int $3, $4 $5) WITH ($6colocated = $7) ", schema_name,
        table_name + enum_suffix, kKeyColumnName, (add_primary_key) ? "PRIMARY KEY" : "",
        kValueColumnName,
        enum_value ? (schema_name + "." + "coupon_discount_type" + enum_suffix) : "int",
        table_oid_string, colocated));
  } else {
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE TABLE $0.$1($2 int $3, $4 $5) SPLIT INTO $6 TABLETS", schema_name,
        table_name + enum_suffix, kKeyColumnName, (add_primary_key) ? "PRIMARY KEY" : "",
        kValueColumnName,
        enum_value ? (schema_name + "." + "coupon_discount_type" + enum_suffix) : "int",
        num_tablets));
  }
  return GetTable(cluster, namespace_name, table_name + enum_suffix);
}

Status CDCSDKTestBase::AddColumn(
    Cluster* cluster,
    const std::string& namespace_name,
    const std::string& table_name,
    const std::string& add_column_name,
    const std::string& enum_suffix,
    const std::string& schema_name) {
  auto conn = VERIFY_RESULT(cluster->ConnectToDB(namespace_name));
  RETURN_NOT_OK(conn.ExecuteFormat(
      "ALTER TABLE $0.$1 ADD COLUMN $2 int", schema_name, table_name + enum_suffix,
      add_column_name));
  return Status::OK();
}

Status CDCSDKTestBase::DropColumn(
    Cluster* cluster,
    const std::string& namespace_name,
    const std::string& table_name,
    const std::string& column_name,
    const std::string& enum_suffix,
    const std::string& schema_name) {
  auto conn = VERIFY_RESULT(cluster->ConnectToDB(namespace_name));
  RETURN_NOT_OK(conn.ExecuteFormat(
      "ALTER TABLE $0.$1 DROP COLUMN $2", schema_name, table_name + enum_suffix, column_name));
  return Status::OK();
}

Status CDCSDKTestBase::RenameColumn(
    Cluster* cluster,
    const std::string& namespace_name,
    const std::string& table_name,
    const std::string& old_column_name,
    const std::string& new_column_name,
    const std::string& enum_suffix,
    const std::string& schema_name) {
  auto conn = VERIFY_RESULT(cluster->ConnectToDB(namespace_name));
  RETURN_NOT_OK(conn.ExecuteFormat(
      "ALTER TABLE $0.$1 RENAME COLUMN $2 TO $3", schema_name, table_name + enum_suffix,
      old_column_name, new_column_name));
  return Status::OK();
}

Result<std::string> CDCSDKTestBase::GetNamespaceId(const std::string& namespace_name) {
  master::GetNamespaceInfoResponsePB namespace_info_resp;

  RETURN_NOT_OK(test_client()->GetNamespaceInfo(
      std::string(), kNamespaceName, YQL_DATABASE_PGSQL, &namespace_info_resp));

  // Return namespace_id.
  return namespace_info_resp.namespace_().id();
}

Result<std::string> CDCSDKTestBase::GetTableId(
    Cluster* cluster,
    const std::string& namespace_name,
    const std::string& table_name,
    bool verify_table_name,
    bool exclude_system_tables) {
  master::ListTablesRequestPB req;
  master::ListTablesResponsePB resp;

  req.set_name_filter(table_name);
  req.mutable_namespace_()->set_name(namespace_name);
  req.mutable_namespace_()->set_database_type(YQL_DATABASE_PGSQL);
  if (!exclude_system_tables) {
    req.set_exclude_system_tables(true);
    req.add_relation_type_filter(master::USER_TABLE_RELATION);
  }

  master::MasterDdlProxy master_proxy(
      &cluster->client_->proxy_cache(),
      VERIFY_RESULT(cluster->mini_cluster_->GetLeaderMasterBoundRpcAddr()));

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  RETURN_NOT_OK(master_proxy.ListTables(req, &resp, &rpc));
  if (resp.has_error()) {
    return STATUS(IllegalState, "Failed listing tables");
  }

  // Now need to find the table and return it.
  for (const auto& table : resp.tables()) {
    // If !verify_table_name, just return the first table.
    if (!verify_table_name ||
        (table.name() == table_name && table.namespace_().name() == namespace_name)) {
      return table.id();
    }
  }
  return STATUS_FORMAT(
      IllegalState, "Unable to find table id for $0 in $1", table_name, namespace_name);
}

// Initialize a CreateCDCStreamRequest to be used while creating a DB stream ID.
void CDCSDKTestBase::InitCreateStreamRequest(
    CreateCDCStreamRequestPB* create_req,
    const CDCCheckpointType& checkpoint_type,
    const CDCRecordType& record_type,
    const std::string& namespace_name,
    CDCSDKDynamicTablesOption dynamic_tables_option) {
  create_req->set_namespace_name(namespace_name);
  create_req->set_checkpoint_type(checkpoint_type);
  create_req->set_record_type(record_type);
  create_req->set_record_format(CDCRecordFormat::PROTO);
  create_req->set_source_type(CDCSDK);
  create_req->mutable_cdcsdk_stream_create_options()->set_cdcsdk_dynamic_tables_option(
      dynamic_tables_option);
}

// This creates a DB stream on the database kNamespaceName by default.
Result<xrepl::StreamId> CDCSDKTestBase::CreateDBStream(
    CDCCheckpointType checkpoint_type, CDCRecordType record_type, std::string namespace_name,
    CDCSDKDynamicTablesOption dynamic_tables_option) {
  CreateCDCStreamRequestPB req;
  CreateCDCStreamResponsePB resp;

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_write_rpc_timeout_ms));

  InitCreateStreamRequest(
      &req, checkpoint_type, record_type, namespace_name, dynamic_tables_option);

  RETURN_NOT_OK(cdc_proxy_->CreateCDCStream(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return xrepl::StreamId::FromString(resp.db_stream_id());
}

// This creates a Consistent Snapshot stream on the database kNamespaceName by default.
Result<xrepl::StreamId> CDCSDKTestBase::CreateConsistentSnapshotStream(
    CDCSDKSnapshotOption snapshot_option,
    CDCCheckpointType checkpoint_type,
    CDCRecordType record_type) {
  CreateCDCStreamRequestPB req;
  CreateCDCStreamResponsePB resp;

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_write_rpc_timeout_ms));

  InitCreateStreamRequest(&req, checkpoint_type, record_type);
  req.set_cdcsdk_consistent_snapshot_option(snapshot_option);

  RETURN_NOT_OK(cdc_proxy_->CreateCDCStream(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  // Sleep for 1 second - temporary till synchronous implementation of CreateCDCStream
  SleepFor(MonoDelta::FromSeconds(1));

  return xrepl::StreamId::FromString(resp.db_stream_id());
}



Result<master::ListCDCStreamsResponsePB> CDCSDKTestBase::ListDBStreams() {
  auto ns_id = VERIFY_RESULT(GetNamespaceId(kNamespaceName));

  master::ListCDCStreamsRequestPB req;
  master::ListCDCStreamsResponsePB resp;

  req.set_namespace_id(ns_id);

  master::MasterReplicationProxy master_proxy(
      &test_cluster_.client_->proxy_cache(),
      VERIFY_RESULT(test_cluster_.mini_cluster_->GetLeaderMasterBoundRpcAddr()));

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  RETURN_NOT_OK(master_proxy.ListCDCStreams(req, &resp, &rpc));
  if (resp.has_error()) {
    return STATUS(IllegalState, "Failed listing CDC streams");
  }

  return resp;
}

}  // namespace cdc
}  // namespace yb
