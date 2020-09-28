/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * dispatcher.cpp
 *      Parallel recovery has a centralized log dispatcher which runs inside
 *      the StartupProcess.  The dispatcher is responsible for managing the
 *      life cycle of PageRedoWorkers and the TxnRedoWorker, analyzing log
 *      records and dispatching them to workers for processing.
 *
 * IDENTIFICATION
 *        src/gausskernel/storage/access/transam/parallel_recovery/dispatcher.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "postmaster/startup.h"
#include "access/clog.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/nbtree.h"
#include "access/xlogreader.h"
#include "access/gist_private.h"
#include "access/multixact.h"
#include "access/spgist_private.h"
#include "access/gin_private.h"
#include "access/xlogutils.h"
#include "access/gin.h"

#include "catalog/storage_xlog.h"
#include "storage/buf_internals.h"
#include "storage/ipc.h"
#include "storage/standby.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/guc.h"
#include "utils/relmapper.h"

#include "portability/instr_time.h"

#include "access/parallel_recovery/dispatcher.h"
#include "access/parallel_recovery/page_redo.h"
#include "access/multi_redo_api.h"

#include "access/parallel_recovery/txn_redo.h"
#include "access/parallel_recovery/spsc_blocking_queue.h"
#include "access/parallel_recovery/redo_item.h"

#include "catalog/storage.h"
#include <sched.h>
#include "utils/memutils.h"

#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "commands/sequence.h"

#include "replication/slot.h"
#include "gssignal/gs_signal.h"
#include "utils/atomic.h"
#include "pgstat.h"

#ifdef PGXC
#include "pgxc/pgxc.h"
#endif

extern THR_LOCAL bool redo_oldversion_xlog;

namespace parallel_recovery {

typedef struct RmgrDispatchData {
    bool (*rm_dispatch)(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
    bool (*rm_loginfovalid)(XLogReaderState* record, uint8 minInfo, uint8 maxInfo);
    RmgrId rm_id;
    uint8 rm_mininfo;
    uint8 rm_maxinfo;
} RmgrDispatchData;

LogDispatcher* g_dispatcher = NULL;

static const int XLOG_INFO_SHIFT_SIZE = 4; /* xlog info flag shift size */

static const int32 MAX_PENDING = 1;
static const int32 MAX_PENDING_STANDBY = 1;
static const int32 ITEM_QUQUE_SIZE_RATIO = 10;

static const uint32 EXIT_WAIT_DELAY = 100; /* 100 us */

typedef void* (*GetStateFunc)(PageRedoWorker* worker);

static void AddWorkerToSet(uint32);
static void** CollectStatesFromWorkers(GetStateFunc);
static void GetWorkerIds(XLogReaderState* record, uint32 designatedWorker, bool rnodedispatch);
static LogDispatcher* CreateDispatcher();
static void DestroyRecoveryWorkers();

static void DispatchRecordWithPages(XLogReaderState*, List*, bool);
static void DispatchRecordWithoutPage(XLogReaderState*, List*);
static void DispatchTxnRecord(XLogReaderState*, List*, TimestampTz, bool);
static void StartPageRedoWorkers(uint32);
static void StopRecoveryWorkers(int, Datum);
static bool XLogWillChangeStandbyState(XLogReaderState*);
static bool StandbyWillChangeStandbyState(XLogReaderState*);
static void DispatchToSpecPageWorker(XLogReaderState* record, List* expectedTLIs, bool waittrxnsync);

static bool DispatchXLogRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchXactRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchSmgrRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchCLogRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchHashRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchDataBaseRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchTableSpaceRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchMultiXactRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchRelMapRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchStandbyRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchHeap2Record(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchHeapRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchSeqRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchGinRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchGistRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchSpgistRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchRepSlotRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchHeap3Record(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchDefaultRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
#ifdef ENABLE_MULTIPLE_NODES
static bool DispatchBarrierRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
#endif
static bool DispatchBtreeRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool DispatchMotRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime);
static bool RmgrRecordInfoValid(XLogReaderState* record, uint8 minInfo, uint8 maxInfo);
static bool RmgrGistRecordInfoValid(XLogReaderState* record, uint8 minInfo, uint8 maxInfo);
RedoWaitInfo redo_get_io_event(int32 event_id);

/* dispatchTable must consistent with RmgrTable */
static const RmgrDispatchData g_dispatchTable[RM_MAX_ID + 1] = {
    {DispatchXLogRecord, RmgrRecordInfoValid, RM_XLOG_ID, XLOG_CHECKPOINT_SHUTDOWN, XLOG_FPI},
    {DispatchXactRecord, RmgrRecordInfoValid, RM_XACT_ID, XLOG_XACT_COMMIT, XLOG_XACT_COMMIT_COMPACT},
    {DispatchSmgrRecord, RmgrRecordInfoValid, RM_SMGR_ID, XLOG_SMGR_CREATE, XLOG_SMGR_TRUNCATE},
    {DispatchCLogRecord, RmgrRecordInfoValid, RM_CLOG_ID, CLOG_ZEROPAGE, CLOG_TRUNCATE},
    {DispatchDataBaseRecord, RmgrRecordInfoValid, RM_DBASE_ID, XLOG_DBASE_CREATE, XLOG_DBASE_DROP},
    {DispatchTableSpaceRecord, RmgrRecordInfoValid, RM_TBLSPC_ID, XLOG_TBLSPC_CREATE, XLOG_TBLSPC_RELATIVE_CREATE},
    {DispatchMultiXactRecord,
        RmgrRecordInfoValid,
        RM_MULTIXACT_ID,
        XLOG_MULTIXACT_ZERO_OFF_PAGE,
        XLOG_MULTIXACT_CREATE_ID},
    {DispatchRelMapRecord, RmgrRecordInfoValid, RM_RELMAP_ID, XLOG_RELMAP_UPDATE, XLOG_RELMAP_UPDATE},
#ifdef ENABLE_MULTIPLE_NODES
    {DispatchStandbyRecord, RmgrRecordInfoValid, RM_STANDBY_ID, XLOG_STANDBY_LOCK, XLOG_STANDBY_CSN},
#else
    {DispatchStandbyRecord, RmgrRecordInfoValid, RM_STANDBY_ID, XLOG_STANDBY_LOCK, XLOG_STANDBY_CSN_ABORTED},
#endif
    {DispatchHeap2Record, RmgrRecordInfoValid, RM_HEAP2_ID, XLOG_HEAP2_FREEZE, XLOG_HEAP2_LOGICAL_NEWPAGE},
    {DispatchHeapRecord, RmgrRecordInfoValid, RM_HEAP_ID, XLOG_HEAP_INSERT, XLOG_HEAP_INPLACE},
    {DispatchBtreeRecord, RmgrRecordInfoValid, RM_BTREE_ID, XLOG_BTREE_INSERT_LEAF, XLOG_BTREE_REUSE_PAGE},
    {DispatchHashRecord, NULL, RM_HASH_ID, 0, 0},
    {DispatchGinRecord, RmgrRecordInfoValid, RM_GIN_ID, XLOG_GIN_CREATE_INDEX, XLOG_GIN_VACUUM_DATA_LEAF_PAGE},
    /* XLOG_GIST_PAGE_DELETE is not used and info isn't continus  */
    {DispatchGistRecord, RmgrGistRecordInfoValid, RM_GIST_ID, 0, 0},
    {DispatchSeqRecord, RmgrRecordInfoValid, RM_SEQ_ID, XLOG_SEQ_LOG, XLOG_SEQ_LOG},
    {DispatchSpgistRecord, RmgrRecordInfoValid, RM_SPGIST_ID, XLOG_SPGIST_CREATE_INDEX, XLOG_SPGIST_VACUUM_REDIRECT},
    {DispatchRepSlotRecord, RmgrRecordInfoValid, RM_SLOT_ID, XLOG_SLOT_CREATE, XLOG_TERM_LOG},
    {DispatchHeap3Record, RmgrRecordInfoValid, RM_HEAP3_ID, XLOG_HEAP3_NEW_CID, XLOG_HEAP3_REWRITE},
#ifdef ENABLE_MULTIPLE_NODES
    {DispatchBarrierRecord, NULL, RM_BARRIER_ID, 0, 0},
#endif
    {DispatchMotRecord, NULL, RM_MOT_ID, 0, 0},
};

/* Run from the dispatcher and txn worker thread. */
bool OnHotStandBy()
{
    return t_thrd.xlog_cxt.standbyState >= STANDBY_INITIALIZED;
}

void RearrangeWorkers()
{
    PageRedoWorker* tmpReadyPageWorkers[MOST_FAST_RECOVERY_LIMIT] = {};
    PageRedoWorker* tmpUnReadyPageWorkers[MOST_FAST_RECOVERY_LIMIT] = {};

    uint32 nextReadyIndex = 0;
    uint32 nextunReadyIndex = 0;
    for (uint32 i = 0; i < g_instance.comm_cxt.predo_cxt.totalNum; ++i) {
        uint32 state = pg_atomic_read_u32(&(g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadState));
        if (state == PAGE_REDO_WORKER_READY) {
            tmpReadyPageWorkers[nextReadyIndex] = g_dispatcher->pageWorkers[i];
            ++nextReadyIndex;
        } else {
            tmpUnReadyPageWorkers[nextunReadyIndex] = g_dispatcher->pageWorkers[i];
            ++nextunReadyIndex;
        }
    }

    for (uint32 i = 0; i < nextReadyIndex; ++i) {
        ereport(LOG,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("RearrangeWorkers, rearrange ready workers originWorkerId :%u, threadId:%lu, "
                       "newWorkerId:%u",
                    tmpReadyPageWorkers[i]->id,
                    tmpReadyPageWorkers[i]->tid.thid,
                    i)));
        g_dispatcher->pageWorkers[i] = tmpReadyPageWorkers[i];
        g_dispatcher->pageWorkers[i]->id = i;
    }

    for (uint32 i = 0; i < nextunReadyIndex; ++i) {
        ereport(LOG,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("RearrangeWorkers, rearrange ready workers originWorkerId :%u, threadId:%lu, "
                       "newWorkerId:%u",
                    tmpUnReadyPageWorkers[i]->id,
                    tmpUnReadyPageWorkers[i]->tid.thid,
                    i)));
        g_dispatcher->pageWorkers[i + nextReadyIndex] = tmpUnReadyPageWorkers[i];
    }

    g_dispatcher->pageWorkerCount = nextReadyIndex;
}

const int REDO_WAIT_SLEEP_TIME = 5000; /* 5ms */
const int MAX_REDO_WAIT_LOOP = 24000;  /* 5ms*24000 is 2min */

uint32 GetReadyWorker()
{
    uint32 readyWorkerCnt = 0;

    for (uint32 i = 0; i < g_instance.comm_cxt.predo_cxt.totalNum; i++) {
        uint32 state = pg_atomic_read_u32(&(g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadState));
        if (state == PAGE_REDO_WORKER_READY) {
            ++readyWorkerCnt;
        }
    }
    return readyWorkerCnt;
}

void WaitWorkerReady()
{
    uint32 waitLoop = 0;
    uint32 readyWorkerCnt = 0;
    /* MAX wait 2min */
    for (waitLoop = 0; waitLoop < MAX_REDO_WAIT_LOOP; ++waitLoop) {
        readyWorkerCnt = GetReadyWorker();
        if (readyWorkerCnt == g_instance.comm_cxt.predo_cxt.totalNum) {
            ereport(LOG,
                (errmodule(MOD_REDO),
                    errcode(ERRCODE_LOG),
                    errmsg("WaitWorkerReady total worker count:%u, readyWorkerCnt:%u",
                        g_dispatcher->totalWorkerCount,
                        readyWorkerCnt)));
            break;
        }
        pg_usleep(REDO_WAIT_SLEEP_TIME);
    }
    SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.rwlock));
    g_instance.comm_cxt.predo_cxt.state = REDO_STARTING_END;
    SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.rwlock));
    readyWorkerCnt = GetReadyWorker();
    if (waitLoop == MAX_REDO_WAIT_LOOP && readyWorkerCnt == 0) {
        ereport(PANIC,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("WaitWorkerReady failed, no worker is ready for work. totalWorkerCount :%u",
                    g_dispatcher->totalWorkerCount)));
    }

    ereport(LOG,
        (errmodule(MOD_REDO),
            errcode(ERRCODE_LOG),
            errmsg("WaitWorkerReady total worker count:%u, readyWorkerCnt:%u",
                g_dispatcher->totalWorkerCount,
                readyWorkerCnt)));
    RearrangeWorkers();
}

