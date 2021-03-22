//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <assert.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

#include "db/write_callback.h"
#include "monitoring/instrumented_mutex.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/types.h"
#include "rocksdb/write_batch.h"
#include "util/autovector.h"

namespace rocksdb {

class WriteThread {
 public:
  enum State : uint8_t {
    // The initial state of a writer.  This is a Writer that is
    // waiting in JoinBatchGroup.  This state can be left when another
    // thread informs the waiter that it has become a group leader
    // (-> STATE_GROUP_LEADER), when a leader that has chosen to be
    // non-parallel informs a follower that its writes have been committed
    // (-> STATE_COMPLETED), or when a leader that has chosen to perform
    // updates in parallel and needs this Writer to apply its batch (->
    // STATE_PARALLEL_FOLLOWER).
    STATE_INIT = 1,

    // The state used to inform a waiting Writer that it has become the
    // leader, and it should now build a write batch group.  Tricky:
    // this state is not used if newest_writer_ is empty when a writer
    // enqueues itself, because there is no need to wait (or even to
    // create the mutex and condvar used to wait) in that case.  This is
    // a terminal state unless the leader chooses to make this a parallel
    // batch, in which case the last parallel worker to finish will move
    // the leader to STATE_COMPLETED.
    STATE_GROUP_LEADER = 2,

    /*
     * leader要么为整个组写memtable，要么通过调用LaunchParallelMemTableWrite来启动
     * 一个对memtable的并行gruop写。 
     */
    // The state used to inform a waiting writer that it has become the
    // leader of memtable writer group. The leader will either write
    // memtable for the whole group, or launch a parallel group write
    // to memtable by calling LaunchParallelMemTableWrite.
    STATE_MEMTABLE_WRITER_LEADER = 4,

    /* 并行写memtable */
    // The state used to inform a waiting writer that it has become a
    // parallel memtable writer. It can be the group leader who launch the
    // praallel writer group, or one of the followers. The writer should then
    // apply its batch to the memtable concurrently and call
    // CompleteParallelMemTableWriter.
    STATE_PARALLEL_MEMTABLE_WRITER = 8,

    // A follower whose writes have been applied, or a parallel leader
    // whose followers have all finished their work.  This is a terminal
    // state.
    STATE_COMPLETED = 16,

    // A state indicating that the thread may be waiting using StateMutex()
    // and StateCondVar()
    STATE_LOCKED_WAITING = 32,
  };

  struct Writer;

  /*
   * 下面的代码看起来似乎这样的：多个Writer组成一个链表，构成一个WriteGroup
   */

  struct WriteGroup {
    Writer* leader = nullptr;
    /* 当前write group最后一个writer，参考EnterAsBatchGroupLeader函数 */
    Writer* last_writer = nullptr;
    /* 当前write group最后的sequence */
    SequenceNumber last_sequence;
    // before running goes to zero, status needs leader->StateMutex()
    Status status;
    /* 表示当前group中，处于PARALLEL_MEMTABLE_WRITER状态的wirter的个数 */
    std::atomic<size_t> running;
    /* size表示有多少个writer处于当前的WriteGroup中 */
    size_t size = 0;

    struct Iterator {
      Writer* writer;
      Writer* last_writer;

      explicit Iterator(Writer* w, Writer* last)
          : writer(w), last_writer(last) {}

      Writer* operator*() const { return writer; }

      Iterator& operator++() {
        assert(writer != nullptr);
        if (writer == last_writer) {
          writer = nullptr;
        } else {
          writer = writer->link_newer;
        }
        return *this;
      }

      bool operator!=(const Iterator& other) const {
        return writer != other.writer;
      }
    };

    Iterator begin() const { return Iterator(leader, last_writer); }
    Iterator end() const { return Iterator(nullptr, nullptr); }
  };

