/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc
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

#include "config.h"

#include "checkpoint_remover.h"
#include "ep.h"
#include "ep_engine.h"
#include "vbucket.h"
#include "connmap.h"

/**
 * Remove all the closed unreferenced checkpoints for each vbucket.
 */
class CheckpointVisitor : public VBucketVisitor {
public:

    /**
     * Construct a CheckpointVisitor.
     */
    CheckpointVisitor(EventuallyPersistentStore *s, EPStats &st,
                      AtomicValue<bool> &sfin)
        : store(s), stats(st), removed(0), taskStart(gethrtime()),
          wasHighMemoryUsage(s->isMemoryUsageTooHigh()), stateFinalizer(sfin) {}

    bool visitBucket(RCPtr<VBucket> &vb) {
        currentBucket = vb;
        bool newCheckpointCreated = false;
        removed = vb->checkpointManager.removeClosedUnrefCheckpoints(vb,
                                                         newCheckpointCreated);
        // If the new checkpoint is created, notify this event to the
        // corresponding paused TAP & DCP connections.
        if (newCheckpointCreated) {
            store->getEPEngine().getTapConnMap().notifyVBConnections(
                                                                  vb->getId());
            store->getEPEngine().getDcpConnMap().notifyVBConnections(
                                        vb->getId(),
                                        vb->checkpointManager.getHighSeqno());
        }
        update();
        return false;
    }

    void update() {
        stats.itemsRemovedFromCheckpoints.fetch_add(removed);
        if (removed > 0) {
            LOG(EXTENSION_LOG_INFO,
                "Removed %ld closed unreferenced checkpoints from VBucket %d",
                removed, currentBucket->getId());
        }
        removed = 0;
    }

    void complete() {
        bool inverse = false;
        stateFinalizer.compare_exchange_strong(inverse, true);

        stats.checkpointRemoverHisto.add((gethrtime() - taskStart) / 1000);

        // Wake up any sleeping backfill tasks if the memory usage is lowered
        // below the high watermark as a result of checkpoint removal.
        if (wasHighMemoryUsage && !store->isMemoryUsageTooHigh()) {
            store->getEPEngine().getDcpConnMap().notifyBackfillManagerTasks();
        }
    }

private:
    EventuallyPersistentStore *store;
    EPStats                   &stats;
    size_t                     removed;
    hrtime_t                   taskStart;
    bool                       wasHighMemoryUsage;
    AtomicValue<bool>         &stateFinalizer;
};

void ClosedUnrefCheckpointRemoverTask::cursorDroppingIfNeeded(void) {
    /**
     * Cursor dropping will commence only if the total memory used is
     * greater than the upper threshold which is a percentage of the
     * quota, specified by cursor_dropping_upper_mark. Once cursor
     * dropping starts, it will continue until memory usage is projected
     * to go under the lower threshold which is a percentage of the quota,
     * specified by cursor_dropping_lower_mark.
     */
    if (stats.getTotalMemoryUsed() > stats.cursorDroppingUThreshold.load()) {
        size_t amountOfMemoryToClear = stats.getTotalMemoryUsed() -
                                          stats.cursorDroppingLThreshold.load();
        size_t memoryCleared = 0;
        EventuallyPersistentStore *store = engine->getEpStore();
        // Get a list of active vbuckets sorted by memory usage
        // of their respective checkpoint managers.
        auto vbuckets = store->getVBuckets().getActiveVBucketsSortedByChkMgrMem();
        for (const auto& it: vbuckets) {
            if (memoryCleared < amountOfMemoryToClear) {
                uint16_t vbid = it.first;
                RCPtr<VBucket> vb = store->getVBucket(vbid);
                if (vb) {
                    // Get a list of cursors that can be dropped from the
                    // vbucket's checkpoint manager, so as to unreference
                    // an estimated number of checkpoints.
                    std::vector<std::string> cursors =
                                vb->checkpointManager.getListOfCursorsToDrop();
                    std::vector<std::string>::iterator itr = cursors.begin();
                    for (; itr != cursors.end(); ++itr) {
                        if (memoryCleared < amountOfMemoryToClear) {
                            if (engine->getDcpConnMap().handleSlowStream(vbid,
                                                                        *itr))
                            {
                                ++stats.cursorsDropped;
                                memoryCleared +=
                                      vb->getChkMgrMemUsageOfUnrefCheckpoints();
                            }
                        } else {
                            break;
                        }
                    }
                }
            } else {    // memoryCleared >= amountOfMemoryToClear
                break;
            }
        }
    }
}

bool ClosedUnrefCheckpointRemoverTask::run(void) {
    bool inverse = true;
    if (available.compare_exchange_strong(inverse, false)) {
        cursorDroppingIfNeeded();
        EventuallyPersistentStore *store = engine->getEpStore();
        std::shared_ptr<CheckpointVisitor> pv(new CheckpointVisitor(store, stats,
                                                               available));
        store->visit(pv, "Checkpoint Remover", NONIO_TASK_IDX,
                     TaskId::ClosedUnrefCheckpointRemoverVisitorTask);
    }
    snooze(sleepTime);
    return true;
}