void CheckAlivePageWorkers()
{
    for (uint32 i = 0; i < MOST_FAST_RECOVERY_LIMIT; ++i) {
        if (g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadState != PAGE_REDO_WORKER_INVALID) {
            ereport(PANIC,
                (errmodule(MOD_REDO),
                    errcode(ERRCODE_LOG),
                    errmsg("CheckAlivePageWorkers: thread %lu is still alive",
                        g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadId)));
        }
        g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadId = 0;
    }
    g_instance.comm_cxt.predo_cxt.totalNum = 0;
}

/* Run from the dispatcher thread. */
void StartRecoveryWorkers()
{
    if (get_real_recovery_parallelism() > 1) {
        CheckAlivePageWorkers();
        g_dispatcher = CreateDispatcher();
        g_dispatcher->oldCtx = MemoryContextSwitchTo(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
        g_dispatcher->txnWorker = StartTxnRedoWorker();
        if (g_dispatcher->txnWorker != NULL)
            StartPageRedoWorkers(get_real_recovery_parallelism());

        ereport(LOG,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[PR]: max=%d, thrd=%d, workers=%u",
                    g_instance.attr.attr_storage.max_recovery_parallelism,
                    get_real_recovery_parallelism(),
                    g_dispatcher->pageWorkerCount)));
        WaitWorkerReady();
        SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.rwlock));
        g_instance.comm_cxt.predo_cxt.state = REDO_IN_PROGRESS;
        SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.rwlock));
        on_shmem_exit(StopRecoveryWorkers, 0);
    }
}

void DumpDispatcher()
{
    knl_parallel_redo_state state;
    state = g_instance.comm_cxt.predo_cxt.state;
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        ereport(LOG,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]dispatcher : pageWorkerCount %u, state %u, curItemNum %u, maxItemNum %u",
                    g_dispatcher->pageWorkerCount,
                    (uint32)state,
                    g_dispatcher->curItemNum,
                    g_dispatcher->maxItemNum)));

        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; ++i) {
            DumpPageRedoWorker(g_dispatcher->pageWorkers[i]);
        }

        DumpTxnWorker(g_dispatcher->txnWorker);
    }
}

List* CheckImcompleteAction(List* imcompleteActionList)
{
    uint32 npageworkers = GetPageWorkerCount();
    for (uint32 i = 0; i < npageworkers; ++i) {
        List* perWorkerList = (List*)GetBTreeIncompleteActions(g_dispatcher->pageWorkers[i]);
        imcompleteActionList = lappend3(imcompleteActionList, perWorkerList);

        /* memory leak */
        ClearBTreeIncompleteActions(g_dispatcher->pageWorkers[i]);
    }
    return imcompleteActionList;
}

/* Run from the dispatcher thread. */
static LogDispatcher* CreateDispatcher()
{
    MemoryContext ctx = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "ParallelRecoveryDispatcher",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE,
        SHARED_CONTEXT);

    LogDispatcher* newDispatcher = (LogDispatcher*)MemoryContextAllocZero(ctx, sizeof(LogDispatcher));

    g_instance.comm_cxt.predo_cxt.parallelRedoCtx = ctx;
    SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.rwlock));
    g_instance.comm_cxt.predo_cxt.state = REDO_STARTING_BEGIN;
    SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.rwlock));
    if (OnHotStandBy())
        newDispatcher->pendingMax = MAX_PENDING_STANDBY;
    else
        newDispatcher->pendingMax = MAX_PENDING; /* one batch, one recorder */
    newDispatcher->totalCostTime = 0;
    newDispatcher->txnCostTime = 0;
    newDispatcher->pprCostTime = 0;
    return newDispatcher;
}

/* Run from the dispatcher thread. */
static void StartPageRedoWorkers(uint32 parallelism)
{
    g_dispatcher->pageWorkers = (PageRedoWorker**)palloc(sizeof(PageRedoWorker*) * parallelism);

    /* This is necessary to avoid the cache coherence problem. */
    /* Because we are using atomic operation to do the synchronization. */
    uint32 started;
    for (started = 0; started < parallelism; started++) {
        g_dispatcher->pageWorkers[started] = StartPageRedoWorker(started);
        if (g_dispatcher->pageWorkers[started] == NULL)
            break;
    }

    if (started == 0) {
        ereport(PANIC,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]StartPageRedoWorkers we need at least one worker thread")));
    }

    g_dispatcher->totalWorkerCount = started;
    g_instance.comm_cxt.predo_cxt.totalNum = started;
    /* (worker num + txn) * (per thread queue num) * 10 */
    g_dispatcher->maxItemNum = (started + 1) * PAGE_WORK_QUEUE_SIZE * ITEM_QUQUE_SIZE_RATIO;

    g_dispatcher->chosedWorkerIds = (uint32*)palloc(sizeof(uint32) * started);

    g_dispatcher->chosedWorkerCount = 0;
}

static void ResetChosedWorkerList()
{
    g_dispatcher->chosedWorkerCount = 0;

    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; ++i) {
        g_dispatcher->chosedWorkerIds[i] = 0;
    }
}

