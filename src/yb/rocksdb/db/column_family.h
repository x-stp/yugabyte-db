//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.


#pragma once

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include "yb/rocksdb/compaction_job_stats.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/db/memtable_list.h"
#include "yb/rocksdb/db/table_properties_collector.h"
#include "yb/rocksdb/db/write_batch_internal.h"
#include "yb/rocksdb/db/write_controller.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/util/mutable_cf_options.h"
#include "yb/rocksdb/util/thread_local.h"

#include "yb/util/enums.h"

namespace rocksdb {

class Version;
class VersionSet;
class MemTable;
class MemTableListVersion;
class CompactionPicker;
class Compaction;
class InternalKey;
class InternalStats;
class ColumnFamilyData;
class DBImpl;
class LogBuffer;
class InstrumentedMutex;
class InstrumentedMutexLock;

YB_DEFINE_ENUM(CompactionSizeKind, (kSmall)(kLarge));

extern const double kSlowdownRatio;

// ColumnFamilyHandleImpl is the class that clients use to access different
// column families. It has non-trivial destructor, which gets called when client
// is done using the column family
class ColumnFamilyHandleImpl : public ColumnFamilyHandle {
 public:
  // create while holding the mutex
  ColumnFamilyHandleImpl(
      ColumnFamilyData* cfd, DBImpl* db, InstrumentedMutex* mutex);
  // destroy without mutex
  virtual ~ColumnFamilyHandleImpl();
  virtual ColumnFamilyData* cfd() const { return cfd_; }
  virtual const Comparator* user_comparator() const;

  virtual uint32_t GetID() const override;
  virtual const std::string& GetName() const override;
  virtual Status GetDescriptor(ColumnFamilyDescriptor* desc) override;

 private:
  ColumnFamilyData* cfd_;
  DBImpl* db_;
  InstrumentedMutex* mutex_;
};

// Does not ref-count ColumnFamilyData
// We use this dummy ColumnFamilyHandleImpl because sometimes MemTableInserter
// calls DBImpl methods. When this happens, MemTableInserter need access to
// ColumnFamilyHandle (same as the client would need). In that case, we feed
// MemTableInserter dummy ColumnFamilyHandle and enable it to call DBImpl
// methods
class ColumnFamilyHandleInternal : public ColumnFamilyHandleImpl {
 public:
  ColumnFamilyHandleInternal()
      : ColumnFamilyHandleImpl(nullptr, nullptr, nullptr) {}

  void SetCFD(ColumnFamilyData* _cfd) { internal_cfd_ = _cfd; }
  virtual ColumnFamilyData* cfd() const override { return internal_cfd_; }

 private:
  ColumnFamilyData* internal_cfd_;
};

// holds references to memtable, all immutable memtables and version
struct SuperVersion {
  // Accessing members of this class is not thread-safe and requires external
  // synchronization (ie db mutex held or on write thread).
  MemTable* mem;
  MemTableListVersion* imm;
  Version* current;
  MutableCFOptions mutable_cf_options;
  // Version number of the current SuperVersion
  uint64_t version_number;

  InstrumentedMutex* db_mutex;

  // should be called outside the mutex
  SuperVersion() = default;
  ~SuperVersion();
  SuperVersion* Ref();
  // If Unref() returns true, Cleanup() should be called with mutex held
  // before deleting this SuperVersion.
  bool Unref();

  // call these two methods with db mutex held
  // Cleanup unrefs mem, imm and current. Also, it stores all memtables
  // that needs to be deleted in to_delete vector. Unrefing those
  // objects needs to be done in the mutex
  void Cleanup();
  void Init(MemTable* new_mem, MemTableListVersion* new_imm,
            Version* new_current);

  // The value of dummy is not actually used. kSVInUse takes its address as a
  // mark in the thread local storage to indicate the SuperVersion is in use
  // by thread. This way, the value of kSVInUse is guaranteed to have no
  // conflict with SuperVersion object address and portable on different
  // platform.
  static int dummy;
  static void* const kSVInUse;
  static void* const kSVObsolete;