  /*
   * rocksdb将每个将要写入的WriteBatch对象封装为一个Writer对象，其中记录了：
   * + 本次write的wal配置，比如是否要做sync，要写入的wal日志编号等等
   * + 本次要write的数据，即WriteBatch对象
   * + 本次write所属的write group
   * + 本次write的第一个数据对象的SN
   * + 前置writer
   * + 后置writer
   */
  // Information kept for every waiting writer.
  struct Writer {
    WriteBatch* batch;
    bool sync;
    bool no_slowdown;
    bool disable_wal;
    bool disable_memtable;
    /* 记录当前batch要写入的wal日志文件编号 */
    uint64_t log_used;  // log number that this batch was inserted into
    /* TODO: log_ref哪里初始化的 */
    uint64_t log_ref;   // log number that memtable insert should reference
    WriteCallback* callback;
    bool made_waitable;          // records lazy construction of mutex and cv
    std::atomic<uint8_t> state;  // write under StateMutex() or pre-link
    /*
     * 关于write_group的赋值，参见EnterAsBatchGroupLeader函数 
     */
    WriteGroup* write_group;
    SequenceNumber sequence;  // the sequence number to use for the first key
    Status status;            // status of memtable inserter
    /* callback_status默认初始化为ok */
    Status callback_status;   // status returned by callback->Callback()
    /*
     * std::aligned_storage的作用：不需要通过构造函数，申请特定大小的内存空间。参见：
     * https://en.cppreference.com/w/cpp/types/aligned_storage
     */
    std::aligned_storage<sizeof(std::mutex)>::type state_mutex_bytes;
    std::aligned_storage<sizeof(std::condition_variable)>::type state_cv_bytes;
    /* link_older前置指针，link_newer后置指针 */
    Writer* link_older;  // read/write only before linking, or as leader
    Writer* link_newer;  // lazy, read/write only before linking, or as leader

    Writer()
        : batch(nullptr),
          sync(false),
          no_slowdown(false),
          disable_wal(false),
          disable_memtable(false),
          log_used(0),
          log_ref(0),
          callback(nullptr),
          made_waitable(false),
          state(STATE_INIT),
          write_group(nullptr),
          link_older(nullptr),
          link_newer(nullptr) {}

    Writer(const WriteOptions& write_options, WriteBatch* _batch,
           WriteCallback* _callback, uint64_t _log_ref, bool _disable_memtable)
        : batch(_batch),
          sync(write_options.sync),
          no_slowdown(write_options.no_slowdown),
          disable_wal(write_options.disableWAL),
          disable_memtable(_disable_memtable),
          log_used(0),
          log_ref(_log_ref),
          callback(_callback),
          made_waitable(false),
          state(STATE_INIT),
          write_group(nullptr),
          link_older(nullptr),
          link_newer(nullptr) {}

    ~Writer() {
      if (made_waitable) {
        StateMutex().~mutex();
        StateCV().~condition_variable();
      }
    }

    bool CheckCallback(DB* db) {
      if (callback != nullptr) {
        callback_status = callback->Callback(db);
      }
      return callback_status.ok();
    }

    void CreateMutex() {
      if (!made_waitable) {
        // Note that made_waitable is tracked separately from state
        // transitions, because we can't atomically create the mutex and
        // link into the list.
        made_waitable = true;
        /*
         * placement new operator, 参见：
         * https://www.geeksforgeeks.org/placement-new-operator-cpp/ 
         */
        new (&state_mutex_bytes) std::mutex;
        new (&state_cv_bytes) std::condition_variable;
      }
    }

    // returns the aggregate status of this Writer
    Status FinalStatus() {
      if (!status.ok()) {
        // a non-ok memtable write status takes presidence
        assert(callback == nullptr || callback_status.ok());
        return status;
      } else if (!callback_status.ok()) {
        // if the callback failed then that is the status we want
        // because a memtable insert should not have been attempted
        assert(callback != nullptr);
        assert(status.ok());
        return callback_status;
      } else {
        // if there is no callback then we only care about
        // the memtable insert status
        assert(callback == nullptr || callback_status.ok());
        return status;
      }
    }

    bool CallbackFailed() {
      return (callback != nullptr) && !callback_status.ok();
    }

    bool ShouldWriteToMemtable() {
      return status.ok() && !CallbackFailed() && !disable_memtable;
    }

    bool ShouldWriteToWAL() {
      return status.ok() && !CallbackFailed() && !disable_wal;
    }