bool DispathCouldExit()
{
    for (uint32 i = 0; i < g_instance.comm_cxt.predo_cxt.totalNum; ++i) {
        uint32 state = pg_atomic_read_u32(&(g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadState));
        if (state == PAGE_REDO_WORKER_READY) {
            return false;
        }
    }

    return true;
}

void SetPageWorkStateByThreadId(uint32 threadState)
{
    gs_thread_t curThread = gs_thread_get_cur_thread();
    for (uint32 i = 0; i < g_instance.comm_cxt.predo_cxt.totalNum; ++i) {
        if (g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadId == curThread.thid) {
            pg_atomic_write_u32(&(g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadState), threadState);
            break;
        }
    }
}

void SendSingalToPageWorker(int signal)
{
    for (uint32 i = 0; i < g_instance.comm_cxt.predo_cxt.totalNum; ++i) {
        uint32 state = pg_atomic_read_u32(&(g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadState));
        if (state == PAGE_REDO_WORKER_READY) {
            int err = gs_signal_send(g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadId, signal);
            if (0 != err) {
                ereport(WARNING,
                    (errmsg("Dispatch kill(pid %lu, signal %d) failed: \"%s\",",
                        g_instance.comm_cxt.predo_cxt.pageRedoThreadStatusList[i].threadId,
                        signal,
                        gs_strerror(err))));
            }
        }
    }
}

/* Run from the dispatcher thread. */
static void StopRecoveryWorkers(int code, Datum arg)
{
    ereport(LOG,
        (errmodule(MOD_REDO),
            errcode(ERRCODE_LOG),
            errmsg("parallel redo workers are going to stop, "
                   "code:%d, arg:%lu",
                code,
                DatumGetUInt64(arg))));
    SendSingalToPageWorker(SIGTERM);

    uint64 count = 0;
    while (!DispathCouldExit()) {
        ++count;
        if ((count & OUTPUT_WAIT_COUNT) == OUTPUT_WAIT_COUNT) {
            ereport(WARNING,
                (errmodule(MOD_REDO), errcode(ERRCODE_LOG), errmsg("StopRecoveryWorkers wait page work exit")));
            if ((count & PRINT_ALL_WAIT_COUNT) == PRINT_ALL_WAIT_COUNT) {
                DumpDispatcher();
                ereport(
                    PANIC, (errmodule(MOD_REDO), errcode(ERRCODE_LOG), errmsg("StopRecoveryWorkers wait too long!!!")));
            }
            pg_usleep(EXIT_WAIT_DELAY);
        }
    }

    FreeAllocatedRedoItem();
    DestroyRecoveryWorkers();
    ereport(LOG, (errmodule(MOD_REDO), errcode(ERRCODE_LOG), errmsg("parallel redo(startup) thread exit")));
}

/* Run from the dispatcher thread. */
static void DestroyRecoveryWorkers()
{
    if (g_dispatcher != NULL) {
        SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
        for (uint32 i = 0; i < g_dispatcher->totalWorkerCount; i++)
            DestroyPageRedoWorker(g_dispatcher->pageWorkers[i]);
        if (g_dispatcher->txnWorker != NULL)
            DestroyTxnRedoWorker(g_dispatcher->txnWorker);
        if (g_dispatcher->chosedWorkerIds != NULL) {
            pfree(g_dispatcher->chosedWorkerIds);
            g_dispatcher->chosedWorkerIds = NULL;
        }
        if (get_real_recovery_parallelism() > 1) {
            MemoryContextSwitchTo(g_dispatcher->oldCtx);
            MemoryContextDelete(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
            g_instance.comm_cxt.predo_cxt.parallelRedoCtx = NULL;
        }
        g_dispatcher = NULL;
        SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
    }
}

static bool RmgrRecordInfoValid(XLogReaderState* record, uint8 minInfo, uint8 maxInfo)
{
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));

    if ((XLogRecGetRmid(record) == RM_HEAP2_ID) || (XLogRecGetRmid(record) == RM_HEAP_ID)) {
        info = (info & XLOG_HEAP_OPMASK);
    }
    if ((XLogRecGetRmid(record) == RM_MULTIXACT_ID)) {
        info = (info & XLOG_MULTIXACT_MASK);
    }

    info = (info >> XLOG_INFO_SHIFT_SIZE);
    minInfo = (minInfo >> XLOG_INFO_SHIFT_SIZE);
    maxInfo = (maxInfo >> XLOG_INFO_SHIFT_SIZE);

    if ((info >= minInfo) && (info <= maxInfo)) {
        return true;
    }
    return false;
}

static bool RmgrGistRecordInfoValid(XLogReaderState* record, uint8 minInfo, uint8 maxInfo)
{
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));
    if ((info == XLOG_GIST_PAGE_UPDATE) || (info == XLOG_GIST_PAGE_SPLIT) || (info == XLOG_GIST_CREATE_INDEX)) {
        return true;
    }
    return false;
}

/* Run from the dispatcher thread. */
void DispatchRedoRecordToFile(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;
    bool fatalerror = false;
    uint32 indexid = (uint32)-1;
    uint32 rmid = XLogRecGetRmid(record);
    uint32 term = XLogRecGetTerm(record);
    if (term > g_instance.comm_cxt.localinfo_cxt.term) {
        g_instance.comm_cxt.localinfo_cxt.term = term;
    }
    t_thrd.xlog_cxt.redoItemIdx = 0;
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        if (rmid <= RM_MAX_ID) {
            indexid = g_dispatchTable[rmid].rm_id;
            if ((indexid != rmid) ||
                ((g_dispatchTable[rmid].rm_loginfovalid != NULL) &&
                    (g_dispatchTable[rmid].rm_loginfovalid(
                         record, g_dispatchTable[rmid].rm_mininfo, g_dispatchTable[rmid].rm_maxinfo) == false))) {
                /* it's invalid info */
                fatalerror = true;
            }
        } else {
            fatalerror = true;
        }

        ResetChosedWorkerList();

        if (fatalerror != true) {
            isNeedFullSync = g_dispatchTable[rmid].rm_dispatch(record, expectedTLIs, recordXTime);
        } else {
            isNeedFullSync = DispatchDefaultRecord(record, expectedTLIs, recordXTime);
            isNeedFullSync = true;
        }

        if (isNeedFullSync)
            ProcessPendingRecords(true);
        else if (++g_dispatcher->pendingCount >= g_dispatcher->pendingMax)
            ProcessPendingRecords();

        if (fatalerror == true) {
            /* output panic error info */
            DumpDispatcher();
            ereport(PANIC,
                (errmodule(MOD_REDO),
                    errcode(ERRCODE_LOG),
                    errmsg("[REDO_LOG_TRACE]DispatchRedoRecord encounter fatal error:rmgrID:%u, info:%u, indexid:%u",
                        rmid,
                        (uint32)XLogRecGetInfo(record),
                        indexid)));
        }
    } else {
        ereport(PANIC,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]DispatchRedoRecord could not be here config recovery num %d, work num %u",
                    get_real_recovery_parallelism(),
                    GetPageWorkerCount())));
    }
}

/**
 * process record need sync with page worker and trxn thread
 * trxnthreadexe is true when the record need execute on trxn thread
 * pagethredexe is true when the record need execute on pageworker thread
 */
static void DispatchSyncTxnRecord(
    XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime, uint32 designatedWorker)
{
    RedoItem* item = CreateRedoItem(
        record, (g_dispatcher->chosedWorkerCount + 1), designatedWorker, expectedTLIs, recordXTime, true);

    item->sharewithtrxn = true;
    item->blockbytrxn = false;

    if ((g_dispatcher->chosedWorkerCount != 1) && (XLogRecGetRmid(&item->record) != RM_XACT_ID)) {
        ereport(WARNING,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]DispatchSyncTxnRecord maybe some error:rmgrID:%u, info:%u, workerCount:%u",
                    (uint32)XLogRecGetRmid(&item->record),
                    (uint32)XLogRecGetInfo(&item->record),
                    g_dispatcher->chosedWorkerCount)));
    }

    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; ++i) {
        if (g_dispatcher->chosedWorkerIds[i] > 0) {
            AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
        } else {
            RedoItem* lsnMarker = CreateLSNMarker(record, expectedTLIs, false);
            AddPageRedoItem(g_dispatcher->pageWorkers[i], lsnMarker);
        }
    }

    /* ensure eyery pageworker is receive recored to update pageworker Lsn
     * trxn record's recordtime must set , see SetLatestXTime
     */
    AddTxnRedoItem(g_dispatcher->txnWorker, item);
    return;
}

static void DispatchToOnePageWorker(XLogReaderState* record, const RelFileNode& rnode, List* expectedTLIs)
{
    /* for bcm different attr need to dispath to the same page redo thread */
    uint32 workerId = GetWorkerId(rnode, 0, 0);
    AddPageRedoItem(g_dispatcher->pageWorkers[workerId], CreateRedoItem(record, 1, ANY_WORKER, expectedTLIs, 0, true));
}