 private:
  std::atomic<uint32_t> refs;
  // We need to_delete because during Cleanup(), imm->Unref() returns
  // all memtables that we need to free through this vector. We then
  // delete all those memtables outside of mutex, during destruction
  autovector<MemTable*> to_delete;
};

extern Status CheckCompressionSupported(const ColumnFamilyOptions& cf_options);

extern Status CheckConcurrentWritesSupported(
    const ColumnFamilyOptions& cf_options);

extern ColumnFamilyOptions SanitizeOptions(const DBOptions& db_options,
                                           const InternalKeyComparator* icmp,
                                           const ColumnFamilyOptions& src);
// Wrap user defined table proproties collector factories `from cf_options`
// into internal ones in int_tbl_prop_collector_factories. Add a system internal
// one too.
extern void GetIntTblPropCollectorFactory(
    const ColumnFamilyOptions& cf_options,
    IntTblPropCollectorFactories* int_tbl_prop_collector_factories);

class ColumnFamilySet;

// This class keeps all the data that a column family needs.
// Most methods require DB mutex held, unless otherwise noted
class ColumnFamilyData {
 public:
  ~ColumnFamilyData();

  // thread-safe
  uint32_t GetID() const { return id_; }
  // thread-safe
  const std::string& GetName() const { return name_; }

  // Ref() can only be called from a context where the caller can guarantee
  // that ColumnFamilyData is alive (while holding a non-zero ref already,
  // holding a DB mutex, or as the leader in a write batch group).
  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }

  // Unref decreases the reference count, but does not handle deletion
  // when the count goes to 0.  If this method returns true then the
  // caller should delete the instance immediately, or later, by calling
  // FreeDeadColumnFamilies().  Unref() can only be called while holding
  // a DB mutex, or during single-threaded recovery.
  bool Unref() {
    int old_refs = refs_.fetch_sub(1, std::memory_order_relaxed);
    assert(old_refs > 0);
    return old_refs == 1;
  }

  // SetDropped() can only be called under following conditions:
  // 1) Holding a DB mutex,
  // 2) from single-threaded write thread, AND
  // 3) from single-threaded VersionSet::LogAndApply()
  // After dropping column family no other operation on that column family
  // will be executed. All the files and memory will be, however, kept around
  // until client drops the column family handle. That way, client can still
  // access data from dropped column family.
  // Column family can be dropped and still alive. In that state:
  // *) Compaction and flush is not executed on the dropped column family.
  // *) Client can continue reading from column family. Writes will fail unless
  // WriteOptions::ignore_missing_column_families is true
  // When the dropped column family is unreferenced, then we:
  // *) Remove column family from the linked list maintained by ColumnFamilySet
  // *) delete all memory associated with that column family
  // *) delete all the files associated with that column family
  void SetDropped();
  bool IsDropped() const { return dropped_; }

  // thread-safe
  int NumberLevels() const { return ioptions_.num_levels; }

  void SetLogNumber(uint64_t log_number) { log_number_ = log_number; }
  uint64_t GetLogNumber() const { return log_number_; }

  // !!! To be deprecated! Please don't not use this function anymore!
  const Options* options() const { return &options_; }

  // thread-safe
  const EnvOptions* soptions() const;
  const ImmutableCFOptions* ioptions() const { return &ioptions_; }
  // REQUIRES: DB mutex held
  // This returns the MutableCFOptions used by current SuperVersion
  // You shoul use this API to reference MutableCFOptions most of the time.
  const MutableCFOptions* GetCurrentMutableCFOptions() const {
    return &(super_version_->mutable_cf_options);
  }
  // REQUIRES: DB mutex held
  // This returns the latest MutableCFOptions, which may be not in effect yet.
  const MutableCFOptions* GetLatestMutableCFOptions() const {
    return &mutable_cf_options_;
  }
  // REQUIRES: DB mutex held
  Status SetOptions(
      const std::unordered_map<std::string, std::string>& options_map);

  InternalStats* internal_stats() { return internal_stats_.get(); }

  MemTableList* imm() { return &imm_; }
  MemTable* mem() { return mem_; }
  Version* current() const { return current_.load(); }
  Version* dummy_versions() { return dummy_versions_; }
  void SetCurrent(Version* _current);
  uint64_t GetNumLiveVersions() const;  // REQUIRE: DB mutex held
  uint64_t GetTotalSstFilesSize() const;  // REQUIRE: DB mutex held
  void SetMemtable(MemTable* new_mem) { mem_ = new_mem; }

  // See Memtable constructor for explanation of earliest_seq param.
  MemTable* ConstructNewMemtable(const MutableCFOptions& mutable_cf_options,
                                 SequenceNumber earliest_seq);
  void CreateNewMemtable(const MutableCFOptions& mutable_cf_options,
                         SequenceNumber earliest_seq);