    // No other mutexes may be acquired while holding StateMutex(), it is
    // always last in the order
    std::mutex& StateMutex() {
      assert(made_waitable);
      return *static_cast<std::mutex*>(static_cast<void*>(&state_mutex_bytes));
    }

    std::condition_variable& StateCV() {
      assert(made_waitable);
      return *static_cast<std::condition_variable*>(
                 static_cast<void*>(&state_cv_bytes));
    }
  };

  struct AdaptationContext {
    const char* name;
    std::atomic<int32_t> value;

    explicit AdaptationContext(const char* name0) : name(name0), value(0) {}
  };

  /*
   * WriteThread构造调用栈
#0  rocksdb::WriteThread::WriteThread (this=0x832730, db_options=...) at /root/code/rocksdb/db/write_thread.cc:26
#1  0x00007ffff728b3fc in rocksdb::DBImpl::DBImpl (this=0x832040, options=..., dbname=...) at /root/code/rocksdb/db/db_impl.cc:193
#2  0x00007ffff72e628c in rocksdb::DB::Open (db_options=..., dbname=..., column_families=..., handles=0x7fffffffd730, dbptr=0x7fffffffdd28)
    at /root/code/rocksdb/db/db_impl_open.cc:951
#3  0x00007ffff72e5f62 in rocksdb::DB::Open (options=..., dbname=..., dbptr=0x7fffffffdd28) at /root/code/rocksdb/db/db_impl_open.cc:919
#4  0x00000000004081d4 in main () at test.cpp:16
   */
  explicit WriteThread(const ImmutableDBOptions& db_options);

  virtual ~WriteThread() = default;

  // IMPORTANT: None of the methods in this class rely on the db mutex
  // for correctness. All of the methods except JoinBatchGroup and
  // EnterUnbatched may be called either with or without the db mutex held.
  // Correctness is maintained by ensuring that only a single thread is
  // a leader at a time.

  // Registers w as ready to become part of a batch group, waits until the
  // caller should perform some work, and returns the current state of the
  // writer.  If w has become the leader of a write batch group, returns
  // STATE_GROUP_LEADER.  If w has been made part of a sequential batch
  // group and the leader has performed the write, returns STATE_DONE.
  // If w has been made part of a parallel batch group and is responsible
  // for updating the memtable, returns STATE_PARALLEL_FOLLOWER.
  //
  // The db mutex SHOULD NOT be held when calling this function, because
  // it will block.
  //
  // Writer* w:        Writer to be executed as part of a batch group
  void JoinBatchGroup(Writer* w);

  // Constructs a write batch group led by leader, which should be a
  // Writer passed to JoinBatchGroup on the current thread.
  //
  // Writer* leader:          Writer that is STATE_GROUP_LEADER
  // WriteGroup* write_group: Out-param of group members
  // returns:                 Total batch group byte size
  size_t EnterAsBatchGroupLeader(Writer* leader, WriteGroup* write_group);

  // Unlinks the Writer-s in a batch group, wakes up the non-leaders,
  // and wakes up the next leader (if any).
  //
  // WriteGroup* write_group: the write group
  // Status status:           Status of write operation
  void ExitAsBatchGroupLeader(WriteGroup& write_group, Status status);

  // Exit batch group on behalf of batch group leader.
  void ExitAsBatchGroupFollower(Writer* w);

  // Constructs a write batch group led by leader from newest_memtable_writers_
  // list. The leader should either write memtable for the whole group and
  // call ExitAsMemTableWriter, or launch parallel memtable write through
  // LaunchParallelMemTableWriters.
  void EnterAsMemTableWriter(Writer* leader, WriteGroup* write_grup);

  // Memtable writer group leader, or the last finished writer in a parallel
  // write group, exit from the newest_memtable_writers_ list, and wake up
  // the next leader if needed.
  void ExitAsMemTableWriter(Writer* self, WriteGroup& write_group);