/**
* The transaction worker waits until every page worker has replayed
* all records before this.  We dispatch a LSN marker to every page
* worker so they can update their progress.
*
* We need to dispatch to page workers first, because the transaction
* worker runs in the dispatcher thread and may block wait on page
* workers.
* ensure eyery pageworker is receive recored to update pageworker Lsn
* trxn record's recordtime must set , see SetLatestXTime

*/
static void DispatchTxnRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime, bool imcheckpoint)
{
    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
        RedoItem* item = CreateLSNMarker(record, expectedTLIs, false);
        AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
    }

    RedoItem* trxnItem = CreateRedoItem(record, 1, ANY_WORKER, expectedTLIs, recordXTime, true);
    trxnItem->imcheckpoint = imcheckpoint; /* immdiate checkpoint set imcheckpoint  */
    AddTxnRedoItem(g_dispatcher->txnWorker, trxnItem);
}

#ifdef ENABLE_MULTIPLE_NODES
/* Run  from the dispatcher thread. */
static bool DispatchBarrierRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return false;
}
#endif

/* Run  from the dispatcher thread. */
static bool DispatchRepSlotRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return false;
}

/* Run  from the dispatcher thread. */
static bool DispatchHeap3Record(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return false;
}

/* record of rmid or info error, we inter this function to make every worker run to this position */
static bool DispatchDefaultRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return true;
}

/* Run from the dispatcher thread. */
static bool DispatchXLogRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));

    if (IsCheckPoint(record)) {
        RedoItem* item =
            CreateRedoItem(record, (g_dispatcher->pageWorkerCount + 1), ALL_WORKER, expectedTLIs, recordXTime, true);
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; ++i) {
            /*
             * A check point record may save a recovery restart point or
             * update the timeline.
             */
            AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
        }
        /* ensure eyery pageworker is receive recored to update pageworker Lsn
         * trxn record's recordtime must set , see SetLatestXTime
         */
        AddTxnRedoItem(g_dispatcher->txnWorker, item);

        isNeedFullSync = XLogWillChangeStandbyState(record);
    } else if ((info == XLOG_FPI) || (info == XLOG_FPI_FOR_HINT)) {
        if (SUPPORT_FPAGE_DISPATCH) {
            DispatchRecordWithPages(record, expectedTLIs, true);
        } else {
            DispatchRecordWithoutPage(record, expectedTLIs); /* fullpagewrite include btree, so need strong sync */
        }
    } else {
        /* process in trxn thread and need to sync to other pagerredo thread */
        DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    }
    return isNeedFullSync;
}

/* Run  from the dispatcher thread. */
static bool DispatchRelMapRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    /* page redo worker directly use relnode, will not use relmapfile */
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return false;
}

/* Run  from the dispatcher thread. */
static bool DispatchXactRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    if (XactWillRemoveRelFiles(record)) {
        /* for parallel performance */
        if (SUPPORT_FPAGE_DISPATCH) {
            int nrels = 0;
            ColFileNodeRel* xnodes = NULL;
            XactGetRelFiles(record, &xnodes, &nrels);
            for (int i = 0; ((i < nrels) && (xnodes != NULL)); ++i) {
                ColFileNode node;
                ColFileNodeRel* nodeRel = xnodes + i;
                ColFileNodeCopy(&node, nodeRel);
                uint32 id = GetWorkerId(node.filenode, 0, 0);
                AddWorkerToSet(id);
            }
        } else {
            for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
                AddWorkerToSet(i);
            }
        }
        /* sync with trxn thread */
        /* trx execute drop action, pageworker forger invalid page,
         * pageworker first exe and update lastcomplateLSN
         * then trx thread exe
         * first pageworker execute and update lsn, then trxn thread */
        DispatchSyncTxnRecord(record, expectedTLIs, recordXTime, ALL_WORKER);
    } else {
        /* process in trxn thread and need to sync to other pagerredo thread */
        DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    }

    return false;
}

/* Run from the dispatcher thread. */
static bool DispatchStandbyRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    /* change standbystate, must be full sync, see UpdateStandbyState */
    bool isNeedFullSync = StandbyWillChangeStandbyState(record);

    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);

    return isNeedFullSync;
}

/* Run from the dispatcher thread. */
static bool DispatchMultiXactRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    /* page worker will not use multixact */
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);

    return false;
}

/* Run from the dispatcher thread. */
static void DispatchRecordWithoutPage(XLogReaderState* record, List* expectedTLIs)
{
    RedoItem* item = CreateRedoItem(record, g_dispatcher->pageWorkerCount, ANY_WORKER, expectedTLIs, 0, true);
    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++)
        AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
}

/* Run from the dispatcher thread. */
static void DispatchRecordWithPages(XLogReaderState* record, List* expectedTLIs, bool rnodedispatch)
{
    GetWorkerIds(record, ANY_WORKER, rnodedispatch);

    RedoItem* item = CreateRedoItem(record, g_dispatcher->chosedWorkerCount, ANY_WORKER, expectedTLIs, 0, true);
    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
        if (g_dispatcher->chosedWorkerIds[i] > 0) {
            AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
        }
    }
}

static bool DispatchHeapRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    if (record->max_block_id >= 0)
        DispatchRecordWithPages(record, expectedTLIs, SUPPORT_FPAGE_DISPATCH);
    else
        DispatchRecordWithoutPage(record, expectedTLIs);

    return false;
}

static bool DispatchSeqRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchRecordWithPages(record, expectedTLIs, SUPPORT_FPAGE_DISPATCH);

    return false;
}

static bool DispatchDataBaseRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;

    if (IsDataBaseDrop(record)) {
        RedoItem* item =
            CreateRedoItem(record, (g_dispatcher->pageWorkerCount + 1), ALL_WORKER, expectedTLIs, recordXTime, true);
        item->imcheckpoint = true;
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
            AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
        }
        /* ensure eyery pageworker is receive recored to update pageworker Lsn
         * trxn record's recordtime must set , see SetLatestXTime
         */
        AddTxnRedoItem(g_dispatcher->txnWorker, item);
        isNeedFullSync = true;
    } else {
        /* database dir may impact many rel so need to sync to all pageworks */
        DispatchRecordWithoutPage(record, expectedTLIs);

        RedoItem* txnItem = CreateLSNMarker(record, expectedTLIs, false);
        /* ensure eyery pageworker is receive recored to update pageworker Lsn
         * recordtime not set ,  SetLatestXTime is not need to process
         */
        txnItem->imcheckpoint = true; /* immdiate checkpoint set true  */
        AddTxnRedoItem(g_dispatcher->txnWorker, txnItem);
    }

    return isNeedFullSync;
}

static bool DispatchTableSpaceRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));

    if (info == XLOG_TBLSPC_DROP) {
        DispatchTxnRecord(record, expectedTLIs, recordXTime, true);
        isNeedFullSync = true;
    } else {
        /* tablespace dir may impact many rel so need to sync to all pageworks */
        DispatchRecordWithoutPage(record, expectedTLIs);

        RedoItem* trxnItem = CreateLSNMarker(record, expectedTLIs, false);
        /* ensure eyery pageworker is receive recored to update pageworker Lsn
         * recordtime not set ,  SetLatestXTime is not need to process
         */
        trxnItem->imcheckpoint = true; /* immdiate checkpoint set true  */
        AddTxnRedoItem(g_dispatcher->txnWorker, trxnItem);
    }

    return isNeedFullSync;
}

static bool DispatchSmgrRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));
    if (info == XLOG_SMGR_CREATE) {
        /* only need to dispatch to one page worker */
        /* for parallel performance */
        if (SUPPORT_FPAGE_DISPATCH) {
            xl_smgr_create* xlrec = (xl_smgr_create*)XLogRecGetData(record);
            RelFileNode rnode;
            RelFileNodeCopy(rnode, xlrec->rnode, XLogRecGetBucketId(record));

            DispatchToOnePageWorker(record, rnode, expectedTLIs);
        } else {
            DispatchRecordWithoutPage(record, expectedTLIs);
        }
    } else if (IsSmgrTruncate(record)) {
        /*
         * SMGR_TRUNCATE acquires relation exclusive locks.
         * We need to force a full sync on it on stand by.
         *
         * Plus, it affects invalid pages bookkeeping.  We also need
         * to send it to all page workers.
         */
        /* for parallel performance */
        if (SUPPORT_FPAGE_DISPATCH) {
            uint32 id;
            xl_smgr_truncate* xlrec = (xl_smgr_truncate*)XLogRecGetData(record);
            RelFileNode rnode;
            RelFileNodeCopy(rnode, xlrec->rnode, XLogRecGetBucketId(record));
            id = GetWorkerId(rnode, 0, 0);
            AddWorkerToSet(id);
        } else {
            for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
                AddWorkerToSet(i);
            }
        }

        /* sync with trxn thread */
        /* trx truncate drop action, pageworker forger invalid page,
         * pageworker first exe and update lastcomplateLSN
         * then trx thread exe
         * first pageworker execute and update lsn, then trxn thread */
        DispatchSyncTxnRecord(record, expectedTLIs, recordXTime, ALL_WORKER);
    }

    return isNeedFullSync;
}