  TableCache* table_cache() const { return table_cache_.get(); }

  // See documentation in compaction_picker.h
  // REQUIRES: DB mutex held
  bool NeedsCompaction() const;
  // REQUIRES: DB mutex held
  std::unique_ptr<Compaction> PickCompaction(
      const MutableCFOptions& mutable_options, LogBuffer* log_buffer);
  // A flag to tell a manual compaction is to compact all levels together
  // instad of for specific level.
  static const int kCompactAllLevels;
  // A flag to tell a manual compaction's output is base level.
  static const int kCompactToBaseLevel;
  // REQUIRES: DB mutex held
  std::unique_ptr<Compaction> CompactRange(
      const MutableCFOptions& mutable_cf_options, int input_level, int output_level,
      const CompactRangeOptions& compact_range_options, const InternalKey* begin,
      const InternalKey* end, InternalKey** compaction_end, bool* manual_conflict);

  CompactionPicker* compaction_picker() { return compaction_picker_.get(); }
  // thread-safe
  const Comparator* user_comparator() const {
    return internal_comparator_->user_comparator();
  }
  // thread-safe
  const InternalKeyComparatorPtr& internal_comparator() const {
    return internal_comparator_;
  }

  const IntTblPropCollectorFactories& int_tbl_prop_collector_factories() const {
    return int_tbl_prop_collector_factories_;
  }

  SuperVersion* GetSuperVersion() { return super_version_; }
  // thread-safe
  // Return a already referenced SuperVersion to be used safely.
  SuperVersion* GetReferencedSuperVersion(InstrumentedMutex* db_mutex);
  // thread-safe
  // Get SuperVersion stored in thread local storage. If it does not exist,
  // get a reference from a current SuperVersion.
  SuperVersion* GetThreadLocalSuperVersion(InstrumentedMutex* db_mutex);
  // Try to return SuperVersion back to thread local storage. Retrun true on
  // success and false on failure. It fails when the thread local storage
  // contains anything other than SuperVersion::kSVInUse flag.
  bool ReturnThreadLocalSuperVersion(SuperVersion* sv);
  // thread-safe
  uint64_t GetSuperVersionNumber() const {
    return super_version_number_.load();
  }
  // will return a pointer to SuperVersion* if previous SuperVersion
  // if its reference count is zero and needs deletion or nullptr if not
  // As argument takes a pointer to allocated SuperVersion to enable
  // the clients to allocate SuperVersion outside of mutex.
  // IMPORTANT: Only call this from DBImpl::InstallSuperVersion()
  MUST_USE_RESULT std::unique_ptr<SuperVersion> InstallSuperVersion(
      SuperVersion* new_superversion,
      InstrumentedMutex* db_mutex,
      const MutableCFOptions& mutable_cf_options);
  MUST_USE_RESULT std::unique_ptr<SuperVersion> InstallSuperVersion(
      SuperVersion* new_superversion,
      InstrumentedMutex* db_mutex);

  void ResetThreadLocalSuperVersions();

  // Protected by DB mutex
  void set_pending_flush(bool value) { pending_flush_ = value; }
  bool pending_flush() { return pending_flush_; }

  void PendingCompactionAdded(CompactionSizeKind compaction_size_kind);
  void PendingCompactionRemoved(CompactionSizeKind compaction_size_kind);
  void PendingCompactionSizeKindUpdated(CompactionSizeKind from, CompactionSizeKind to);
  bool pending_compaction() const;

  bool pending_compaction(CompactionSizeKind compaction_size_kind) const {
    return num_pending_compactions_[std::to_underlying(compaction_size_kind)] > 0;
  }

  size_t num_pending_compactions(CompactionSizeKind compaction_size_kind) const {
    return num_pending_compactions_[std::to_underlying(compaction_size_kind)];
  }

  // Recalculate some small conditions, which are changed only during
  // compaction, adding new memtable and/or
  // recalculation of compaction score. These values are used in
  // DBImpl::MakeRoomForWrite function to decide, if it need to make
  // a write stall
  void RecalculateWriteStallConditions(
      const MutableCFOptions& mutable_cf_options);

 private:
  friend class ColumnFamilySet;
  ColumnFamilyData(uint32_t id, const std::string& name,
                   Version* dummy_versions, Cache* table_cache,
                   WriteBuffer* write_buffer,
                   const ColumnFamilyOptions& options,
                   const DBOptions* db_options, const EnvOptions& env_options,
                   ColumnFamilySet* column_family_set);

