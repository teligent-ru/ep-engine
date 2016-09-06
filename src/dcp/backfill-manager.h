/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * The BackfillManager is responsible for multiple DCP backfill
 * operations owned by a single DCP connection.
 * It consists of two main classes:
 *
 * - BackfillManager, which acts as the main interface for adding new
 *    streams.
 * - BackfillManagerTask, which runs on a background AUXIO thread and
 *    performs most of the actual backfilling operations.
 *
 * One main purpose of the BackfillManager is to impose a limit on the
 * in-memory buffer space a streams' backfills consume - often
 * ep-engine can read data from disk faster than the client connection
 * can consume it, and so without any limits we could exhaust the
 * bucket quota and cause Items to be evicted from the HashTable,
 * which is Bad. At a high level, these limits are based on giving
 * each DCP connection a maximum amount of buffer space, and pausing
 * backfills if the buffer limit is reached. When the buffers are
 * sufficiently drained (by sending to the client), backfilling can be
 * resumed.
 *
 * Significant configuration parameters affecting backfill:
 * - dcp_scan_byte_limit
 * - dcp_scan_item_limit
 * - dcp_backfill_byte_limit
 */

#ifndef SRC_DCP_BACKFILL_MANAGER_H_
#define SRC_DCP_BACKFILL_MANAGER_H_ 1

#include "config.h"
#include "connmap.h"
#include "dcp/backfill.h"
#include "dcp/producer.h"
#include "dcp/stream.h"

class EventuallyPersistentEngine;

class BackfillManager : public std::enable_shared_from_this<BackfillManager> {
public:
    BackfillManager(EventuallyPersistentEngine* e);

    ~BackfillManager();

    void addStats(connection_t conn, ADD_STAT add_stat, const void *c);

    void schedule(stream_t stream, uint64_t start, uint64_t end);

    bool bytesRead(uint32_t bytes);

    void bytesSent(uint32_t bytes);

    // Called by the managerTask to acutally perform backfilling & manage
    // backfills between the different queues.
    backfill_status_t backfill();

    void wakeUpTask();

private:

    void moveToActiveQueue();

    std::mutex lock;
    std::list<DCPBackfill*> activeBackfills;
    std::list<std::pair<rel_time_t, DCPBackfill*> > snoozingBackfills;
    //! When the number of (activeBackfills + snoozingBackfills) crosses a
    //!   threshold we use waitingBackfills
    std::list<DCPBackfill*> pendingBackfills;
    EventuallyPersistentEngine* engine;
    ExTask managerTask;

    //! The scan buffer is for the current stream being backfilled
    struct {
        uint32_t bytesRead;
        uint32_t itemsRead;
        uint32_t maxBytes;
        uint32_t maxItems;
    } scanBuffer;

    //! The buffer is the total bytes used by all backfills for this connection
    struct {
        uint32_t bytesRead;
        uint32_t maxBytes;
        uint32_t nextReadSize;
        bool full;
    } buffer;
};

#endif  // SRC_DCP_BACKFILL_MANAGER_H_