/* Run from the dispatcher thread. */
static bool DispatchCLogRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return false;
}

/* Run from the dispatcher thread. */
static bool DispatchHashRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return true;
}

static bool DispatchBtreeRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));
    if (info == XLOG_BTREE_REUSE_PAGE) {
        DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    } else {
        DispatchRecordWithPages(record, expectedTLIs, true);
    }
    return false;
}

/* Run from the dispatcher thread. */
static bool DispatchGinRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));

    if (info == XLOG_GIN_DELETE_LISTPAGE) {
        ginxlogDeleteListPages* data = (ginxlogDeleteListPages*)XLogRecGetData(record);
        /* output warning */
        if (data->ndeleted != record->max_block_id) {
            ereport(WARNING,
                (errmodule(MOD_REDO),
                    errcode(ERRCODE_LOG),
                    errmsg("[REDO_LOG_TRACE]DispatchGinRecord warnninginfo:ndeleted:%d, max_block_id:%d",
                        data->ndeleted,
                        record->max_block_id)));
        }
    }

    /* index not support mvcc, so we need to sync with trx thread when the record is vacuum */
    if (IsGinVacuumPages(record) && SUPPORT_HOT_STANDBY) {
        GetWorkerIds(record, ANY_WORKER, true);
        /* sync with trxn thread */
        /* only need to process in pageworker  thread, wait trxn sync */
        /* pageworker exe, trxn don't need exe */
        DispatchToSpecPageWorker(record, expectedTLIs, true);
    } else {
        DispatchRecordWithPages(record, expectedTLIs, true);
    }

    return isNeedFullSync;
}

/* Run from the dispatcher thread. */
static bool DispatchGistRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));
    bool isNeedFullSync = false;

    if (info == XLOG_GIST_PAGE_SPLIT) {
        gistxlogPageSplit* xldata = (gistxlogPageSplit*)XLogRecGetData(record);
        /* output warning */
        if (xldata->npage != record->max_block_id) {
            ereport(WARNING,
                (errmodule(MOD_REDO),
                    errcode(ERRCODE_LOG),
                    errmsg("[REDO_LOG_TRACE]DispatchGistRecord warnninginfo:npage:%hu, max_block_id:%d",
                        xldata->npage,
                        record->max_block_id)));
        }
    }

    /* index not support mvcc, so we need to sync with trx thread when the record is vacuum */
    if (IsGistPageUpdate(record) && SUPPORT_HOT_STANDBY) {
        GetWorkerIds(record, ANY_WORKER, true);
        /* sync with trx thread */
        /* only need to process in pageworker  thread, wait trxn sync */
        /* pageworker exe, trxn don't need exe */
        DispatchToSpecPageWorker(record, expectedTLIs, true);
    } else {
        DispatchRecordWithPages(record, expectedTLIs, true);
    }

    return isNeedFullSync;
}

/* Run from the dispatcher thread. */
static bool DispatchSpgistRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    /* index not support mvcc, so we need to sync with trx thread when the record is vacuum */
    if (IsSpgistVacuum(record) && SUPPORT_HOT_STANDBY) {
        uint8 info = (XLogRecGetInfo(record) & (~XLR_INFO_MASK));

        GetWorkerIds(record, ANY_WORKER, true);
        /* sync with trx thread */
        if ((info == XLOG_SPGIST_VACUUM_REDIRECT) && (InHotStandby)) {
            /* trxn thread first reslove confilict snapshot ,then do the page action */
            /* first pageworker update lsn, then trxn thread exe */
            DispatchSyncTxnRecord(record, expectedTLIs, recordXTime, TRXN_WORKER);
        } else {
            /* only need to process in pageworker  thread, wait trxn sync */
            /* pageworker exe, trxn don't need exe */
            DispatchToSpecPageWorker(record, expectedTLIs, true);
        }
    } else {
        DispatchRecordWithPages(record, expectedTLIs, true);
    }
    return false;
}

/**
 *  dispatch record to a specified thread
 */
static void DispatchToSpecPageWorker(XLogReaderState* record, List* expectedTLIs, bool waittrxnsync)
{
    RedoItem* item = CreateRedoItem(record, g_dispatcher->chosedWorkerCount, ANY_WORKER, expectedTLIs, 0, true);

    item->sharewithtrxn = false;
    item->blockbytrxn = waittrxnsync;

    if (g_dispatcher->chosedWorkerCount != 1) {
        ereport(WARNING,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]DispatchToSpecPageWorker maybe some error:rmgrID:%u, info:%u, workerCount:%u",
                    (uint32)XLogRecGetRmid(&item->record),
                    (uint32)XLogRecGetInfo(&item->record),
                    g_dispatcher->chosedWorkerCount)));
    }

    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
        if (g_dispatcher->chosedWorkerIds[i] > 0) {
            AddPageRedoItem(g_dispatcher->pageWorkers[i], item);
        } else {
            /* add LSN Marker to pageworker */
            RedoItem* lsnItem = CreateLSNMarker(record, expectedTLIs, false);
            AddPageRedoItem(g_dispatcher->pageWorkers[i], lsnItem);
        }
    }

    /* ensure eyery pageworker is receive recored to update pageworker Lsn
     * recordtime not set ,  SetLatestXTime is not need to process
     */
    AddTxnRedoItem(g_dispatcher->txnWorker, CreateLSNMarker(record, expectedTLIs, false));
}

static bool DispatchHeap2VacuumRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    /*
     * don't support consistency view
     */
    bool isNeedFullSync = false;
    uint8 info = ((XLogRecGetInfo(record) & (~XLR_INFO_MASK)) & XLOG_HEAP_OPMASK);
    if (info == XLOG_HEAP2_CLEANUP_INFO) {
        DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    } else {
        DispatchRecordWithPages(record, expectedTLIs, SUPPORT_FPAGE_DISPATCH);
    }

    return isNeedFullSync;
}

/* Run from the dispatcher thread. */
static bool DispatchHeap2Record(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    bool isNeedFullSync = false;

    uint8 info = ((XLogRecGetInfo(record) & (~XLR_INFO_MASK)) & XLOG_HEAP_OPMASK);
    if ((info == XLOG_HEAP2_MULTI_INSERT) || (info == XLOG_HEAP2_PAGE_UPGRADE)) {
        DispatchRecordWithPages(record, expectedTLIs, SUPPORT_FPAGE_DISPATCH);
    } else if (info == XLOG_HEAP2_BCM) {
        /* we use renode as dispatch key, so the same relation will dispath to the same page redo thread
         * although they have different fork num
         */
        /* for parallel redo performance */
        if (SUPPORT_FPAGE_DISPATCH) {
            xl_heap_bcm* xlrec = (xl_heap_bcm*)XLogRecGetData(record);

            RelFileNode tmp_node;
            RelFileNodeCopy(tmp_node, xlrec->node, XLogRecGetBucketId(record));

            DispatchToOnePageWorker(record, tmp_node, expectedTLIs);
        } else {
            DispatchRecordWithoutPage(record, expectedTLIs);
        }
    } else if (info == XLOG_HEAP2_LOGICAL_NEWPAGE) {
        if (IS_DN_MULTI_STANDYS_MODE()) {
            xl_heap_logical_newpage* xlrec = (xl_heap_logical_newpage*)XLogRecGetData(record);

            if (xlrec->type == COLUMN_STORE && xlrec->hasdata) {
                /* for parallel redo performance */
                if (SUPPORT_FPAGE_DISPATCH) {
                    RelFileNode tmp_node;
                    RelFileNodeCopy(tmp_node, xlrec->node, XLogRecGetBucketId(record));

                    DispatchToOnePageWorker(record, tmp_node, expectedTLIs);
                } else {
                    DispatchRecordWithoutPage(record, expectedTLIs);
                }
            }
        } else {
            if (!g_instance.attr.attr_storage.enable_mix_replication) {
                DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
                isNeedFullSync = true;
            }
        }
    } else {
        isNeedFullSync = DispatchHeap2VacuumRecord(record, expectedTLIs, recordXTime);
    }

    return isNeedFullSync;
}

/* Run from the dispatcher thread. */
static bool DispatchMotRecord(XLogReaderState* record, List* expectedTLIs, TimestampTz recordXTime)
{
    DispatchTxnRecord(record, expectedTLIs, recordXTime, false);
    return false;
}

