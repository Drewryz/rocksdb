//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include "db/db_impl.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <inttypes.h>
#include "db/event_helpers.h"
#include "monitoring/perf_context_imp.h"
#include "options/options_helper.h"
#include "util/sync_point.h"
#include <stdio.h>

namespace rocksdb {
// Convenience methods
Status DBImpl::Put(const WriteOptions& o, ColumnFamilyHandle* column_family,
                   const Slice& key, const Slice& val) {
  return DB::Put(o, column_family, key, val);
}

Status DBImpl::Merge(const WriteOptions& o, ColumnFamilyHandle* column_family,
                     const Slice& key, const Slice& val) {
  auto cfh = reinterpret_cast<ColumnFamilyHandleImpl*>(column_family);
  if (!cfh->cfd()->ioptions()->merge_operator) {
    return Status::NotSupported("Provide a merge_operator when opening DB");
  } else {
    return DB::Merge(o, column_family, key, val);
  }
}

Status DBImpl::Delete(const WriteOptions& write_options,
                      ColumnFamilyHandle* column_family, const Slice& key) {
  return DB::Delete(write_options, column_family, key);
}

Status DBImpl::SingleDelete(const WriteOptions& write_options,
                            ColumnFamilyHandle* column_family,
                            const Slice& key) {
  return DB::SingleDelete(write_options, column_family, key);
}

Status DBImpl::Write(const WriteOptions& write_options, WriteBatch* my_batch) {
  return WriteImpl(write_options, my_batch, nullptr, nullptr);
}

#ifndef ROCKSDB_LITE
Status DBImpl::WriteWithCallback(const WriteOptions& write_options,
                                 WriteBatch* my_batch,
                                 WriteCallback* callback) {
  return WriteImpl(write_options, my_batch, callback, nullptr);
}
#endif  // ROCKSDB_LITE

/*
 * 调用栈如下：
#0  rocksdb::DBImpl::WriteImpl (this=0x832040, write_options=..., my_batch=0x7fffffffd870, callback=0x0, log_used=0x0, log_ref=0, disable_memtable=false)
    at /root/code/rocksdb/db/db_impl_write.cc:98
#1  0x00007ffff72b3cbe in rocksdb::DBImpl::Write (this=0x832040, write_options=..., my_batch=0x7fffffffd870) at /root/code/rocksdb/db/db_impl_write.cc:49
#2  0x00007ffff72b952d in rocksdb::DB::Put (this=0x832040, opt=..., column_family=0x83d640, key=..., value=...) at /root/code/rocksdb/db/db_impl_write.cc:1209
#3  0x00007ffff72b3b2e in rocksdb::DBImpl::Put (this=0x832040, o=..., column_family=0x83d640, key=..., val=...) at /root/code/rocksdb/db/db_impl_write.cc:24
#4  0x00007ffff72416cc in rocksdb::DB::Put (this=0x832040, options=..., key=..., value=...) at /root/code/rocksdb/include/rocksdb/db.h:228
#5  0x0000000000408304 in main () at test.cpp:20 
 */
/*
 * rocksdb的写操作入口函数
 * write_options: 配置项
 * my_batch: 持有kv数据
 * 1. 最开始有一些与配置项相关的代码，跳过
 * 2. 构造一个writer，并将其join到一个batch group中
 * 3. 根据join的结果，如果是：
 *    1) STATE_PARALLEL_MEMTABLE_WRITER:
 *       表示当前线程自己要将自己的数据写入memtable
 *       1)) 写入memtable， InsertInto
 *       2)) 如果本writer是当前write batch group的最后一个writer，则
 *           1))) 设置全局sequence number
 *           2))) 退出write batch group, ExitAsBatchGroupFollower
 *       3)) 
 *    2) STATE_COMPLETED:
 *       表示数据已经被leader写完成，返回
 *    3) STATE_GROUP_LEADER:
 *       表示当前线程作为leader:
 *       1)) 写入数据之前预处理，参见PreprocessWrite
 *       2)) 获取WAL的log_writer
 *       3)) 构建write batch group，参见EnterAsBatchGroupLeader
 *       4)) 遍历write batch group, 计算total_count/total_byte_size/parallel，
 *           计算的过程涉及WriteBatchInternal和WriteBatch两个类，细节暂时跳过
 *       5)) 记录一些DB状态
 *       6)) 写write batch group的wal日志
 *       7)) 写memtable
 *           1))) 根据之前的判断，如果不能并发写memtable，则由当前线程将所有的数据写入memtable
 *           2))) 如果可以并发写, 计算并设置当前write batch group的所有writer的sequence
 *                将当前write batch group的所有writer的状态设置为STATE_PARALLEL_MEMTABLE_WRITER
 *                leader将自己的数据写入memtable
 *       8)) 如果处于并发写memtable模式下，则等待本组所有的writer写完成
 *       9)) 如果当前leader线程是最后一个完成写memtable的：
 *           1))) 设置全局sequence number
 *           2))) 退出write batch group, ExitAsBatchGroupLeader
 *       10)) 返回
 * 
 * TODO:
 * 1. versions_->GetColumnFamilySet() / VersionSet
 *    参见：http://liuyangming.tech/05-2020/write-prepared.html
 * 2. ColumnFamilySet
 * 3. InsertInto
 * 4. enable_pipelined_write
 *    之前我们分析了RocksDB的默认写入方式，而在options可以设置enable_pipelined_write, 即pipelined(流水线)写入方式，
 *    默认的写入方式中，一个batch的需要完成WAL之后，再完成Memtable的写入才选出下一个Leader.而Pipelined写入中不需要等待Memtable写入完成，
 *    即当WAL写入完成之后，即可选出下一个Leader继续完成下一个batch的写入从而达到Pipelined的效果.
 *    参见：https://zhuanlan.zhihu.com/p/45666093
 * 5. concurrent_prepare_
 *    与2PC协议有关，参见：
 *    http://liuyangming.tech/05-2020/write-prepared.html
 * 6. writer->batch->HasMerge()
 * 7. InternalStats* default_cf_internal_stats_
 * 8. WriteBatchInternal::InsertInto
 * 
 * !!! pipeline写，InsertInto，write_batch, 2PC
 */
Status DBImpl::WriteImpl(const WriteOptions& write_options,
                         WriteBatch* my_batch, WriteCallback* callback,
                         uint64_t* log_used, uint64_t log_ref,
                         bool disable_memtable) {
  if (my_batch == nullptr) {
    return Status::Corruption("Batch is nullptr!");
  }
  if (concurrent_prepare_ && immutable_db_options_.enable_pipelined_write) {
    return Status::NotSupported(
        "pipelined_writes is not compatible with concurrent prepares");
  }

  Status status;
  if (write_options.low_pri) {
    status = ThrottleLowPriWritesIfNeeded(write_options, my_batch);
    if (!status.ok()) {
      return status;
    }
  }

  if (concurrent_prepare_ && disable_memtable) {
    return WriteImplWALOnly(write_options, my_batch, callback, log_used,
                            log_ref);
  }
  /* TODO: ??? */
  if (immutable_db_options_.enable_pipelined_write) {
    return PipelinedWriteImpl(write_options, my_batch, callback, log_used,
                              log_ref, disable_memtable);
  }

  PERF_TIMER_GUARD(write_pre_and_post_process_time);
  WriteThread::Writer w(write_options, my_batch, callback, log_ref,
                        disable_memtable);

  if (!write_options.disableWAL) {
    RecordTick(stats_, WRITE_WITH_WAL);
  }

  /* RAII类，统计一些数据，忽略 */
  StopWatch write_sw(env_, immutable_db_options_.statistics.get(), DB_WRITE);
  /*
   *  一个DB对象持有一个write_thread_对象
   */
  /*
   * Join的时候当前线程可能会sleep，处于sleep中的线程会被别的线程唤醒，有以下几种情况
   * 1. 被当前write batch group的leader唤醒，唤醒的状态可能为STATE_PARALLEL_MEMTABLE_WRITER和STATE_COMPLETED
   * 2. 被上一个write batch group的leader唤醒，唤醒的状态为STATE_GROUP_LEADER
   */
  write_thread_.JoinBatchGroup(&w);
  /*
   * reading here. 2021-3-7-18:46 
   */
  if (w.state == WriteThread::STATE_PARALLEL_MEMTABLE_WRITER) {
    // we are a non-leader in a parallel group
    PERF_TIMER_GUARD(write_memtable_time);
    printf("mark1mark1\n");
    if (w.ShouldWriteToMemtable()) {
      ColumnFamilyMemTablesImpl column_family_memtables(
          versions_->GetColumnFamilySet());
      /* 
       * 传入的参数: w.sequence是已经初始化过的
       * 猜测是leader做的
       */
      printf("yzryzr %llu\n", (long long unsigned int)w.sequence);
      /* 此时writer自身的一些变量已经被leader设置过了 */
      w.status = WriteBatchInternal::InsertInto(
          &w, w.sequence, &column_family_memtables, &flush_scheduler_,
          write_options.ignore_missing_column_families, 0 /*log_number*/, this,
          true /*concurrent_memtable_writes*/);
    }

    if (write_thread_.CompleteParallelMemTableWriter(&w)) {
      // we're responsible for exit batch group
      auto last_sequence = w.write_group->last_sequence;
      versions_->SetLastSequence(last_sequence);
      MemTableInsertStatusCheck(w.status);
      write_thread_.ExitAsBatchGroupFollower(&w);
    }
    assert(w.state == WriteThread::STATE_COMPLETED);
    // STATE_COMPLETED conditional below handles exit

    status = w.FinalStatus();
  }
  if (w.state == WriteThread::STATE_COMPLETED) {
    if (log_used != nullptr) {
      *log_used = w.log_used;
    }
    // write is complete and leader has updated sequence
    return w.FinalStatus();
  }
  // else we are the leader of the write batch group
  assert(w.state == WriteThread::STATE_GROUP_LEADER);

  /* 以下代码是当前线程作为leader时的逻辑 */
  // Once reaches this point, the current writer "w" will try to do its write
  // job.  It may also pick up some of the remaining writers in the "writers_"
  // when it finds suitable, and finish them in the same write batch.
  // This is how a write job could be done by the other writer.
  WriteContext write_context;
  WriteThread::WriteGroup write_group;
  bool in_parallel_group = false;
  uint64_t last_sequence = kMaxSequenceNumber;
  if (!concurrent_prepare_) {
    last_sequence = versions_->LastSequence();
  }

  mutex_.Lock();

  bool need_log_sync = !write_options.disableWAL && write_options.sync;
  bool need_log_dir_sync = need_log_sync && !log_dir_synced_;
  if (!concurrent_prepare_ || !disable_memtable) {
    /* 写入之前的预处理 */
    // With concurrent writes we do preprocess only in the write thread that
    // also does write to memtable to avoid sync issue on shared data structure
    // with the other thread
    status = PreprocessWrite(write_options, &need_log_sync, &write_context);
  }
  log::Writer* log_writer = logs_.back().writer;

  mutex_.Unlock();

  // Add to log and apply to memtable.  We can release the lock
  // during this phase since &w is currently responsible for logging
  // and protects against concurrent loggers and concurrent writes
  // into memtables

  last_batch_group_size_ =
      write_thread_.EnterAsBatchGroupLeader(&w, &write_group);

  if (status.ok()) {
    // Rules for when we can update the memtable concurrently
    // 1. supported by memtable
    // 2. Puts are not okay if inplace_update_support
    // 3. Merges are not okay
    //
    // Rules 1..2 are enforced by checking the options
    // during startup (CheckConcurrentWritesSupported), so if
    // options.allow_concurrent_memtable_write is true then they can be
    // assumed to be true.  Rule 3 is checked for each batch.  We could
    // relax rules 2 if we could prevent write batches from referring
    // more than once to a particular key.
    bool parallel = immutable_db_options_.allow_concurrent_memtable_write &&
                    write_group.size > 1;
    int total_count = 0;
    uint64_t total_byte_size = 0;
    /* reading here. 2021-3-8-17:45 */
    for (auto* writer : write_group) {
      if (writer->CheckCallback(this)) {
        if (writer->ShouldWriteToMemtable()) {
          total_count += WriteBatchInternal::Count(writer->batch);
          parallel = parallel && !writer->batch->HasMerge();
        }

        total_byte_size = WriteBatchInternal::AppendedByteSize(
            total_byte_size, WriteBatchInternal::ByteSize(writer->batch));
      }
    }

    const bool concurrent_update = concurrent_prepare_;
    // Update stats while we are an exclusive group leader, so we know
    // that nobody else can be writing to these particular stats.
    // We're optimistic, updating the stats before we successfully
    // commit.  That lets us release our leader status early.
    auto stats = default_cf_internal_stats_;
    stats->AddDBStats(InternalStats::NUMBER_KEYS_WRITTEN, total_count,
                      concurrent_update);
    RecordTick(stats_, NUMBER_KEYS_WRITTEN, total_count);
    stats->AddDBStats(InternalStats::BYTES_WRITTEN, total_byte_size,
                      concurrent_update);
    RecordTick(stats_, BYTES_WRITTEN, total_byte_size);
    stats->AddDBStats(InternalStats::WRITE_DONE_BY_SELF, 1, concurrent_update);
    RecordTick(stats_, WRITE_DONE_BY_SELF);
    auto write_done_by_other = write_group.size - 1;
    if (write_done_by_other > 0) {
      stats->AddDBStats(InternalStats::WRITE_DONE_BY_OTHER, write_done_by_other,
                        concurrent_update);
      RecordTick(stats_, WRITE_DONE_BY_OTHER, write_done_by_other);
    }
    MeasureTime(stats_, BYTES_PER_WRITE, total_byte_size);

    if (write_options.disableWAL) {
      has_unpersisted_data_.store(true, std::memory_order_relaxed);
    }

    PERF_TIMER_STOP(write_pre_and_post_process_time);
    /* 写WAL */
    if (!concurrent_prepare_) {
      if (status.ok() && !write_options.disableWAL) {
        PERF_TIMER_GUARD(write_wal_time);
        status = WriteToWAL(write_group, log_writer, log_used, need_log_sync,
                            need_log_dir_sync, last_sequence + 1);
      }
    } else { /* 与2PC有关，跳过 */
      if (status.ok() && !write_options.disableWAL) {
        PERF_TIMER_GUARD(write_wal_time);
        // LastToBeWrittenSequence is increased inside WriteToWAL under
        // wal_write_mutex_ to ensure ordered events in WAL
        status = ConcurrentWriteToWAL(write_group, log_used, &last_sequence,
                                      total_count);
      } else {
        // Otherwise we inc seq number for memtable writes
        last_sequence = versions_->FetchAddLastToBeWrittenSequence(total_count);
      }
    }
    assert(last_sequence != kMaxSequenceNumber);
    const SequenceNumber current_sequence = last_sequence + 1;
    /* ???? */
    last_sequence += total_count;

    if (status.ok()) { /* 以下逻辑，开始写入memtable */
      PERF_TIMER_GUARD(write_memtable_time);

      if (!parallel) {
        w.status = WriteBatchInternal::InsertInto(
            write_group, current_sequence, column_family_memtables_.get(),
            &flush_scheduler_, write_options.ignore_missing_column_families,
            0 /*recovery_log_number*/, this);
      } else {
        SequenceNumber next_sequence = current_sequence;
        for (auto* writer : write_group) {
          if (writer->ShouldWriteToMemtable()) {
            writer->sequence = next_sequence;
            next_sequence += WriteBatchInternal::Count(writer->batch);
          }
        }
        write_group.last_sequence = last_sequence;
        write_group.running.store(static_cast<uint32_t>(write_group.size),
                                  std::memory_order_relaxed);
        write_thread_.LaunchParallelMemTableWriters(&write_group);
        in_parallel_group = true;

        // Each parallel follower is doing each own writes. The leader should
        // also do its own.
        if (w.ShouldWriteToMemtable()) {
          ColumnFamilyMemTablesImpl column_family_memtables(
              versions_->GetColumnFamilySet());
          assert(w.sequence == current_sequence);
          w.status = WriteBatchInternal::InsertInto(
              &w, w.sequence, &column_family_memtables, &flush_scheduler_,
              write_options.ignore_missing_column_families, 0 /*log_number*/,
              this, true /*concurrent_memtable_writes*/);
        }
      }
    }
  }
  PERF_TIMER_START(write_pre_and_post_process_time);

  if (!w.CallbackFailed()) {
    WriteCallbackStatusCheck(status);
  }

  /*
   * 上面在写WAL的时候(WriteToWAL)已经做了flush wal, 为什么这里还要判断need_log_sync？
   * 看逻辑应该和2PC逻辑相关，跳过
   */
  if (need_log_sync) {
    mutex_.Lock();
    MarkLogsSynced(logfile_number_, need_log_dir_sync, status);
    mutex_.Unlock();
    // Requesting sync with concurrent_prepare_ is expected to be very rare. We
    // hance provide a simple implementation that is not necessarily efficient.
    if (concurrent_prepare_) {
      if (manual_wal_flush_) {
        status = FlushWAL(true);
      } else {
        status = SyncWAL();
      }
    }
  }

  bool should_exit_batch_group = true;
  if (in_parallel_group) {
    // CompleteParallelWorker returns true if this thread should
    // handle exit, false means somebody else did
    should_exit_batch_group = write_thread_.CompleteParallelMemTableWriter(&w);
  }
  if (should_exit_batch_group) {
    if (status.ok()) {
      versions_->SetLastSequence(last_sequence);
    }
    MemTableInsertStatusCheck(w.status);
    write_thread_.ExitAsBatchGroupLeader(write_group, w.status);
  }

  if (status.ok()) {
    status = w.FinalStatus();
  }
  return status;
}

Status DBImpl::PipelinedWriteImpl(const WriteOptions& write_options,
                                  WriteBatch* my_batch, WriteCallback* callback,
                                  uint64_t* log_used, uint64_t log_ref,
                                  bool disable_memtable) {
  PERF_TIMER_GUARD(write_pre_and_post_process_time);
  StopWatch write_sw(env_, immutable_db_options_.statistics.get(), DB_WRITE);

  WriteContext write_context;

  WriteThread::Writer w(write_options, my_batch, callback, log_ref,
                        disable_memtable);
  write_thread_.JoinBatchGroup(&w);
  if (w.state == WriteThread::STATE_GROUP_LEADER) {
    WriteThread::WriteGroup wal_write_group;
    if (w.callback && !w.callback->AllowWriteBatching()) {
      write_thread_.WaitForMemTableWriters();
    }
    mutex_.Lock();
    bool need_log_sync = !write_options.disableWAL && write_options.sync;
    bool need_log_dir_sync = need_log_sync && !log_dir_synced_;
    w.status = PreprocessWrite(write_options, &need_log_sync, &write_context);
    log::Writer* log_writer = logs_.back().writer;
    mutex_.Unlock();

    // This can set non-OK status if callback fail.
    last_batch_group_size_ =
        write_thread_.EnterAsBatchGroupLeader(&w, &wal_write_group);
    const SequenceNumber current_sequence =
        write_thread_.UpdateLastSequence(versions_->LastSequence()) + 1;
    size_t total_count = 0;
    size_t total_byte_size = 0;

    if (w.status.ok()) {
      SequenceNumber next_sequence = current_sequence;
      for (auto writer : wal_write_group) {
        if (writer->CheckCallback(this)) {
          if (writer->ShouldWriteToMemtable()) {
            writer->sequence = next_sequence;
            size_t count = WriteBatchInternal::Count(writer->batch);
            next_sequence += count;
            total_count += count;
          }
          total_byte_size = WriteBatchInternal::AppendedByteSize(
              total_byte_size, WriteBatchInternal::ByteSize(writer->batch));
        }
      }
      if (w.disable_wal) {
        has_unpersisted_data_.store(true, std::memory_order_relaxed);
      }
      write_thread_.UpdateLastSequence(current_sequence + total_count - 1);
    }

    auto stats = default_cf_internal_stats_;
    stats->AddDBStats(InternalStats::NUMBER_KEYS_WRITTEN, total_count);
    RecordTick(stats_, NUMBER_KEYS_WRITTEN, total_count);
    stats->AddDBStats(InternalStats::BYTES_WRITTEN, total_byte_size);
    RecordTick(stats_, BYTES_WRITTEN, total_byte_size);

    PERF_TIMER_STOP(write_pre_and_post_process_time);

    if (w.ShouldWriteToWAL()) {
      PERF_TIMER_GUARD(write_wal_time);
      stats->AddDBStats(InternalStats::WRITE_DONE_BY_SELF, 1);
      RecordTick(stats_, WRITE_DONE_BY_SELF, 1);
      if (wal_write_group.size > 1) {
        stats->AddDBStats(InternalStats::WRITE_DONE_BY_OTHER,
                          wal_write_group.size - 1);
        RecordTick(stats_, WRITE_DONE_BY_OTHER, wal_write_group.size - 1);
      }
      w.status = WriteToWAL(wal_write_group, log_writer, log_used,
                            need_log_sync, need_log_dir_sync, current_sequence);
    }

    if (!w.CallbackFailed()) {
      WriteCallbackStatusCheck(w.status);
    }

    if (need_log_sync) {
      mutex_.Lock();
      MarkLogsSynced(logfile_number_, need_log_dir_sync, w.status);
      mutex_.Unlock();
    }

    write_thread_.ExitAsBatchGroupLeader(wal_write_group, w.status);
  }

  WriteThread::WriteGroup memtable_write_group;
  if (w.state == WriteThread::STATE_MEMTABLE_WRITER_LEADER) {
    PERF_TIMER_GUARD(write_memtable_time);
    assert(w.status.ok());
    write_thread_.EnterAsMemTableWriter(&w, &memtable_write_group);
    if (memtable_write_group.size > 1 &&
        immutable_db_options_.allow_concurrent_memtable_write) {
      write_thread_.LaunchParallelMemTableWriters(&memtable_write_group);
    } else {
      memtable_write_group.status = WriteBatchInternal::InsertInto(
          memtable_write_group, w.sequence, column_family_memtables_.get(),
          &flush_scheduler_, write_options.ignore_missing_column_families,
          0 /*log_number*/, this);
      versions_->SetLastSequence(memtable_write_group.last_sequence);
      write_thread_.ExitAsMemTableWriter(&w, memtable_write_group);
    }
  }

  if (w.state == WriteThread::STATE_PARALLEL_MEMTABLE_WRITER) {
    assert(w.ShouldWriteToMemtable());
    ColumnFamilyMemTablesImpl column_family_memtables(
        versions_->GetColumnFamilySet());
    w.status = WriteBatchInternal::InsertInto(
        &w, w.sequence, &column_family_memtables, &flush_scheduler_,
        write_options.ignore_missing_column_families, 0 /*log_number*/, this,
        true /*concurrent_memtable_writes*/);
    if (write_thread_.CompleteParallelMemTableWriter(&w)) {
      MemTableInsertStatusCheck(w.status);
      versions_->SetLastSequence(w.write_group->last_sequence);
      write_thread_.ExitAsMemTableWriter(&w, *w.write_group);
    }
  }

  assert(w.state == WriteThread::STATE_COMPLETED);
  return w.FinalStatus();
}

Status DBImpl::WriteImplWALOnly(const WriteOptions& write_options,
                                WriteBatch* my_batch, WriteCallback* callback,
                                uint64_t* log_used, uint64_t log_ref) {
  Status status;
  PERF_TIMER_GUARD(write_pre_and_post_process_time);
  WriteThread::Writer w(write_options, my_batch, callback, log_ref,
                        true /* disable_memtable */);
  if (write_options.disableWAL) {
    return status;
  }
  RecordTick(stats_, WRITE_WITH_WAL);
  StopWatch write_sw(env_, immutable_db_options_.statistics.get(), DB_WRITE);

  nonmem_write_thread_.JoinBatchGroup(&w);
  assert(w.state != WriteThread::STATE_PARALLEL_MEMTABLE_WRITER);
  if (w.state == WriteThread::STATE_COMPLETED) {
    if (log_used != nullptr) {
      *log_used = w.log_used;
    }
    return w.FinalStatus();
  }
  // else we are the leader of the write batch group
  assert(w.state == WriteThread::STATE_GROUP_LEADER);
  WriteContext write_context;
  WriteThread::WriteGroup write_group;
  uint64_t last_sequence;
  nonmem_write_thread_.EnterAsBatchGroupLeader(&w, &write_group);
  // Note: no need to update last_batch_group_size_ here since the batch writes
  // to WAL only

  uint64_t total_byte_size = 0;
  for (auto* writer : write_group) {
    if (writer->CheckCallback(this)) {
      total_byte_size = WriteBatchInternal::AppendedByteSize(
          total_byte_size, WriteBatchInternal::ByteSize(writer->batch));
    }
  }

  const bool concurrent_update = true;
  // Update stats while we are an exclusive group leader, so we know
  // that nobody else can be writing to these particular stats.
  // We're optimistic, updating the stats before we successfully
  // commit.  That lets us release our leader status early.
  auto stats = default_cf_internal_stats_;
  stats->AddDBStats(InternalStats::BYTES_WRITTEN, total_byte_size,
                    concurrent_update);
  RecordTick(stats_, BYTES_WRITTEN, total_byte_size);
  stats->AddDBStats(InternalStats::WRITE_DONE_BY_SELF, 1, concurrent_update);
  RecordTick(stats_, WRITE_DONE_BY_SELF);
  auto write_done_by_other = write_group.size - 1;
  if (write_done_by_other > 0) {
    stats->AddDBStats(InternalStats::WRITE_DONE_BY_OTHER, write_done_by_other,
                      concurrent_update);
    RecordTick(stats_, WRITE_DONE_BY_OTHER, write_done_by_other);
  }
  MeasureTime(stats_, BYTES_PER_WRITE, total_byte_size);

  PERF_TIMER_STOP(write_pre_and_post_process_time);

  PERF_TIMER_GUARD(write_wal_time);
  // LastToBeWrittenSequence is increased inside WriteToWAL under
  // wal_write_mutex_ to ensure ordered events in WAL
  status = ConcurrentWriteToWAL(write_group, log_used, &last_sequence,
                                0 /*total_count*/);
  if (status.ok() && write_options.sync) {
    // Requesting sync with concurrent_prepare_ is expected to be very rare. We
    // hance provide a simple implementation that is not necessarily efficient.
    if (manual_wal_flush_) {
      status = FlushWAL(true);
    } else {
      status = SyncWAL();
    }
  }
  PERF_TIMER_START(write_pre_and_post_process_time);

  if (!w.CallbackFailed()) {
    WriteCallbackStatusCheck(status);
  }
  nonmem_write_thread_.ExitAsBatchGroupLeader(write_group, w.status);
  if (status.ok()) {
    status = w.FinalStatus();
  }
  return status;
}

void DBImpl::WriteCallbackStatusCheck(const Status& status) {
  // Is setting bg_error_ enough here?  This will at least stop
  // compaction and fail any further writes.
  if (immutable_db_options_.paranoid_checks && !status.ok() &&
      !status.IsBusy() && !status.IsIncomplete()) {
    mutex_.Lock();
    if (bg_error_.ok()) {
      Status new_bg_error = status;
      // may temporarily unlock and lock the mutex.
      EventHelpers::NotifyOnBackgroundError(immutable_db_options_.listeners,
                                            BackgroundErrorReason::kWriteCallback,
                                            &new_bg_error, &mutex_);
      if (!new_bg_error.ok()) {
        bg_error_ = new_bg_error;  // stop compaction & fail any further writes
      }
    }
    mutex_.Unlock();
  }
}

void DBImpl::MemTableInsertStatusCheck(const Status& status) {
  // A non-OK status here indicates that the state implied by the
  // WAL has diverged from the in-memory state.  This could be
  // because of a corrupt write_batch (very bad), or because the
  // client specified an invalid column family and didn't specify
  // ignore_missing_column_families.
  if (!status.ok()) {
    mutex_.Lock();
    assert(bg_error_.ok());
    Status new_bg_error = status;
    // may temporarily unlock and lock the mutex.
    EventHelpers::NotifyOnBackgroundError(immutable_db_options_.listeners,
                                          BackgroundErrorReason::kMemTable,
                                          &new_bg_error, &mutex_);
    if (!new_bg_error.ok()) {
      bg_error_ = new_bg_error;  // stop compaction & fail any further writes
    }
    mutex_.Unlock();
  }
}

Status DBImpl::PreprocessWrite(const WriteOptions& write_options,
                               bool* need_log_sync,
                               WriteContext* write_context) {
  mutex_.AssertHeld();
  assert(write_context != nullptr && need_log_sync != nullptr);
  Status status;

  assert(!single_column_family_mode_ ||
         versions_->GetColumnFamilySet()->NumberOfColumnFamilies() == 1);
  if (UNLIKELY(status.ok() && !single_column_family_mode_ &&
               total_log_size_ > GetMaxTotalWalSize())) {
    status = HandleWALFull(write_context);
  }

  if (UNLIKELY(status.ok() && write_buffer_manager_->ShouldFlush())) {
    // Before a new memtable is added in SwitchMemtable(),
    // write_buffer_manager_->ShouldFlush() will keep returning true. If another
    // thread is writing to another DB with the same write buffer, they may also
    // be flushed. We may end up with flushing much more DBs than needed. It's
    // suboptimal but still correct.
    status = HandleWriteBufferFull(write_context);
  }

  if (UNLIKELY(status.ok() && !bg_error_.ok())) {
    return bg_error_;
  }

  if (UNLIKELY(status.ok() && !flush_scheduler_.Empty())) {
    status = ScheduleFlushes(write_context);
  }

  if (UNLIKELY(status.ok() && (write_controller_.IsStopped() ||
                               write_controller_.NeedsDelay()))) {
    PERF_TIMER_GUARD(write_delay_time);
    // We don't know size of curent batch so that we always use the size
    // for previous one. It might create a fairness issue that expiration
    // might happen for smaller writes but larger writes can go through.
    // Can optimize it if it is an issue.
    status = DelayWrite(last_batch_group_size_, write_options);
  }

  if (status.ok() && *need_log_sync) {
    // Wait until the parallel syncs are finished. Any sync process has to sync
    // the front log too so it is enough to check the status of front()
    // We do a while loop since log_sync_cv_ is signalled when any sync is
    // finished
    // Note: there does not seem to be a reason to wait for parallel sync at
    // this early step but it is not important since parallel sync (SyncWAL) and
    // need_log_sync are usually not used together.
    while (logs_.front().getting_synced) {
      log_sync_cv_.Wait();
    }
    for (auto& log : logs_) {
      assert(!log.getting_synced);
      // This is just to prevent the logs to be synced by a parallel SyncWAL
      // call. We will do the actual syncing later after we will write to the
      // WAL.
      // Note: there does not seem to be a reason to set this early before we
      // actually write to the WAL
      log.getting_synced = true;
    }
  } else {
    *need_log_sync = false;
  }

  return status;
}

/*
 * 将一个batch writer group所有的writer合并成一个writer，并返回
 * write_group：传入参数
 * tmp_batch：传入参数，主要的作用在于内存管理和性能优化
 * write_with_wal: 传出参数, 表示合并了多少writer
 * 返回值：合并后的writer
 * 
 * 1. 如果当前group只有leader一个writer，那merged_batch就是leader本身
 * 2. 如果有多个writer，将他们merge成单个writer
 * 
 */
WriteBatch* DBImpl::MergeBatch(const WriteThread::WriteGroup& write_group,
                               WriteBatch* tmp_batch, size_t* write_with_wal) {
  assert(write_with_wal != nullptr);
  assert(tmp_batch != nullptr);
  WriteBatch* merged_batch = nullptr;
  *write_with_wal = 0;
  auto* leader = write_group.leader;
  if (write_group.size == 1 && leader->ShouldWriteToWAL() &&
      leader->batch->GetWalTerminationPoint().is_cleared()) {
    // we simply write the first WriteBatch to WAL if the group only
    // contains one batch, that batch should be written to the WAL,
    // and the batch is not wanting to be truncated
    /* 如果当前group只有leader一个writer，那merged_batch就是leader本身 */
    merged_batch = leader->batch;
    *write_with_wal = 1;
  } else {
    /* 如果有多个writer，将他们merge成单个writer */
    // WAL needs all of the batches flattened into a single batch.
    // We could avoid copying here with an iov-like AddRecord
    // interface
    merged_batch = tmp_batch;
    for (auto writer : write_group) {
      if (writer->ShouldWriteToWAL()) {
        WriteBatchInternal::Append(merged_batch, writer->batch,
                                   /*WAL_only*/ true);
        (*write_with_wal)++;
      }
    }
  }
  return merged_batch;
}

/*
 * 实际往WAL文件写日志的函数
 * log_used: 传出参数, 记录写入的WAL日志number
 * log_size: 传出参数, merged_batch的log size
 */
// When concurrent_prepare_ is disabled, this function is called from the only
// write thread. Otherwise this must be called holding log_write_mutex_.
Status DBImpl::WriteToWAL(const WriteBatch& merged_batch,
                          log::Writer* log_writer, uint64_t* log_used,
                          uint64_t* log_size) {
  assert(log_size != nullptr);
  Slice log_entry = WriteBatchInternal::Contents(&merged_batch);
  *log_size = log_entry.size();
  Status status = log_writer->AddRecord(log_entry);
  if (log_used != nullptr) {
    *log_used = logfile_number_;
  }
  total_log_size_ += log_entry.size();
  // TODO(myabandeh): it might be unsafe to access alive_log_files_.back() here
  // since alive_log_files_ might be modified concurrently
  alive_log_files_.back().AddSize(log_entry.size());
  log_empty_ = false;
  return status;
}

/*
 * 写WAL。
 * sequence: 传入参数。当前write batch group所属的LSN
 * log_used: 传出参数。当前write batch group写入的WAL文件编号
 * 
 * 1. 将一个batch writer group所有的writer合并成一个writer，MergeBatch
 * 2. 设置write batch要写入的wal日志文件的编号
 * 3. 设置write batch的LSN
 * 4. 实际将日志写入wal文件，WriteToWAL
 * 5. 如果需要sync，则将所有log做sync。将所有log做sync的原因是为了避免日志空洞
 * 6. 清空tmp_batch_
 * 7. 记录一些状态数据
 * 
 * TODO:
 * 1. logfile_number_目测是wal日志文件的编号
 * 2. 
 */
Status DBImpl::WriteToWAL(const WriteThread::WriteGroup& write_group,
                          log::Writer* log_writer, uint64_t* log_used,
                          bool need_log_sync, bool need_log_dir_sync,
                          SequenceNumber sequence) {
  Status status;

  size_t write_with_wal = 0;
  WriteBatch* merged_batch =
      MergeBatch(write_group, &tmp_batch_, &write_with_wal);
  if (merged_batch == write_group.leader->batch) {
    write_group.leader->log_used = logfile_number_;
  } else if (write_with_wal > 1) {
    for (auto writer : write_group) {
      writer->log_used = logfile_number_;
    }
  }

  WriteBatchInternal::SetSequence(merged_batch, sequence);

  uint64_t log_size;
  status = WriteToWAL(*merged_batch, log_writer, log_used, &log_size);

  if (status.ok() && need_log_sync) {
    StopWatch sw(env_, stats_, WAL_FILE_SYNC_MICROS);
    // It's safe to access logs_ with unlocked mutex_ here because:
    //  - we've set getting_synced=true for all logs,
    //    so other threads won't pop from logs_ while we're here,
    //  - only writer thread can push to logs_, and we're in
    //    writer thread, so no one will push to logs_,
    //  - as long as other threads don't modify it, it's safe to read
    //    from std::deque from multiple threads concurrently.
    for (auto& log : logs_) {
      status = log.writer->file()->Sync(immutable_db_options_.use_fsync);
      if (!status.ok()) {
        break;
      }
    }
    if (status.ok() && need_log_dir_sync) {
      // We only sync WAL directory the first time WAL syncing is
      // requested, so that in case users never turn on WAL sync,
      // we can avoid the disk I/O in the write code path.
      status = directories_.GetWalDir()->Fsync();
    }
  }

  if (merged_batch == &tmp_batch_) {
    tmp_batch_.Clear();
  }
  if (status.ok()) {
    auto stats = default_cf_internal_stats_;
    if (need_log_sync) {
      stats->AddDBStats(InternalStats::WAL_FILE_SYNCED, 1);
      RecordTick(stats_, WAL_FILE_SYNCED);
    }
    stats->AddDBStats(InternalStats::WAL_FILE_BYTES, log_size);
    RecordTick(stats_, WAL_FILE_BYTES, log_size);
    stats->AddDBStats(InternalStats::WRITE_WITH_WAL, write_with_wal);
    RecordTick(stats_, WRITE_WITH_WAL, write_with_wal);
  }
  return status;
}

Status DBImpl::ConcurrentWriteToWAL(const WriteThread::WriteGroup& write_group,
                                    uint64_t* log_used,
                                    SequenceNumber* last_sequence,
                                    int total_count) {
  Status status;

  WriteBatch tmp_batch;
  size_t write_with_wal = 0;
  WriteBatch* merged_batch =
      MergeBatch(write_group, &tmp_batch, &write_with_wal);

  // We need to lock log_write_mutex_ since logs_ and alive_log_files might be
  // pushed back concurrently
  log_write_mutex_.Lock();
  if (merged_batch == write_group.leader->batch) {
    write_group.leader->log_used = logfile_number_;
  } else if (write_with_wal > 1) {
    for (auto writer : write_group) {
      writer->log_used = logfile_number_;
    }
  }
  *last_sequence = versions_->FetchAddLastToBeWrittenSequence(total_count);
  auto sequence = *last_sequence + 1;
  WriteBatchInternal::SetSequence(merged_batch, sequence);

  log::Writer* log_writer = logs_.back().writer;
  uint64_t log_size;
  status = WriteToWAL(*merged_batch, log_writer, log_used, &log_size);
  log_write_mutex_.Unlock();

  if (status.ok()) {
    const bool concurrent = true;
    auto stats = default_cf_internal_stats_;
    stats->AddDBStats(InternalStats::WAL_FILE_BYTES, log_size, concurrent);
    RecordTick(stats_, WAL_FILE_BYTES, log_size);
    stats->AddDBStats(InternalStats::WRITE_WITH_WAL, write_with_wal,
                      concurrent);
    RecordTick(stats_, WRITE_WITH_WAL, write_with_wal);
  }
  return status;
}

Status DBImpl::HandleWALFull(WriteContext* write_context) {
  mutex_.AssertHeld();
  assert(write_context != nullptr);
  Status status;

  if (alive_log_files_.begin()->getting_flushed) {
    return status;
  }

  auto oldest_alive_log = alive_log_files_.begin()->number;
  auto oldest_log_with_uncommited_prep = FindMinLogContainingOutstandingPrep();

  if (allow_2pc() &&
      oldest_log_with_uncommited_prep > 0 &&
      oldest_log_with_uncommited_prep <= oldest_alive_log) {
    if (unable_to_flush_oldest_log_) {
        // we already attempted to flush all column families dependent on
        // the oldest alive log but the log still contained uncommited transactions.
        // the oldest alive log STILL contains uncommited transaction so there
        // is still nothing that we can do.
        return status;
    } else {
      ROCKS_LOG_WARN(
          immutable_db_options_.info_log,
          "Unable to release oldest log due to uncommited transaction");
      unable_to_flush_oldest_log_ = true;
    }
  } else {
    // we only mark this log as getting flushed if we have successfully
    // flushed all data in this log. If this log contains outstanding prepared
    // transactions then we cannot flush this log until those transactions are commited.
    unable_to_flush_oldest_log_ = false;
    alive_log_files_.begin()->getting_flushed = true;
  }

  ROCKS_LOG_INFO(immutable_db_options_.info_log,
                 "Flushing all column families with data in WAL number %" PRIu64
                 ". Total log size is %" PRIu64
                 " while max_total_wal_size is %" PRIu64,
                 oldest_alive_log, total_log_size_.load(), GetMaxTotalWalSize());
  // no need to refcount because drop is happening in write thread, so can't
  // happen while we're in the write thread
  for (auto cfd : *versions_->GetColumnFamilySet()) {
    if (cfd->IsDropped()) {
      continue;
    }
    if (cfd->OldestLogToKeep() <= oldest_alive_log) {
      status = SwitchMemtable(cfd, write_context);
      if (!status.ok()) {
        break;
      }
      cfd->imm()->FlushRequested();
      SchedulePendingFlush(cfd);
    }
  }
  MaybeScheduleFlushOrCompaction();
  return status;
}

Status DBImpl::HandleWriteBufferFull(WriteContext* write_context) {
  mutex_.AssertHeld();
  assert(write_context != nullptr);
  Status status;

  // Before a new memtable is added in SwitchMemtable(),
  // write_buffer_manager_->ShouldFlush() will keep returning true. If another
  // thread is writing to another DB with the same write buffer, they may also
  // be flushed. We may end up with flushing much more DBs than needed. It's
  // suboptimal but still correct.
  ROCKS_LOG_INFO(
      immutable_db_options_.info_log,
      "Flushing column family with largest mem table size. Write buffer is "
      "using %" PRIu64 " bytes out of a total of %" PRIu64 ".",
      write_buffer_manager_->memory_usage(),
      write_buffer_manager_->buffer_size());
  // no need to refcount because drop is happening in write thread, so can't
  // happen while we're in the write thread
  ColumnFamilyData* cfd_picked = nullptr;
  SequenceNumber seq_num_for_cf_picked = kMaxSequenceNumber;

  for (auto cfd : *versions_->GetColumnFamilySet()) {
    if (cfd->IsDropped()) {
      continue;
    }
    if (!cfd->mem()->IsEmpty()) {
      // We only consider active mem table, hoping immutable memtable is
      // already in the process of flushing.
      uint64_t seq = cfd->mem()->GetCreationSeq();
      if (cfd_picked == nullptr || seq < seq_num_for_cf_picked) {
        cfd_picked = cfd;
        seq_num_for_cf_picked = seq;
      }
    }
  }
  if (cfd_picked != nullptr) {
    status = SwitchMemtable(cfd_picked, write_context);
    if (status.ok()) {
      cfd_picked->imm()->FlushRequested();
      SchedulePendingFlush(cfd_picked);
      MaybeScheduleFlushOrCompaction();
    }
  }
  return status;
}

uint64_t DBImpl::GetMaxTotalWalSize() const {
  mutex_.AssertHeld();
  return mutable_db_options_.max_total_wal_size == 0
             ? 4 * max_total_in_memory_state_
             : mutable_db_options_.max_total_wal_size;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::DelayWrite(uint64_t num_bytes,
                          const WriteOptions& write_options) {
  uint64_t time_delayed = 0;
  bool delayed = false;
  {
    StopWatch sw(env_, stats_, WRITE_STALL, &time_delayed);
    uint64_t delay = write_controller_.GetDelay(env_, num_bytes);
    if (delay > 0) {
      if (write_options.no_slowdown) {
        return Status::Incomplete();
      }
      TEST_SYNC_POINT("DBImpl::DelayWrite:Sleep");

      mutex_.Unlock();
      // We will delay the write until we have slept for delay ms or
      // we don't need a delay anymore
      const uint64_t kDelayInterval = 1000;
      uint64_t stall_end = sw.start_time() + delay;
      while (write_controller_.NeedsDelay()) {
        if (env_->NowMicros() >= stall_end) {
          // We already delayed this write `delay` microseconds
          break;
        }

        delayed = true;
        // Sleep for 0.001 seconds
        env_->SleepForMicroseconds(kDelayInterval);
      }
      mutex_.Lock();
    }

    while (bg_error_.ok() && write_controller_.IsStopped()) {
      if (write_options.no_slowdown) {
        return Status::Incomplete();
      }
      delayed = true;
      TEST_SYNC_POINT("DBImpl::DelayWrite:Wait");
      bg_cv_.Wait();
    }
  }
  assert(!delayed || !write_options.no_slowdown);
  if (delayed) {
    default_cf_internal_stats_->AddDBStats(InternalStats::WRITE_STALL_MICROS,
                                           time_delayed);
    RecordTick(stats_, STALL_MICROS, time_delayed);
  }

  return bg_error_;
}

Status DBImpl::ThrottleLowPriWritesIfNeeded(const WriteOptions& write_options,
                                            WriteBatch* my_batch) {
  assert(write_options.low_pri);
  // This is called outside the DB mutex. Although it is safe to make the call,
  // the consistency condition is not guaranteed to hold. It's OK to live with
  // it in this case.
  // If we need to speed compaction, it means the compaction is left behind
  // and we start to limit low pri writes to a limit.
  if (write_controller_.NeedSpeedupCompaction()) {
    if (allow_2pc() && (my_batch->HasCommit() || my_batch->HasRollback())) {
      // For 2PC, we only rate limit prepare, not commit.
      return Status::OK();
    }
    if (write_options.no_slowdown) {
      return Status::Incomplete();
    } else {
      assert(my_batch != nullptr);
      // Rate limit those writes. The reason that we don't completely wait
      // is that in case the write is heavy, low pri writes may never have
      // a chance to run. Now we guarantee we are still slowly making
      // progress.
      write_controller_.low_pri_rate_limiter()->Request(
          my_batch->GetDataSize(), Env::IO_HIGH, nullptr /* stats */,
          RateLimiter::OpType::kWrite);
    }
  }
  return Status::OK();
}

Status DBImpl::ScheduleFlushes(WriteContext* context) {
  ColumnFamilyData* cfd;
  while ((cfd = flush_scheduler_.TakeNextColumnFamily()) != nullptr) {
    auto status = SwitchMemtable(cfd, context);
    if (cfd->Unref()) {
      delete cfd;
    }
    if (!status.ok()) {
      return status;
    }
  }
  return Status::OK();
}

#ifndef ROCKSDB_LITE
void DBImpl::NotifyOnMemTableSealed(ColumnFamilyData* cfd,
                                    const MemTableInfo& mem_table_info) {
  if (immutable_db_options_.listeners.size() == 0U) {
    return;
  }
  if (shutting_down_.load(std::memory_order_acquire)) {
    return;
  }

  for (auto listener : immutable_db_options_.listeners) {
    listener->OnMemTableSealed(mem_table_info);
  }
}
#endif  // ROCKSDB_LITE

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::SwitchMemtable(ColumnFamilyData* cfd, WriteContext* context) {
  mutex_.AssertHeld();
  WriteThread::Writer nonmem_w;
  if (concurrent_prepare_) {
    // SwitchMemtable is a rare event. To simply the reasoning, we make sure
    // that there is no concurrent thread writing to WAL.
    nonmem_write_thread_.EnterUnbatched(&nonmem_w, &mutex_);
  }

  unique_ptr<WritableFile> lfile;
  log::Writer* new_log = nullptr;
  MemTable* new_mem = nullptr;

  // In case of pipelined write is enabled, wait for all pending memtable
  // writers.
  if (immutable_db_options_.enable_pipelined_write) {
    write_thread_.WaitForMemTableWriters();
  }

  // Attempt to switch to a new memtable and trigger flush of old.
  // Do this without holding the dbmutex lock.
  assert(versions_->prev_log_number() == 0);
  if (concurrent_prepare_) {
    log_write_mutex_.Lock();
  }
  bool creating_new_log = !log_empty_;
  if (concurrent_prepare_) {
    log_write_mutex_.Unlock();
  }
  uint64_t recycle_log_number = 0;
  if (creating_new_log && immutable_db_options_.recycle_log_file_num &&
      !log_recycle_files.empty()) {
    recycle_log_number = log_recycle_files.front();
    log_recycle_files.pop_front();
  }
  uint64_t new_log_number =
      creating_new_log ? versions_->NewFileNumber() : logfile_number_;
  SuperVersion* new_superversion = nullptr;
  const MutableCFOptions mutable_cf_options = *cfd->GetLatestMutableCFOptions();

  // Set current_memtble_info for memtable sealed callback
#ifndef ROCKSDB_LITE
  MemTableInfo memtable_info;
  memtable_info.cf_name = cfd->GetName();
  memtable_info.first_seqno = cfd->mem()->GetFirstSequenceNumber();
  memtable_info.earliest_seqno = cfd->mem()->GetEarliestSequenceNumber();
  memtable_info.num_entries = cfd->mem()->num_entries();
  memtable_info.num_deletes = cfd->mem()->num_deletes();
#endif  // ROCKSDB_LITE
  // Log this later after lock release. It may be outdated, e.g., if background
  // flush happens before logging, but that should be ok.
  int num_imm_unflushed = cfd->imm()->NumNotFlushed();
  DBOptions db_options =
      BuildDBOptions(immutable_db_options_, mutable_db_options_);
  const auto preallocate_block_size =
    GetWalPreallocateBlockSize(mutable_cf_options.write_buffer_size);
  mutex_.Unlock();
  Status s;
  {
    if (creating_new_log) {
      EnvOptions opt_env_opt =
          env_->OptimizeForLogWrite(env_options_, db_options);
      if (recycle_log_number) {
        ROCKS_LOG_INFO(immutable_db_options_.info_log,
                       "reusing log %" PRIu64 " from recycle list\n",
                       recycle_log_number);
        s = env_->ReuseWritableFile(
            LogFileName(immutable_db_options_.wal_dir, new_log_number),
            LogFileName(immutable_db_options_.wal_dir, recycle_log_number),
            &lfile, opt_env_opt);
      } else {
        s = NewWritableFile(
            env_, LogFileName(immutable_db_options_.wal_dir, new_log_number),
            &lfile, opt_env_opt);
      }
      if (s.ok()) {
        // Our final size should be less than write_buffer_size
        // (compression, etc) but err on the side of caution.

        // use preallocate_block_size instead
        // of calling GetWalPreallocateBlockSize()
        lfile->SetPreallocationBlockSize(preallocate_block_size);
        unique_ptr<WritableFileWriter> file_writer(
            new WritableFileWriter(std::move(lfile), opt_env_opt));
        new_log = new log::Writer(
            std::move(file_writer), new_log_number,
            immutable_db_options_.recycle_log_file_num > 0, manual_wal_flush_);
      }
    }

    if (s.ok()) {
      SequenceNumber seq = versions_->LastSequence();
      new_mem = cfd->ConstructNewMemtable(mutable_cf_options, seq);
      new_superversion = new SuperVersion();
    }

#ifndef ROCKSDB_LITE
    // PLEASE NOTE: We assume that there are no failable operations
    // after lock is acquired below since we are already notifying
    // client about mem table becoming immutable.
    NotifyOnMemTableSealed(cfd, memtable_info);
#endif //ROCKSDB_LITE
  }
  ROCKS_LOG_INFO(immutable_db_options_.info_log,
                 "[%s] New memtable created with log file: #%" PRIu64
                 ". Immutable memtables: %d.\n",
                 cfd->GetName().c_str(), new_log_number, num_imm_unflushed);
  mutex_.Lock();
  if (!s.ok()) {
    // how do we fail if we're not creating new log?
    assert(creating_new_log);
    assert(!new_mem);
    assert(!new_log);
    if (concurrent_prepare_) {
      nonmem_write_thread_.ExitUnbatched(&nonmem_w);
    }
    return s;
  }
  if (creating_new_log) {
    log_write_mutex_.Lock();
    logfile_number_ = new_log_number;
    assert(new_log != nullptr);
    log_empty_ = true;
    log_dir_synced_ = false;
    if (!logs_.empty()) {
      // Alway flush the buffer of the last log before switching to a new one
      log::Writer* cur_log_writer = logs_.back().writer;
      cur_log_writer->WriteBuffer();
    }
    logs_.emplace_back(logfile_number_, new_log);
    alive_log_files_.push_back(LogFileNumberSize(logfile_number_));
    log_write_mutex_.Unlock();
  }
  for (auto loop_cfd : *versions_->GetColumnFamilySet()) {
    // all this is just optimization to delete logs that
    // are no longer needed -- if CF is empty, that means it
    // doesn't need that particular log to stay alive, so we just
    // advance the log number. no need to persist this in the manifest
    if (loop_cfd->mem()->GetFirstSequenceNumber() == 0 &&
        loop_cfd->imm()->NumNotFlushed() == 0) {
      if (creating_new_log) {
        loop_cfd->SetLogNumber(logfile_number_);
      }
      loop_cfd->mem()->SetCreationSeq(versions_->LastSequence());
    }
  }

  cfd->mem()->SetNextLogNumber(logfile_number_);
  cfd->imm()->Add(cfd->mem(), &context->memtables_to_free_);
  new_mem->Ref();
  cfd->SetMemtable(new_mem);
  context->superversions_to_free_.push_back(InstallSuperVersionAndScheduleWork(
      cfd, new_superversion, mutable_cf_options));
  if (concurrent_prepare_) {
    nonmem_write_thread_.ExitUnbatched(&nonmem_w);
  }
  return s;
}

size_t DBImpl::GetWalPreallocateBlockSize(uint64_t write_buffer_size) const {
  mutex_.AssertHeld();
  size_t bsize = write_buffer_size / 10 + write_buffer_size;
  // Some users might set very high write_buffer_size and rely on
  // max_total_wal_size or other parameters to control the WAL size.
  if (mutable_db_options_.max_total_wal_size > 0) {
    bsize = std::min<size_t>(bsize, mutable_db_options_.max_total_wal_size);
  }
  if (immutable_db_options_.db_write_buffer_size > 0) {
    bsize = std::min<size_t>(bsize, immutable_db_options_.db_write_buffer_size);
  }
  if (immutable_db_options_.write_buffer_manager &&
      immutable_db_options_.write_buffer_manager->enabled()) {
    bsize = std::min<size_t>(
        bsize, immutable_db_options_.write_buffer_manager->buffer_size());
  }

  return bsize;
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, ColumnFamilyHandle* column_family,
               const Slice& key, const Slice& value) {
  // Pre-allocate size of write batch conservatively.
  // 8 bytes are taken by header, 4 bytes for count, 1 byte for type,
  // and we allocate 11 extra bytes for key length, as well as value length.
  WriteBatch batch(key.size() + value.size() + 24);
  batch.Put(column_family, key, value);
  return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, ColumnFamilyHandle* column_family,
                  const Slice& key) {
  WriteBatch batch;
  batch.Delete(column_family, key);
  return Write(opt, &batch);
}

Status DB::SingleDelete(const WriteOptions& opt,
                        ColumnFamilyHandle* column_family, const Slice& key) {
  WriteBatch batch;
  batch.SingleDelete(column_family, key);
  return Write(opt, &batch);
}

Status DB::DeleteRange(const WriteOptions& opt,
                       ColumnFamilyHandle* column_family,
                       const Slice& begin_key, const Slice& end_key) {
  WriteBatch batch;
  batch.DeleteRange(column_family, begin_key, end_key);
  return Write(opt, &batch);
}

Status DB::Merge(const WriteOptions& opt, ColumnFamilyHandle* column_family,
                 const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Merge(column_family, key, value);
  return Write(opt, &batch);
}
}  // namespace rocksdb
