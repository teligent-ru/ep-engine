/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc
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

#include <algorithm>
#include <vector>

#include "bgfetcher.h"
#include "ep.h"
#include "kvshard.h"
#include "executorthread.h"

const double BgFetcher::sleepInterval = MIN_SLEEP_TIME;

void BgFetcher::start() {
    bool inverse = false;
    pendingFetch.compare_exchange_strong(inverse, true);
    ExecutorPool* iom = ExecutorPool::get();
    ExTask task = new MultiBGFetcherTask(&(store->getEPEngine()), this, false);
    this->setTaskId(task->getId());
    iom->schedule(task, READER_TASK_IDX);
}

void BgFetcher::stop() {
    bool inverse = true;
    pendingFetch.compare_exchange_strong(inverse, false);
    ExecutorPool::get()->cancel(taskId);
}

void BgFetcher::notifyBGEvent(void) {
    ++stats.numRemainingBgJobs;
    bool inverse = false;
    if (pendingFetch.compare_exchange_strong(inverse, true)) {
        ExecutorPool::get()->wake(taskId);
    }
}

size_t BgFetcher::doFetch(VBucket::id_type vbId) {
    hrtime_t startTime(gethrtime());
    LOG(EXTENSION_LOG_DEBUG, "BgFetcher is fetching data, vBucket = %d "
        "numDocs = %" PRIu64 ", startTime = %" PRIu64,
        vbId, uint64_t(items2fetch.size()), startTime/1000000);

    shard->getROUnderlying()->getMulti(vbId, items2fetch);

    size_t totalfetches = 0;
    std::vector<bgfetched_item_t> fetchedItems;
    vb_bgfetch_queue_t::iterator itr = items2fetch.begin();
    for (; itr != items2fetch.end(); ++itr) {
        vb_bgfetch_item_ctx_t &bg_item_ctx = (*itr).second;
        std::list<VBucketBGFetchItem *> &requestedItems = bg_item_ctx.bgfetched_list;
        std::list<VBucketBGFetchItem *>::iterator itm = requestedItems.begin();
        for(; itm != requestedItems.end(); ++itm) {
            const std::string &key = (*itr).first;
            fetchedItems.push_back(std::make_pair(key, *itm));
            ++totalfetches;
        }
    }

    if (totalfetches > 0) {
        store->completeBGFetchMulti(vbId, fetchedItems, startTime);
        stats.getMultiHisto.add((gethrtime()-startTime)/1000, totalfetches);
    }

    // failed requests will get requeued for retry within clearItems()
    clearItems(vbId);
    return totalfetches;
}

void BgFetcher::clearItems(VBucket::id_type vbId) {
    vb_bgfetch_queue_t::iterator itr = items2fetch.begin();

    for(; itr != items2fetch.end(); ++itr) {
        // every fetched item belonging to the same key shares
        // a single data buffer, just delete it from the first fetched item
        vb_bgfetch_item_ctx_t& bg_item_ctx = (*itr).second;
        std::list<VBucketBGFetchItem *> &doneItems = bg_item_ctx.bgfetched_list;
        VBucketBGFetchItem *firstItem = doneItems.front();
        firstItem->delValue();

        std::list<VBucketBGFetchItem *>::iterator dItr = doneItems.begin();
        for (; dItr != doneItems.end(); ++dItr) {
            delete *dItr;
        }
    }
}

bool BgFetcher::run(GlobalTask *task) {
    size_t num_fetched_items = 0;
    bool inverse = true;
    pendingFetch.compare_exchange_strong(inverse, false);

    std::vector<uint16_t> bg_vbs;
    LockHolder lh(queueMutex);
    std::set<uint16_t>::iterator it = pendingVbs.begin();
    for (; it != pendingVbs.end(); ++it) {
        bg_vbs.push_back(*it);
    }
    pendingVbs.clear();
    lh.unlock();

    std::vector<uint16_t>::iterator ita = bg_vbs.begin();
    for (; ita != bg_vbs.end(); ++ita) {
        uint16_t vbId = *ita;
        if (store->getVBuckets().isBucketCreation(vbId)) {
            // Requeue the bg fetch task if a vbucket DB file is not
            // created yet.
            lh.lock();
            pendingVbs.insert(vbId);
            lh.unlock();
            bool inverse = false;
            pendingFetch.compare_exchange_strong(inverse, true);
            continue;
        }
        RCPtr<VBucket> vb = shard->getBucket(vbId);
        if (vb && vb->getBGFetchItems(items2fetch)) {
            num_fetched_items += doFetch(vbId);
            items2fetch.clear();
        }
    }

    stats.numRemainingBgJobs.fetch_sub(num_fetched_items);

    if (!pendingFetch.load()) {
        // wait a bit until next fetch request arrives
        double sleep = std::max(store->getBGFetchDelay(), sleepInterval);
        task->snooze(sleep);

        if (pendingFetch.load()) {
            // check again a new fetch request could have arrived
            // right before calling above snooze()
            task->snooze(0);
        }
    }
    return true;
}

bool BgFetcher::pendingJob() {
    for (auto vbid : shard->getVBuckets()) {
        RCPtr<VBucket> vb = shard->getBucket(vbid);
        if (vb && vb->hasPendingBGFetchItems()) {
            return true;
        }
    }
    return false;
}