/* Run from the dispatcher thread. */
static void GetWorkerIds(XLogReaderState* record, uint32 designatedWorker, bool rnodedispatch)
{
    uint32 id;
    for (int i = 0; i <= record->max_block_id; i++) {
        DecodedBkpBlock* block = &record->blocks[i];

        if (block->in_use != true) {
            /* blk number is not continue */
            continue;
        }
        if (rnodedispatch)
            id = GetWorkerId(block->rnode, 0, 0);
        else
            id = GetWorkerId(block->rnode, block->blkno, 0);

        AddWorkerToSet(id);
    }

    if ((designatedWorker != ANY_WORKER)) {
        if (designatedWorker < GetPageWorkerCount()) {
            AddWorkerToSet(designatedWorker);
        } else {
            /* output  error info */
        }
    }
}

/**
 * count worker id  by hash
 */
uint32 GetWorkerId(const RelFileNode& node, BlockNumber block, ForkNumber forkNum)
{
    uint32 workerCount = GetPageWorkerCount();
    if (workerCount == 0)
        return ANY_WORKER;

    BufferTag tag;
    INIT_BUFFERTAG(tag, node, forkNum, block);
    return tag_hash(&tag, sizeof(tag)) % workerCount;
}

static void AddWorkerToSet(uint32 id)
{
    if (id >= g_dispatcher->pageWorkerCount) {
        ereport(PANIC,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]AddWorkerToSet:input work id error, id:%u, work num %u",
                    id,
                    g_dispatcher->pageWorkerCount)));
        return;
    }

    if (g_dispatcher->chosedWorkerIds[id] == 0) {
        g_dispatcher->chosedWorkerCount += 1;
    }
    ++(g_dispatcher->chosedWorkerIds[id]);
}

/* Run from the dispatcher and each page worker thread. */
bool XactWillRemoveRelFiles(XLogReaderState* record)
{
    /*
     * Relation files under tablespace folders are removed only from
     * applying transaction log record.
     */
    int nrels = 0;
    ColFileNodeRel* xnodes = NULL;

    if (XLogRecGetRmid(record) != RM_XACT_ID) {
        return false;
    }

    XactGetRelFiles(record, &xnodes, &nrels);

    return (nrels > 0);
}

/* Run from the dispatcher thread. */
static bool XLogWillChangeStandbyState(XLogReaderState* record)
{
    /*
     * If standbyState has reached SNAPSHOT_READY, it will not change
     * anymore.  Otherwise, it will change if the log record's redo
     * function calls ProcArrayApplyRecoveryInfo().
     */
    if ((t_thrd.xlog_cxt.standbyState < STANDBY_INITIALIZED) ||
        (t_thrd.xlog_cxt.standbyState == STANDBY_SNAPSHOT_READY))
        return false;

    if ((XLogRecGetRmid(record) == RM_XLOG_ID) &&
        ((XLogRecGetInfo(record) & (~XLR_INFO_MASK)) == XLOG_CHECKPOINT_SHUTDOWN)) {
        return true;
    }

    return false;
}

/* Run from the dispatcher thread. */
static bool StandbyWillChangeStandbyState(XLogReaderState* record)
{
    /*
     * If standbyState has reached SNAPSHOT_READY, it will not change
     * anymore.  Otherwise, it will change if the log record's redo
     * function calls ProcArrayApplyRecoveryInfo().
     */
    if ((t_thrd.xlog_cxt.standbyState < STANDBY_SNAPSHOT_READY) && (XLogRecGetRmid(record) == RM_STANDBY_ID) &&
        ((XLogRecGetInfo(record) & (~XLR_INFO_MASK)) == XLOG_RUNNING_XACTS)) {
        /* change standbystate, must be full sync, see UpdateStandbyState */
        return true;
    }

    return false;
}

/* Run from the dispatcher thread. */
/* fullSync: true. wait for other workers, transaction need it */
/*        : false not wait for other workers  */
void ProcessPendingRecords(bool fullSync)
{
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
            uint64 blockcnt = 0;
            pgstat_report_waitevent(WAIT_EVENT_PREDO_PROCESS_PENDING);
            while (!ProcessPendingPageRedoItems(g_dispatcher->pageWorkers[i])) {
                blockcnt++;
                ApplyReadyTxnLogRecords(g_dispatcher->txnWorker, false);
                if ((blockcnt & OUTPUT_WAIT_COUNT) == OUTPUT_WAIT_COUNT) {
                    ereport(LOG,
                        (errmodule(MOD_REDO),
                            errcode(ERRCODE_LOG),
                            errmsg("[REDO_LOG_TRACE]ProcessPendingRecords:replayedLsn:%lu, blockcnt:%lu, "
                                   "WorkerCount:%u, readEndLSN:%lu",
                                GetXLogReplayRecPtr(NULL, NULL),
                                blockcnt,
                                g_dispatcher->pageWorkerCount,
                                t_thrd.xlog_cxt.EndRecPtr)));
                    if ((blockcnt & PRINT_ALL_WAIT_COUNT) == PRINT_ALL_WAIT_COUNT) {
                        DumpDispatcher();
                    }
                }
                HandleStartupProcInterrupts();
            }
            pgstat_report_waitevent(WAIT_EVENT_END);
        }
        MoveTxnItemToApplyQueue(g_dispatcher->txnWorker);
        ApplyReadyTxnLogRecords(g_dispatcher->txnWorker, fullSync);
        g_dispatcher->pendingCount = 0;
    }
}

/* Run from the dispatcher thread. */
/* fullSync: true. wait for other workers, transaction need it */
/*        : false not wait for other workers  */
void ProcessTrxnRecords(bool fullSync)
{
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        ApplyReadyTxnLogRecords(g_dispatcher->txnWorker, fullSync);

        if (fullSync && (IsTxnWorkerIdle(g_dispatcher->txnWorker))) {
            /* notify pageworker sleep long time */
            SendSingalToPageWorker(SIGUSR2);
        }
    }
}

/* Run from each page worker thread. */
void FreeRedoItem(RedoItem* item)
{
    RedoItem* oldHead = (RedoItem*)pg_atomic_read_uintptr((uintptr_t*)&g_dispatcher->freeHead);

    do {
        item->freeNext = oldHead;
    } while (!pg_atomic_compare_exchange_uintptr(
        (uintptr_t*)&g_dispatcher->freeHead, (uintptr_t*)&oldHead, (uintptr_t)item));
}

void InitReaderStateByOld(XLogReaderState* newState, XLogReaderState* oldState, bool isNew)
{
    if (isNew) {
        *newState = *oldState;
        newState->main_data = NULL;
        newState->main_data_len = 0;
        newState->main_data_bufsz = 0;

        for (int i = 0; i <= XLR_MAX_BLOCK_ID; i++) {
            newState->blocks[i].data = NULL;
            newState->blocks[i].data_len = 0;
            newState->blocks[i].data_bufsz = 0;
        }
        newState->readRecordBuf = NULL;
        newState->readRecordBufSize = 0;
    } else {
        char* mData = newState->main_data;
        uint32 mDSize = newState->main_data_bufsz;
        char* bData[XLR_MAX_BLOCK_ID + 1];
        uint32 bDSize[XLR_MAX_BLOCK_ID + 1];
        for (int i = 0; i <= XLR_MAX_BLOCK_ID; i++) {
            bData[i] = newState->blocks[i].data;
            bDSize[i] = newState->blocks[i].data_bufsz;
        }
        char* rrBuf = newState->readRecordBuf;
        uint32 rrBufSize = newState->readRecordBufSize;
        /* copy state */
        *newState = *oldState;
        /* restore mem buffer */
        newState->main_data = mData;
        newState->main_data_len = 0;
        newState->main_data_bufsz = mDSize;
        for (int i = 0; i <= XLR_MAX_BLOCK_ID; i++) {
            newState->blocks[i].data = bData[i];
            newState->blocks[i].data_len = 0;
            newState->blocks[i].data_bufsz = bDSize[i];
        }
        newState->readRecordBuf = rrBuf;
        newState->readRecordBufSize = rrBufSize;
    }
}