  uint32_t id_;
  const std::string name_;
  Version* dummy_versions_;  // Head of circular doubly-linked list of versions.
  std::atomic<Version*> current_; // == dummy_versions->prev_

  std::atomic<int> refs_;      // outstanding references to ColumnFamilyData
  bool dropped_;               // true if client dropped it

  InternalKeyComparatorPtr internal_comparator_;
  IntTblPropCollectorFactories int_tbl_prop_collector_factories_;

  const Options options_;
  const ImmutableCFOptions ioptions_;
  MutableCFOptions mutable_cf_options_;

  std::unique_ptr<TableCache> table_cache_;

  std::unique_ptr<InternalStats> internal_stats_;

  WriteBuffer* write_buffer_;

  MemTable* mem_;
  MemTableList imm_;
  SuperVersion* super_version_;

  // An ordinal representing the current SuperVersion. Updated by
  // InstallSuperVersion(), i.e. incremented every time super_version_
  // changes.
  std::atomic<uint64_t> super_version_number_;

  // Thread's local copy of SuperVersion pointer
  // This needs to be destructed before mutex_
  std::unique_ptr<ThreadLocalPtr> local_sv_;

  // pointers for a circular linked list. we use it to support iterations over
  // all column families that are alive (note: dropped column families can also
  // be alive as long as client holds a reference)
  ColumnFamilyData* next_;
  ColumnFamilyData* prev_;

  // This is the earliest log file number that contains data from this
  // Column Family. All earlier log files must be ignored and not
  // recovered from
  uint64_t log_number_;

  // An object that keeps all the compaction stats
  // and picks the next compaction
  std::unique_ptr<CompactionPicker> compaction_picker_;

  ColumnFamilySet* column_family_set_;

  std::unique_ptr<WriteControllerToken> write_controller_token_;

  // If true --> this ColumnFamily is currently present in DBImpl::flush_queue_
  bool pending_flush_;

  // How many times this ColumnFamily is currently present in DBImpl::compaction_queue_.
  // Note: in general it might be not effective to use such nearly aligned atomics. This is not a
  // problem for this particular use case because this is not a hot path, but shouldn't be applied
  // in other cases where performance is critical.
  std::array<std::atomic<size_t>, kElementsInCompactionSizeKind> num_pending_compactions_;

  uint64_t prev_compaction_needed_bytes_;
};

// ColumnFamilySet has interesting thread-safety requirements
// * CreateColumnFamily() or RemoveColumnFamily() -- need to be protected by DB
// mutex AND executed in the write thread.
// CreateColumnFamily() should ONLY be called from VersionSet::LogAndApply() AND
// single-threaded write thread. It is also called during Recovery and in
// DumpManifest().
// RemoveColumnFamily() is only called from SetDropped(). DB mutex needs to be
// held and it needs to be executed from the write thread. SetDropped() also
// guarantees that it will be called only from single-threaded LogAndApply(),
// but this condition is not that important.
// * Iteration -- hold DB mutex, but you can release it in the body of
// iteration. If you release DB mutex in body, reference the column
// family before the mutex and unreference after you unlock, since the column
// family might get dropped when the DB mutex is released
// * GetDefault() -- thread safe
// * GetColumnFamily() -- either inside of DB mutex or from a write thread
// * GetNextColumnFamilyID(), GetMaxColumnFamily(), UpdateMaxColumnFamily(),
// NumberOfColumnFamilies -- inside of DB mutex
class ColumnFamilySet {
 public:
  // ColumnFamilySet supports iteration
  class iterator {
   public:
    explicit iterator(ColumnFamilyData* cfd)
        : current_(cfd) {}
    iterator& operator++() {
      // dropped column families might still be included in this iteration
      // (we're only removing them when client drops the last reference to the
      // column family).
      // dummy is never dead, so this will never be infinite
      do {
        current_ = current_->next_;
      } while (current_->refs_.load(std::memory_order_relaxed) == 0);
      return *this;
    }
    bool operator!=(const iterator& other) {
      return this->current_ != other.current_;
    }
    ColumnFamilyData* operator*() { return current_; }

   private:
    ColumnFamilyData* current_;
  };

  ColumnFamilySet(const std::string& dbname, const DBOptions* db_options,
                  const EnvOptions& env_options, Cache* table_cache,
                  WriteBuffer* write_buffer, WriteController* write_controller);
  ~ColumnFamilySet();