  // Causes JoinBatchGroup to return STATE_PARALLEL_FOLLOWER for all of the
  // non-leader members of this write batch group.  Sets Writer::sequence
  // before waking them up.
  //
  // WriteGroup* write_group: Extra state used to coordinate the parallel add
  void LaunchParallelMemTableWriters(WriteGroup* write_group);

  // Reports the completion of w's batch to the parallel group leader, and
  // waits for the rest of the parallel batch to complete.  Returns true
  // if this thread is the last to complete, and hence should advance
  // the sequence number and then call EarlyExitParallelGroup, false if
  // someone else has already taken responsibility for that.
  bool CompleteParallelMemTableWriter(Writer* w);

  // Waits for all preceding writers (unlocking mu while waiting), then
  // registers w as the currently proceeding writer.
  //
  // Writer* w:              A Writer not eligible for batching
  // InstrumentedMutex* mu:  The db mutex, to unlock while waiting
  // REQUIRES: db mutex held
  void EnterUnbatched(Writer* w, InstrumentedMutex* mu);

  // Completes a Writer begun with EnterUnbatched, unblocking subsequent
  // writers.
  void ExitUnbatched(Writer* w);

  // Wait for all parallel memtable writers to finish, in case pipelined
  // write is enabled.
  void WaitForMemTableWriters();

  SequenceNumber UpdateLastSequence(SequenceNumber sequence) {
    if (sequence > last_sequence_) {
      last_sequence_ = sequence;
    }
    return last_sequence_;
  }

 private:
  // See AwaitState.
  const uint64_t max_yield_usec_;
  const uint64_t slow_yield_usec_;

  // Allow multiple writers write to memtable concurrently.
  const bool allow_concurrent_memtable_write_;

  /*
   * pipelined(流水线)写入方式，默认的写入方式中，一个batch的需要完成WAL之后，再完成Memtable的写入才选出下一个Leader.
   * 而Pipelined写入中不需要等待Memtable写入完成，即当WAL写入完成之后，即可选出下一个Leader继续完成下一个batch的写入从而达到Pipelined的效果.
   * 参见：https://zhuanlan.zhihu.com/p/45666093
   */
  // Enable pipelined write to WAL and memtable.
  const bool enable_pipelined_write_;

  // Points to the newest pending writer. Only leader can remove
  // elements, adding can be done lock-free by anybody.
  std::atomic<Writer*> newest_writer_;

  // Points to the newest pending memtable writer. Used only when pipelined
  // write is enabled.
  std::atomic<Writer*> newest_memtable_writer_;

  // The last sequence that have been consumed by a writer. The sequence
  // is not necessary visible to reads because the writer can be ongoing.
  SequenceNumber last_sequence_;

  // Waits for w->state & goal_mask using w->StateMutex().  Returns
  // the state that satisfies goal_mask.
  uint8_t BlockingAwaitState(Writer* w, uint8_t goal_mask);

  // Blocks until w->state & goal_mask, returning the state value
  // that satisfied the predicate.  Uses ctx to adaptively use
  // std::this_thread::yield() to avoid mutex overheads.  ctx should be
  // a context-dependent static.
  uint8_t AwaitState(Writer* w, uint8_t goal_mask, AdaptationContext* ctx);

  // Set writer state and wake the writer up if it is waiting.
  void SetState(Writer* w, uint8_t new_state);

  // Links w into the newest_writer list. Return true if w was linked directly
  // into the leader position.  Safe to call from multiple threads without
  // external locking.
  bool LinkOne(Writer* w, std::atomic<Writer*>* newest_writer);

  // Link write group into the newest_writer list as a whole, while keeping the
  // order of the writers unchanged. Return true if the group was linked
  // directly into the leader position.
  bool LinkGroup(WriteGroup& write_group, std::atomic<Writer*>* newest_writer);

  // Computes any missing link_newer links.  Should not be called
  // concurrently with itself.
  void CreateMissingNewerLinks(Writer* head);

  // Set the leader in write_group to completed state and remove it from the
  // write group.
  void CompleteLeader(WriteGroup& write_group);

  // Set a follower in write_group to completed state and remove it from the
  // write group.
  void CompleteFollower(Writer* w, WriteGroup& write_group);
};

}  // namespace rocksdb