static XLogReaderState* GetXlogReader(XLogReaderState* readerState)
{
    XLogReaderState* retReaderState = NULL;
    bool isNew = false;
    uint64 count = 0;

    do {
        if (g_dispatcher->freeStateHead != NULL) {
            retReaderState = &g_dispatcher->freeStateHead->record;
            g_dispatcher->freeStateHead = g_dispatcher->freeStateHead->freeNext;
        } else {
            RedoItem* head =
                (RedoItem*)pg_atomic_exchange_uintptr((uintptr_t*)&g_dispatcher->freeHead, (uintptr_t)NULL);
            if (head != NULL) {
                retReaderState = &head->record;
                g_dispatcher->freeStateHead = head->freeNext;
            } else if (g_dispatcher->maxItemNum > g_dispatcher->curItemNum) {
                RedoItem* item = (RedoItem*)palloc_extended(
                    MAXALIGN(sizeof(RedoItem)) + sizeof(RedoItem*) * (GetPageWorkerCount() + 1),
                    MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
                if (item != NULL) {
                    retReaderState = &item->record;
                    item->allocatedNext = g_dispatcher->allocatedRedoItem;
                    g_dispatcher->allocatedRedoItem = item;
                    isNew = true;
                    ++(g_dispatcher->curItemNum);
                }
            }

            ++count;
            if ((count & OUTPUT_WAIT_COUNT) == OUTPUT_WAIT_COUNT) {
                ereport(WARNING,
                    (errmodule(MOD_REDO),
                        errcode(ERRCODE_LOG),
                        errmsg("GetXlogReader Allocated record buffer failed!, cur item:%u, max item:%u",
                            g_dispatcher->curItemNum,
                            g_dispatcher->maxItemNum)));
                if ((count & PRINT_ALL_WAIT_COUNT) == PRINT_ALL_WAIT_COUNT) {
                    DumpDispatcher();
                }
            }
            if (retReaderState == NULL) {
                ProcessTrxnRecords(false);
                HandleStartupProcInterrupts();
            }
        }
    } while (retReaderState == NULL);

    InitReaderStateByOld(retReaderState, readerState, isNew);

    return retReaderState;
}

void CopyDataFromOldReader(XLogReaderState* newReaderState, const XLogReaderState* oldReaderState)
{
    errno_t rc = EOK;
    if ((newReaderState->readRecordBuf == NULL) ||
        (oldReaderState->readRecordBufSize > newReaderState->readRecordBufSize)) {
        if (!allocate_recordbuf(newReaderState, oldReaderState->readRecordBufSize)) {
            ereport(PANIC,
                (errmodule(MOD_REDO),
                    errcode(ERRCODE_LOG),
                    errmsg("Allocated record buffer failed!, cur item:%u, max item:%u",
                        g_dispatcher->curItemNum,
                        g_dispatcher->maxItemNum)));
        }
    }

    rc = memcpy_s(newReaderState->readRecordBuf,
        newReaderState->readRecordBufSize,
        oldReaderState->readRecordBuf,
        oldReaderState->readRecordBufSize);
    securec_check(rc, "\0", "\0");
    newReaderState->decoded_record = (XLogRecord*)newReaderState->readRecordBuf;

    for (int i = 0; i <= newReaderState->max_block_id; i++) {
        if (newReaderState->blocks[i].has_image)
            newReaderState->blocks[i].bkp_image =
                (char*)((uintptr_t)newReaderState->decoded_record +
                        ((uintptr_t)oldReaderState->blocks[i].bkp_image - (uintptr_t)oldReaderState->decoded_record));
        if (newReaderState->blocks[i].has_data) {
            newReaderState->blocks[i].data = oldReaderState->blocks[i].data;
            newReaderState->blocks[i].data_len = oldReaderState->blocks[i].data_len;
        }
    }
    if (oldReaderState->main_data_len > 0) {

        newReaderState->main_data =
            (char*)((uintptr_t)newReaderState->decoded_record +
                    ((uintptr_t)oldReaderState->main_data - (uintptr_t)oldReaderState->decoded_record));
        newReaderState->main_data_len = oldReaderState->main_data_len;
    }
}

XLogReaderState* NewReaderState(XLogReaderState* readerState, bool bCopyState)
{
    Assert(readerState != NULL);
    if (!readerState->isPRProcess)
        return readerState;
    if (DispatchPtrIsNull())
        ereport(PANIC, (errmodule(MOD_REDO), errcode(ERRCODE_LOG), errmsg("NewReaderState Dispatch is null")));

    XLogReaderState* retReaderState = GetXlogReader(readerState);

    if (bCopyState) {
        CopyDataFromOldReader(retReaderState, readerState);
    }
    return retReaderState;
}

void FreeAllocatedRedoItem()
{

    while ((g_dispatcher != NULL) && (g_dispatcher->allocatedRedoItem != NULL)) {
        RedoItem* pItem = g_dispatcher->allocatedRedoItem;
        g_dispatcher->allocatedRedoItem = pItem->allocatedNext;
        XLogReaderState* tmpRec = &(pItem->record);

        if (tmpRec->readRecordBuf) {
            pfree(tmpRec->readRecordBuf);
            tmpRec->readRecordBuf = NULL;
        }

        pfree(pItem);
    }
}

/* Run from the dispatcher thread. */
void SendRecoveryEndMarkToWorkersAndWaitForFinish(int code)
{
    ereport(LOG,
        (errmodule(MOD_REDO),
            errcode(ERRCODE_LOG),
            errmsg("[REDO_LOG_TRACE]SendRecoveryEndMarkToWorkersAndWaitForFinish, ready to stop redo workers, code: %d",
                code)));
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        pg_atomic_write_u32((uint32*)&g_dispatcher->exitCode, (uint32)code);
        ApplyReadyTxnLogRecords(g_dispatcher->txnWorker, true);
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
            uint64 blockcnt = 0;
            while (!SendPageRedoEndMark(g_dispatcher->pageWorkers[i])) {
                blockcnt++;
                ApplyReadyTxnLogRecords(g_dispatcher->txnWorker, false);
                if ((blockcnt & OUTPUT_WAIT_COUNT) == OUTPUT_WAIT_COUNT) {
                    ereport(WARNING,
                        (errmodule(MOD_REDO),
                            errcode(ERRCODE_LOG),
                            errmsg("[REDO_LOG_TRACE]RecoveryEndMark:replayedLsn:%lu, blockcnt:%lu, WorkerCount:%u",
                                GetXLogReplayRecPtr(NULL, NULL),
                                blockcnt,
                                g_dispatcher->pageWorkerCount)));
                }
            }
        }

        ApplyReadyTxnLogRecords(g_dispatcher->txnWorker, true);
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++)
            WaitPageRedoWorkerReachLastMark(g_dispatcher->pageWorkers[i]);
        SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.rwlock));
        g_instance.comm_cxt.predo_cxt.state = REDO_DONE;
        SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.rwlock));
        ereport(LOG,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_LOG_TRACE]SendRecoveryEndMarkToWorkersAndWaitForFinish, disptach total elapsed: %lu,"
                       " txn elapsed: %lu, process pending record elapsed: %lu code: %d",
                    g_dispatcher->totalCostTime,
                    g_dispatcher->txnCostTime,
                    g_dispatcher->pprCostTime,
                    code)));
    }
}

/* Run from each page worker and the txn worker thread. */
int GetDispatcherExitCode()
{
    return (int)pg_atomic_read_u32((uint32*)&g_dispatcher->exitCode);
}

/* Run from the dispatcher thread. */
uint32 GetPageWorkerCount()
{
    return g_dispatcher == NULL ? 0 : g_dispatcher->pageWorkerCount;
}

bool DispatchPtrIsNull()
{
    return (g_dispatcher == NULL);
}

/* Run from each page worker thread. */
PGPROC* StartupPidGetProc(ThreadId pid)
{
    if (pid == g_instance.proc_base->startupProcPid)
        return g_instance.proc_base->startupProc;
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
            PGPROC* proc = GetPageRedoWorkerProc(g_dispatcher->pageWorkers[i]);
            if (pid == proc->pid)
                return proc;
        }
    }
    return NULL;
}

/*
 * Used from bufgr to share the value of the buffer that Startup waits on,
 * or to reset the value to "not waiting" (-1). This allows processing
 * of recovery conflicts for buffer pins. Set is made before backends look
 * at this value, so locking not required, especially since the set is
 * an atomic integer set operation.
 */
void SetStartupBufferPinWaitBufId(int bufid)
{
    if (g_instance.proc_base->startupProcPid == t_thrd.proc->pid) {
        g_instance.proc_base->startupBufferPinWaitBufId = bufid;
    }
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
            PGPROC* proc = GetPageRedoWorkerProc(g_dispatcher->pageWorkers[i]);
            if (t_thrd.proc->pid == proc->pid) {
                g_dispatcher->pageWorkers[i]->bufferPinWaitBufId = bufid;
                break;
            }
        }
    }
}

uint32 GetStartupBufferPinWaitBufLen()
{
    uint32 buf_len = 1;
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        buf_len += g_dispatcher->pageWorkerCount;
    }
    return buf_len;
}

/*
 * Used by backends when they receive a request to check for buffer pin waits.
 */
void GetStartupBufferPinWaitBufId(int* bufids, uint32 len)
{
    for (uint32 i = 0; i < len - 1; i++) {
        bufids[i] = g_dispatcher->pageWorkers[i]->bufferPinWaitBufId;
    }
    bufids[len - 1] = g_instance.proc_base->startupBufferPinWaitBufId;
}

void GetReplayedRecPtrFromWorkers(XLogRecPtr* readPtr, XLogRecPtr* endPtr)
{
    XLogRecPtr minRead = MAX_XLOG_REC_PTR;
    XLogRecPtr minEnd = MAX_XLOG_REC_PTR;

    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
        if (!RedoWorkerIsIdle(g_dispatcher->pageWorkers[i])) {
            XLogRecPtr read;
            XLogRecPtr end;
            GetCompletedReadEndPtr(g_dispatcher->pageWorkers[i], &read, &end);
            if (XLByteLT(end, minEnd)) {
                minEnd = end;
                minRead = read;
            }
        }
    }

    *readPtr = minRead;
    *endPtr = minEnd;
}