  ColumnFamilyData* GetDefault() const;
  // GetColumnFamily() calls return nullptr if column family is not found
  ColumnFamilyData* GetColumnFamily(uint32_t id) const;
  ColumnFamilyData* GetColumnFamily(const std::string& name) const;
  // this call will return the next available column family ID. it guarantees
  // that there is no column family with id greater than or equal to the
  // returned value in the current running instance or anytime in RocksDB
  // instance history.
  uint32_t GetNextColumnFamilyID();
  uint32_t GetMaxColumnFamily();
  void UpdateMaxColumnFamily(uint32_t new_max_column_family);
  size_t NumberOfColumnFamilies() const;

  ColumnFamilyData* CreateColumnFamily(const std::string& name, uint32_t id,
                                       Version* dummy_version,
                                       const ColumnFamilyOptions& options);

  iterator begin() { return iterator(dummy_cfd_->next_); }
  iterator end() { return iterator(dummy_cfd_); }

  // REQUIRES: DB mutex held
  // Don't call while iterating over ColumnFamilySet
  void FreeDeadColumnFamilies();

 private:
  friend class ColumnFamilyData;
  // helper function that gets called from cfd destructor
  // REQUIRES: DB mutex held
  void RemoveColumnFamily(ColumnFamilyData* cfd);

  // column_families_ and column_family_data_ need to be protected:
  // * when mutating both conditions have to be satisfied:
  // 1. DB mutex locked
  // 2. thread currently in single-threaded write thread
  // * when reading, at least one condition needs to be satisfied:
  // 1. DB mutex locked
  // 2. accessed from a single-threaded write thread
  std::unordered_map<std::string, uint32_t> column_families_;
  std::unordered_map<uint32_t, ColumnFamilyData*> column_family_data_;

  uint32_t max_column_family_;
  ColumnFamilyData* dummy_cfd_;
  // We don't hold the refcount here, since default column family always exists
  // We are also not responsible for cleaning up default_cfd_cache_. This is
  // just a cache that makes common case (accessing default column family)
  // faster
  ColumnFamilyData* default_cfd_cache_;

  const std::string db_name_;
  const DBOptions* const db_options_;
  const EnvOptions env_options_;
  Cache* table_cache_;
  WriteBuffer* write_buffer_;
  WriteController* write_controller_;
};

// We use ColumnFamilyMemTablesImpl to provide WriteBatch a way to access
// memtables of different column families (specified by ID in the write batch)
class ColumnFamilyMemTablesImpl : public ColumnFamilyMemTables {
 public:
  explicit ColumnFamilyMemTablesImpl(ColumnFamilySet* column_family_set)
      : column_family_set_(column_family_set), current_(nullptr) {}

  // Constructs a ColumnFamilyMemTablesImpl equivalent to one constructed
  // with the arguments used to construct *orig.
  explicit ColumnFamilyMemTablesImpl(ColumnFamilyMemTablesImpl* orig)
      : column_family_set_(orig->column_family_set_), current_(nullptr) {}

  // sets current_ to ColumnFamilyData with column_family_id
  // returns false if column family doesn't exist
  // REQUIRES: use this function of DBImpl::column_family_memtables_ should be
  //           under a DB mutex OR from a write thread
  bool Seek(uint32_t column_family_id) override;

  // Returns log number of the selected column family
  // REQUIRES: under a DB mutex OR from a write thread
  uint64_t GetLogNumber() const override;

  // REQUIRES: Seek() called first
  // REQUIRES: use this function of DBImpl::column_family_memtables_ should be
  //           under a DB mutex OR from a write thread
  virtual MemTable* GetMemTable() const override;

  // Returns column family handle for the selected column family
  // REQUIRES: use this function of DBImpl::column_family_memtables_ should be
  //           under a DB mutex OR from a write thread
  virtual ColumnFamilyHandle* GetColumnFamilyHandle() override;

  // Cannot be called while another thread is calling Seek().
  // REQUIRES: use this function of DBImpl::column_family_memtables_ should be
  //           under a DB mutex OR from a write thread
  virtual ColumnFamilyData* current() override { return current_; }

 private:
  ColumnFamilySet* column_family_set_;
  ColumnFamilyData* current_;
  ColumnFamilyHandleInternal handle_;
};

extern uint32_t GetColumnFamilyID(ColumnFamilyHandle* column_family);

extern const Comparator* GetColumnFamilyUserComparator(
    ColumnFamilyHandle* column_family);

}  // namespace rocksdb