/* Run from the txn worker thread. */
bool IsRecoveryRestartPointSafeForWorkers(XLogRecPtr restartPoint)
{
    bool safe = true;
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++)
            if (!IsRecoveryRestartPointSafe(g_dispatcher->pageWorkers[i], restartPoint)) {
                ereport(LOG,
                    (errmodule(MOD_REDO),
                        errcode(ERRCODE_LOG),
                        errmsg("[REDO_LOG_TRACE]IsRecoveryRestartPointSafeForWorkers: workerId:%u, restartPoint:%lu",
                            i,
                            restartPoint)));
                safe = false;
            }
        if (safe) {
            for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
                SetWorkerRestartPoint(g_dispatcher->pageWorkers[i], restartPoint);
            }
        }
    }

    return safe;
}

/* Run from the dispatcher and txn worker thread. */
void UpdateStandbyState(HotStandbyState newState)
{
    if ((get_real_recovery_parallelism() > 1) && (GetPageWorkerCount() > 0)) {
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++)
            UpdatePageRedoWorkerStandbyState(g_dispatcher->pageWorkers[i], newState);
    }
}

/* Run from the dispatcher thread. */
void** GetXLogInvalidPagesFromWorkers()
{
    return CollectStatesFromWorkers(GetXLogInvalidPages);
}

/* Run from the dispatcher thread. */
static void** CollectStatesFromWorkers(GetStateFunc getStateFunc)
{
    if (g_dispatcher->pageWorkerCount > 0) {
        void** stateArray = (void**)palloc(sizeof(void*) * g_dispatcher->pageWorkerCount);
        for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++)
            stateArray[i] = getStateFunc(g_dispatcher->pageWorkers[i]);
        return stateArray;
    } else
        return NULL;
}

void DiagLogRedoRecord(XLogReaderState* record, const char* funcName)
{
    uint8 info;
    RelFileNode oldRn = {0};
    RelFileNode newRn = {0};
    BlockNumber oldblk = InvalidBlockNumber;
    BlockNumber newblk = InvalidBlockNumber;
    bool newBlkExistFlg = false;
    bool oldBlkExistFlg = false;
    ForkNumber oldFk = InvalidForkNumber;
    ForkNumber newFk = InvalidForkNumber;
    StringInfoData buf;

    /* Support  redo old version xlog during upgrade (Just the runningxact log with chekpoint online ) */
    uint32 rmid = redo_oldversion_xlog ? ((XLogRecordOld*)record->decoded_record)->xl_rmid : XLogRecGetRmid(record);
    info = redo_oldversion_xlog ? ((((XLogRecordOld*)record->decoded_record)->xl_info) & ~XLR_INFO_MASK)
                                : (XLogRecGetInfo(record) & ~XLR_INFO_MASK);

    initStringInfo(&buf);
    RmgrTable[rmid].rm_desc(&buf, record);

    if (XLogRecGetBlockTag(record, 0, &newRn, &newFk, &newblk)) {
        newBlkExistFlg = true;
    }
    if (XLogRecGetBlockTag(record, 1, &oldRn, &oldFk, &oldblk)) {
        oldBlkExistFlg = true;
    }
    ereport(DEBUG4,
        (errmodule(MOD_REDO),
            errcode(ERRCODE_LOG),
            errmsg("[REDO_LOG_TRACE]DiagLogRedoRecord: %s, ReadRecPtr:%lu,EndRecPtr:%lu,"
                   "newBlkExistFlg:%d,"
                   "newRn(spcNode:%u, dbNode:%u, relNode:%u),newFk:%d,newblk:%u,"
                   "oldBlkExistFlg:%d,"
                   "oldRn(spcNode:%u, dbNode:%u, relNode:%u),oldFk:%d,oldblk:%u,"
                   "info:%u,redo_oldversion_xlog:%d, rm_name:%s, desc:%s,"
                   "max_block_id:%d",
                funcName,
                record->ReadRecPtr,
                record->EndRecPtr,
                newBlkExistFlg,
                newRn.spcNode,
                newRn.dbNode,
                newRn.relNode,
                newFk,
                newblk,
                oldBlkExistFlg,
                oldRn.spcNode,
                oldRn.dbNode,
                oldRn.relNode,
                oldFk,
                oldblk,
                (uint32)info,
                redo_oldversion_xlog,
                RmgrTable[rmid].rm_name,
                buf.data,
                record->max_block_id)));
    pfree_ext(buf.data);
}

void redo_get_wroker_statistic(uint32* realNum, RedoWorkerStatsData* worker, uint32 workerLen)
{
    PageRedoWorker* redoWorker = NULL;
    Assert(workerLen == MAX_RECOVERY_THREAD_NUM);
    SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
    if (g_dispatcher == NULL) {
        SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
        *realNum = 0;
        return;
    }
    *realNum = g_dispatcher->pageWorkerCount;
    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
        redoWorker = (g_dispatcher->pageWorkers[i]);
        worker[i].id = redoWorker->id;
        worker[i].queue_usage = SPSCGetQueueCount(redoWorker->queue);
        worker[i].queue_max_usage = (uint32)(pg_atomic_read_u32(&((redoWorker->queue)->maxUsage)));
        worker[i].redo_rec_count = (uint32)(pg_atomic_read_u64(&((redoWorker->queue)->totalCnt)));
    }
    SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
}

RedoWaitInfo redo_get_io_event(int32 event_id)
{
    WaitStatisticsInfo tmp_io = {};
    RedoWaitInfo result_info = {};
    PgBackendStatus* beentry = NULL;
    int index = MAX_BACKEND_SLOT + StartupProcess;

    if (IS_PGSTATE_TRACK_UNDEFINE || t_thrd.shemem_ptr_cxt.BackendStatusArray == NULL) {
        return result_info;
    }

    beentry = t_thrd.shemem_ptr_cxt.BackendStatusArray + index;
    tmp_io = beentry->waitInfo.event_info.io_info[event_id - WAIT_EVENT_BUFFILE_READ];
    result_info.total_duration = tmp_io.total_duration;
    result_info.counter = tmp_io.counter;
    SpinLockAcquire(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
    if (g_dispatcher == NULL || event_id == WAIT_EVENT_WAL_READ || event_id == WAIT_EVENT_PREDO_PROCESS_PENDING) {
        SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
        return result_info;
    }

    for (uint32 i = 0; i < g_dispatcher->pageWorkerCount; i++) {
        index = g_dispatcher->pageWorkers[i]->index;
        beentry = t_thrd.shemem_ptr_cxt.BackendStatusArray + index;
        tmp_io = beentry->waitInfo.event_info.io_info[event_id - WAIT_EVENT_BUFFILE_READ];
        result_info.total_duration += tmp_io.total_duration;
        result_info.counter += tmp_io.counter;
    }
    SpinLockRelease(&(g_instance.comm_cxt.predo_cxt.destroy_lock));
    return result_info;
}

void redo_dump_all_stats()
{
    RedoPerf* redo = &(g_instance.comm_cxt.predo_cxt.redoPf);
    uint64 redoBytes = redo->read_ptr - redo->redo_start_ptr;
    int64 curr_time = GetCurrentTimestamp();
    uint64 totalTime = curr_time - redo->redo_start_time;
    uint64 speed = 0; /* KB/s */
    if (totalTime > 0) {
        speed = (redoBytes / totalTime) * US_TRANSFER_TO_S / BYTES_TRANSFER_KBYTES;
    }
    ereport(LOG,
        (errmodule(MOD_REDO),
            errcode(ERRCODE_LOG),
            errmsg("[REDO_STATS]redo_dump_all_stats: the basic statistic during redo are as follows : "
                   "redo_start_ptr:%lu, redo_start_time:%ld, min_recovery_point:%lu, "
                   "read_ptr:%lu, last_replayed_read_Ptr:%lu, speed:%lu KB/s",
                redo->redo_start_ptr,
                redo->redo_start_time,
                redo->min_recovery_point,
                redo->read_ptr,
                redo->last_replayed_read_ptr,
                speed)));

    uint32 type;
    RedoWaitInfo tmp_info;
    for (type = 0; type < WAIT_REDO_NUM; type++) {
        tmp_info = redo_get_io_event(redo_get_event_type_by_wait_type(type));
        ereport(LOG,
            (errmodule(MOD_REDO),
                errcode(ERRCODE_LOG),
                errmsg("[REDO_STATS]redo_dump_all_stats %s: the event io statistic during redo are as follows : "
                       "total_duration:%ld, counter:%ld",
                    redo_get_name_by_wait_type(type),
                    tmp_info.total_duration,
                    tmp_info.counter)));
    }

    if (g_dispatcher != NULL) {
        redo_dump_worker_queue_info();
    }
}

}  // namespace parallel_recovery
