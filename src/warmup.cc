/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc.
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

#include <limits>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ep_engine.h"
#include "failover-table.h"
#define STATWRITER_NAMESPACE warmup
#include "statwriter.h"
#undef STATWRITER_NAMESPACE
#include "mutation_log.h"
#include "connmap.h"
#include "warmup.h"

#include <platform/random.h>

class NoLookupCallback : public Callback<CacheLookup> {
public:
    NoLookupCallback() {}
    ~NoLookupCallback() {}
    void callback(CacheLookup&) {}
};

struct WarmupCookie {
    WarmupCookie(EventuallyPersistentStore *s, Callback<GetValue>&c) :
        cb(c), epstore(s),
        loaded(0), skipped(0), error(0)
    { /* EMPTY */ }
    Callback<GetValue> &cb;
    EventuallyPersistentStore *epstore;
    size_t loaded;
    size_t skipped;
    size_t error;
};

static bool batchWarmupCallback(uint16_t vbId,
                                std::vector<std::pair<std::string,
                                uint64_t> > &fetches,
                                void *arg)
{
    WarmupCookie *c = static_cast<WarmupCookie *>(arg);

    if (!c->epstore->maybeEnableTraffic()) {
        vb_bgfetch_queue_t items2fetch;
        std::vector<std::pair<std::string, uint64_t> >::iterator itm =
                                                              fetches.begin();
        for (; itm != fetches.end(); itm++) {
            // ignore duplicate keys, if any in access log
            if (items2fetch.find((*itm).first) != items2fetch.end()) {
                continue;
            }
            VBucketBGFetchItem *fit = new VBucketBGFetchItem(NULL, false);
            items2fetch[(*itm).first].push_back(fit);
        }

        c->epstore->getROUnderlying(vbId)->getMulti(vbId, items2fetch);

        vb_bgfetch_queue_t::iterator items = items2fetch.begin();
        for (; items != items2fetch.end(); items++) {
           VBucketBGFetchItem * fetchedItem = (*items).second.back();
           GetValue &val = fetchedItem->value;
           if (val.getStatus() == ENGINE_SUCCESS) {
                c->loaded++;
                c->cb.callback(val);
           } else {
                LOG(EXTENSION_LOG_WARNING,
                "Warning: warmup failed to load data for vBucket = %d"
                " key = %s error = %X\n",
                vbId,
                    (*items).first.c_str(), val.getStatus());
                c->error++;
          }
          delete fetchedItem;
        }

        return true;
    } else {
        c->skipped++;
        return false;
    }
}

static bool warmupCallback(void *arg, uint16_t vb, const std::string &key)
{
    WarmupCookie *cookie = static_cast<WarmupCookie*>(arg);

    if (!cookie->epstore->maybeEnableTraffic()) {
        RememberingCallback<GetValue> cb;
        cookie->epstore->getROUnderlying(vb)->get(key, vb, cb);
        cb.waitForValue();

        if (cb.val.getStatus() == ENGINE_SUCCESS) {
            cookie->cb.callback(cb.val);
            cookie->loaded++;
        } else {
            LOG(EXTENSION_LOG_WARNING, "Warning: warmup failed to load data "
                "for vBucket = %d key = %s error = %X\n", vb, key.c_str(),
                cb.val.getStatus());
            cookie->error++;
        }

        return true;
    } else {
        cookie->skipped++;
        return false;
    }
}

const int WarmupState::Initialize = 0;
const int WarmupState::CreateVBuckets = 1;
const int WarmupState::EstimateDatabaseItemCount = 2;
const int WarmupState::KeyDump = 3;
const int WarmupState::CheckForAccessLog = 4;
const int WarmupState::LoadingAccessLog = 5;
const int WarmupState::LoadingKVPairs = 6;
const int WarmupState::LoadingData = 7;
const int WarmupState::Done = 8;

const char *WarmupState::toString(void) const {
    return getStateDescription(state);
}

const char *WarmupState::getStateDescription(int st) const {
    switch (st) {
    case Initialize:
        return "initialize";
    case CreateVBuckets:
        return "creating vbuckets";
    case EstimateDatabaseItemCount:
        return "estimating database item count";
    case KeyDump:
        return "loading keys";
    case CheckForAccessLog:
        return "determine access log availability";
    case LoadingAccessLog:
        return "loading access log";
    case LoadingKVPairs:
        return "loading k/v pairs";
    case LoadingData:
        return "loading data";
    case Done:
        return "done";
    default:
        return "Illegal state";
    }
}

void WarmupState::transition(int to, bool allowAnystate) {
    if (allowAnystate || legalTransition(to)) {
        std::stringstream ss;
        ss << "Warmup transition from state \""
           << getStateDescription(state) << "\" to \""
           << getStateDescription(to) << "\"";
        LOG(EXTENSION_LOG_DEBUG, "%s", ss.str().c_str());
        state = to;
    } else {
        // Throw an exception to make it possible to test the logic ;)
        std::stringstream ss;
        ss << "Illegal state transition from \"" << *this << "\" to " << to;
        throw std::runtime_error(ss.str());
    }
}

bool WarmupState::legalTransition(int to) const {
    switch (state) {
    case Initialize:
        return (to == CreateVBuckets);
    case CreateVBuckets:
        return (to == EstimateDatabaseItemCount);
    case EstimateDatabaseItemCount:
        return (to == KeyDump || to == CheckForAccessLog);
    case KeyDump:
        return (to == LoadingKVPairs || to == CheckForAccessLog);
    case CheckForAccessLog:
        return (to == LoadingAccessLog || to == LoadingData ||
                to == LoadingKVPairs || to == Done);
    case LoadingAccessLog:
        return (to == Done || to == LoadingData);
    case LoadingKVPairs:
        return (to == Done);
    case LoadingData:
        return (to == Done);

    default:
        return false;
    }
}

std::ostream& operator <<(std::ostream &out, const WarmupState &state)
{
    out << state.toString();
    return out;
}

void LoadStorageKVPairCallback::callback(GetValue &val) {
    Item *i = val.getValue();
    bool stopLoading = false;
    if (i != NULL && !epstore->getWarmup()->isComplete()) {
        RCPtr<VBucket> vb = vbuckets.getBucket(i->getVBucketId());
        if (!vb) {
            return;
        }
        bool succeeded(false);
        int retry = 2;
        item_eviction_policy_t policy = epstore->getItemEvictionPolicy();
        do {
            if (i->getCas() == static_cast<uint64_t>(-1)) {
                if (val.isPartial()) {
                    i->setCas(0);
                } else {
                    i->setCas(vb->nextHLCCas());
                }
            }

            switch (vb->ht.insert(*i, policy, shouldEject(), val.isPartial())) {
            case NOMEM:
                if (retry == 2) {
                    if (hasPurged) {
                        if (++stats.warmOOM == 1) {
                            LOG(EXTENSION_LOG_WARNING,
                                "Warmup dataload failure: max_size too low.");
                        }
                    } else {
                        LOG(EXTENSION_LOG_WARNING,
                            "Emergency startup purge to free space for load.");
                        purge();
                    }
                } else {
                    LOG(EXTENSION_LOG_WARNING,
                        "Cannot store an item after emergency purge.");
                    ++stats.warmOOM;
                }
                break;
            case INVALID_CAS:
                if (vb->getShard()->getROUnderlying()->isKeyDumpSupported()) {
                    LOG(EXTENSION_LOG_DEBUG,
                        "Value changed in memory before restore from disk. "
                        "Ignored disk value for: %s.", i->getKey().c_str());
                } else {
                    LOG(EXTENSION_LOG_WARNING,
                        "Warmup dataload error: Duplicate key: %s.",
                        i->getKey().c_str());
                }
                ++stats.warmDups;
                succeeded = true;
                break;
            case NOT_FOUND:
                succeeded = true;
                break;
            default:
                abort();
            }
        } while (!succeeded && retry-- > 0);

        delete i;
        val.setValue(NULL);

        if (maybeEnableTraffic) {
            stopLoading = epstore->maybeEnableTraffic();
        }

        switch (warmupState) {
            case WarmupState::KeyDump:
                ++stats.warmedUpKeys;
                break;
            case WarmupState::LoadingData:
            case WarmupState::LoadingAccessLog:
                if (epstore->getItemEvictionPolicy() == FULL_EVICTION) {
                    ++stats.warmedUpKeys;
                }
                ++stats.warmedUpValues;
                break;
            default:
                ++stats.warmedUpKeys;
                ++stats.warmedUpValues;
        }
    } else {
        stopLoading = true;
        delete i;
    }

    if (stopLoading) {
        // warmup has completed, return ENGINE_ENOMEM to
        // cancel remaining data dumps from couchstore
        if (epstore->getWarmup()->setComplete()) {
            epstore->getWarmup()->setWarmupTime();
            epstore->warmupCompleted();
            LOG(EXTENSION_LOG_WARNING, "Warmup completed in %s",
                    hrtime2text(epstore->getWarmup()->getTime()).c_str());

        }
        LOG(EXTENSION_LOG_WARNING,
            "Engine warmup is complete, request to stop "
            "loading remaining database");
        setStatus(ENGINE_ENOMEM);
    } else {
        setStatus(ENGINE_SUCCESS);
    }
}

void LoadStorageKVPairCallback::purge() {
    class EmergencyPurgeVisitor : public VBucketVisitor {
    public:
        EmergencyPurgeVisitor(EventuallyPersistentStore *store) :
            epstore(store) {}

        void visit(StoredValue *v) {
            currentBucket->ht.unlocked_ejectItem(v,
                                             epstore->getItemEvictionPolicy());
        }
    private:
        EventuallyPersistentStore *epstore;
    };

    std::vector<int> vbucketIds(vbuckets.getBuckets());
    std::vector<int>::iterator it;
    EmergencyPurgeVisitor epv(epstore);
    for (it = vbucketIds.begin(); it != vbucketIds.end(); ++it) {
        int vbid = *it;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (vb && epv.visitBucket(vb)) {
            vb->ht.visit(epv);
        }
    }
    hasPurged = true;
}

void LoadValueCallback::callback(CacheLookup &lookup)
{
    if (warmupState == WarmupState::LoadingData) {
        RCPtr<VBucket> vb = vbuckets.getBucket(lookup.getVBucketId());
        if (!vb) {
            return;
        }

        int bucket_num(0);
        LockHolder lh = vb->ht.getLockedBucket(lookup.getKey(), &bucket_num);

        StoredValue *v = vb->ht.unlocked_find(lookup.getKey(), bucket_num);
        if (v && v->isResident()) {
            setStatus(ENGINE_KEY_EEXISTS);
            return;
        }
    }
    setStatus(ENGINE_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//    Implementation of the warmup class                                    //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////


Warmup::Warmup(EventuallyPersistentStore *st) :
    state(), store(st), taskId(0), startTime(0), metadata(0), warmup(0),
    threadtask_count(0),
    estimateTime(0), estimatedItemCount(std::numeric_limits<size_t>::max()),
    cleanShutdown(true), corruptAccessLog(false), warmupComplete(false),
    estimatedWarmupCount(std::numeric_limits<size_t>::max())
{
    shardVbStates = new std::map<uint16_t, vbucket_state>[
                                                       store->vbMap.numShards];
    shardVbIds = new std::vector<uint16_t>[store->vbMap.numShards];
    shardKeyDumpStatus = new bool[store->vbMap.numShards];
    for (size_t i = 0; i < store->vbMap.numShards; i++) {
        shardKeyDumpStatus[i] = false;
    }
}

Warmup::~Warmup() {
    delete [] shardVbStates;
    delete [] shardVbIds;
    delete [] shardKeyDumpStatus;
}

void Warmup::setEstimatedWarmupCount(size_t to)
{
    estimatedWarmupCount = to;
}

size_t Warmup::getEstimatedItemCount()
{
    return estimatedItemCount;
}

void Warmup::start(void)
{
    step();
}

void Warmup::stop(void)
{
    if (taskId) {
        ExecutorPool::get()->cancel(taskId);
        transition(WarmupState::Done, true);
        done();
    }
}

void Warmup::scheduleInitialize()
{
    ExTask task = new WarmupInitialize(*store, this,
            Priority::WarmupPriority);
    taskId = task->getId();
    ExecutorPool::get()->schedule(task, READER_TASK_IDX);
}

void Warmup::initialize()
{
    startTime = gethrtime();
    allVbStates = store->loadVBucketState();

    std::map<std::string, std::string> session_stats;
    store->getOneROUnderlying()->getPersistedStats(session_stats);
    store->getEPEngine().getTapConnMap().loadPrevSessionStats(session_stats);


    std::map<std::string, std::string>::const_iterator it =
        session_stats.find("ep_force_shutdown");

    if (it == session_stats.end() || it->second.compare("false") != 0) {
        cleanShutdown = false;
    }

    populateShardVbStates();
    transition(WarmupState::CreateVBuckets);
}

void Warmup::scheduleCreateVBuckets()
{
    threadtask_count = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        ExTask task = new WarmupCreateVBuckets(*store, i, this,
                                               Priority::WarmupPriority);
        taskId = task->getId();
        ExecutorPool::get()->schedule(task, READER_TASK_IDX);
    }
}

void Warmup::createVBuckets(uint16_t shardId) {
    size_t maxEntries = store->getEPEngine().getMaxFailoverEntries();
    std::map<uint16_t, vbucket_state>& vbStates = shardVbStates[shardId];

    std::map<uint16_t, vbucket_state>::iterator itr;
    for (itr = vbStates.begin(); itr != vbStates.end(); ++itr) {
        uint16_t vbid = itr->first;
        vbucket_state vbs = itr->second;

        RCPtr<VBucket> vb = store->getVBucket(vbid);
        if (!vb) {
            FailoverTable* table;
            if (vbs.failovers.empty()) {
                table = new FailoverTable(maxEntries);
            } else {
                table = new FailoverTable(vbs.failovers, maxEntries);
            }
            KVShard* shard = store->getVBuckets().getShard(vbid);
            shared_ptr<Callback<uint16_t> > cb(new NotifyFlusherCB(shard));
            vb.reset(new VBucket(vbid, vbs.state,
                                 store->getEPEngine().getEpStats(),
                                 store->getEPEngine().getCheckpointConfig(),
                                 shard, vbs.highSeqno, vbs.lastSnapStart,
                                 vbs.lastSnapEnd, table, cb, vbs.state, 1,
                                 vbs.purgeSeqno, vbs.maxCas,
                                 vbs.driftCounter));

            if(vbs.state == vbucket_state_active && !cleanShutdown) {
                if (static_cast<uint64_t>(vbs.highSeqno) == vbs.lastSnapEnd) {
                    vb->failovers->createEntry(vbs.lastSnapEnd);
                } else {
                    vb->failovers->createEntry(vbs.lastSnapStart);
                }
            }

            store->vbMap.addBucket(vb);
        }

        // Pass the open checkpoint Id for each vbucket.
        vb->checkpointManager.setOpenCheckpointId(vbs.checkpointId + 1);
        // Pass the max deleted seqno for each vbucket.
        vb->ht.setMaxDeletedRevSeqno(vbs.maxDeletedSeqno);
        // For each vbucket, set its latest checkpoint Id that was
        // successfully persisted.
        store->vbMap.setPersistenceCheckpointId(vbid, vbs.checkpointId);
        // For each vbucket, set the last persisted seqno checkpoint
        store->vbMap.setPersistenceSeqno(vbid, vbs.highSeqno);
    }

    if (++threadtask_count == store->vbMap.numShards) {
        transition(WarmupState::EstimateDatabaseItemCount);
    }
}


void Warmup::scheduleEstimateDatabaseItemCount()
{
    threadtask_count = 0;
    estimateTime = 0;
    estimatedItemCount = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        ExTask task = new WarmupEstimateDatabaseItemCount(
                                *store, i, this, Priority::WarmupPriority);
        taskId = task->getId();
        ExecutorPool::get()->schedule(task, READER_TASK_IDX);
    }
}

void Warmup::estimateDatabaseItemCount(uint16_t shardId)
{
    hrtime_t st = gethrtime();
    size_t item_count = 0;

    const std::vector<uint16_t> &vbs = shardVbIds[shardId];
    std::vector<uint16_t>::const_iterator it = vbs.begin();
    for (; it != vbs.end(); ++it) {
        DBFileInfo info = store->getRWUnderlyingByShard(shardId)->
                                                        getDbFileInfo(*it);
        RCPtr<VBucket> vb = store->getVBucket(*it);
        if (vb) {
            vb->ht.numTotalItems = info.itemCount;
            vb->fileSize = info.fileSize;
            vb->fileSpaceUsed = info.spaceUsed;

        }
        item_count += info.itemCount;
    }

    estimatedItemCount.fetch_add(item_count);
    estimateTime.fetch_add(gethrtime() - st);


    if (++threadtask_count == store->vbMap.numShards) {
        if (store->getItemEvictionPolicy() == VALUE_ONLY) {
            transition(WarmupState::KeyDump);
        } else {
            transition(WarmupState::CheckForAccessLog);
        }
    }
}

void Warmup::scheduleKeyDump()
{
    threadtask_count = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        ExTask task = new WarmupKeyDump(*store, this,
                                        i, Priority::WarmupPriority);
        taskId = task->getId();
        ExecutorPool::get()->schedule(task, READER_TASK_IDX);
    }

}

void Warmup::keyDumpforShard(uint16_t shardId)
{
    if (store->getROUnderlyingByShard(shardId)->isKeyDumpSupported()) {
        KVStore* kvstore = store->getROUnderlyingByShard(shardId);
        LoadStorageKVPairCallback *load_cb =
            new LoadStorageKVPairCallback(store, false, state.getState());
        shared_ptr<Callback<GetValue> > cb(load_cb);
        shared_ptr<Callback<CacheLookup> > cl(new NoLookupCallback());

        std::vector<uint16_t>::iterator itr = shardVbIds[shardId].begin();
        for (; itr != shardVbIds[shardId].end(); ++itr) {
            ScanContext* ctx = kvstore->initScanContext(cb, cl, *itr, 0, true,
                                                        true, false);
            if (ctx) {
                kvstore->scan(ctx);
                kvstore->destroyScanContext(ctx);
            }
        }

        shardKeyDumpStatus[shardId] = true;
    }

    if (++threadtask_count == store->vbMap.numShards) {
        bool success = false;
        for (size_t i = 0; i < store->vbMap.numShards; i++) {
            if (shardKeyDumpStatus[i]) {
                success = true;
            } else {
                success = false;
                break;
            }
        }

        if (success) {
            transition(WarmupState::CheckForAccessLog);
        } else {
            if (store->getROUnderlyingByShard(shardId)->isKeyDumpSupported()) {
                LOG(EXTENSION_LOG_WARNING,
                        "Failed to dump keys, falling back to full dump");
            }
            transition(WarmupState::LoadingKVPairs);
        }
    }
}

void Warmup::scheduleCheckForAccessLog()
{
    ExTask task = new WarmupCheckforAccessLog(*store, this,
            Priority::WarmupPriority);
    taskId = task->getId();
    ExecutorPool::get()->schedule(task, READER_TASK_IDX);
}

void Warmup::checkForAccessLog()
{
    metadata = gethrtime() - startTime;
    LOG(EXTENSION_LOG_WARNING, "metadata loaded in %s",
        hrtime2text(metadata).c_str());

    if (store->maybeEnableTraffic()) {
        transition(WarmupState::Done);
    }

    size_t accesslogs = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        std::string curr = store->accessLog[i]->getLogFile();
        std::string old = store->accessLog[i]->getLogFile();
        old.append(".old");
        if (access(curr.c_str(), F_OK) == 0 ||
            access(old.c_str(), F_OK) == 0) {
            accesslogs++;
        }
    }
    if (accesslogs == store->vbMap.shards.size()) {
        transition(WarmupState::LoadingAccessLog);
    } else {
        if (store->getItemEvictionPolicy() == VALUE_ONLY) {
            transition(WarmupState::LoadingData);
        } else {
            transition(WarmupState::LoadingKVPairs);
        }
    }

}

void Warmup::scheduleLoadingAccessLog()
{
    threadtask_count = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        ExTask task = new WarmupLoadAccessLog(*store, this, i,
                Priority::WarmupPriority);
        taskId = task->getId();
        ExecutorPool::get()->schedule(task, READER_TASK_IDX);
    }
}

void Warmup::loadingAccessLog(uint16_t shardId)
{

    LoadStorageKVPairCallback *load_cb =
        new LoadStorageKVPairCallback(store, true, state.getState());
    bool success = false;
    hrtime_t stTime = gethrtime();
    if (store->accessLog[shardId]->exists()) {
        try {
            store->accessLog[shardId]->open();
            if (doWarmup(*(store->accessLog[shardId]),
                         shardVbStates[shardId], *load_cb) != (size_t)-1) {
                success = true;
            }
        } catch (MutationLog::ReadException &e) {
            corruptAccessLog = true;
            LOG(EXTENSION_LOG_WARNING, "Error reading warmup access log:  %s",
                    e.what());
        }
    }

    if (!success) {
        // Do we have the previous file?
        std::string nm = store->accessLog[shardId]->getLogFile();
        nm.append(".old");
        MutationLog old(nm);
        if (old.exists()) {
            try {
                old.open();
                if (doWarmup(old, shardVbStates[shardId],
                             *load_cb) != (size_t)-1) {
                    success = true;
                }
            } catch (MutationLog::ReadException &e) {
                corruptAccessLog = true;
                LOG(EXTENSION_LOG_WARNING, "Error reading old access log:  %s",
                        e.what());
            }
        }
    }

    size_t numItems = store->getEPEngine().getEpStats().warmedUpValues;
    if (success && numItems) {
        LOG(EXTENSION_LOG_WARNING,
            "%d items loaded from access log, completed in %s", numItems,
            hrtime2text((gethrtime() - stTime) / 1000).c_str());
    } else {
        size_t estimatedCount= store->getEPEngine().getEpStats().warmedUpKeys;
        setEstimatedWarmupCount(estimatedCount);
    }

    delete load_cb;
    if (++threadtask_count == store->vbMap.numShards) {
        if (!store->maybeEnableTraffic()) {
            transition(WarmupState::LoadingData);
        } else {
            transition(WarmupState::Done);
        }

    }
}

size_t Warmup::doWarmup(MutationLog &lf, const std::map<uint16_t,
                        vbucket_state> &vbmap, Callback<GetValue> &cb)
{
    MutationLogHarvester harvester(lf, &store->getEPEngine());
    std::map<uint16_t, vbucket_state>::const_iterator it;
    for (it = vbmap.begin(); it != vbmap.end(); ++it) {
        harvester.setVBucket(it->first);
    }

    hrtime_t st = gethrtime();
    if (!harvester.load()) {
        return -1;
    }
    hrtime_t end = gethrtime();

    size_t total = harvester.total();
    setEstimatedWarmupCount(total);
    LOG(EXTENSION_LOG_DEBUG, "Completed log read in %s with %ld entries",
        hrtime2text(end - st).c_str(), total);

    st = gethrtime();
    WarmupCookie cookie(store, cb);
    if (store->multiBGFetchEnabled()) {
        harvester.apply(&cookie, &batchWarmupCallback);
    } else {
        harvester.apply(&cookie, &warmupCallback);
    }
    end = gethrtime();
    LOG(EXTENSION_LOG_DEBUG,
        "Populated log in %s with(l: %ld, s: %ld, e: %ld)",
        hrtime2text(end - st).c_str(), cookie.loaded, cookie.skipped,
        cookie.error);
    return cookie.loaded;
}

void Warmup::scheduleLoadingKVPairs()
{
    // We reach here only if keyDump didn't return SUCCESS or if
    // in case of Full Eviction. Either way, set estimated value
    // count equal to the estimated item count, as very likely no
    // keys have been warmed up at this point.
    setEstimatedWarmupCount(estimatedItemCount);

    threadtask_count = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        ExTask task = new WarmupLoadingKVPairs(*store, this,
                                               i, Priority::WarmupPriority);
        taskId = task->getId();
        ExecutorPool::get()->schedule(task, READER_TASK_IDX);
    }

}

void Warmup::loadKVPairsforShard(uint16_t shardId)
{
    bool maybe_enable_traffic = false;
    if (store->getItemEvictionPolicy() == FULL_EVICTION) {
        maybe_enable_traffic = true;
    }

    KVStore* kvstore = store->getROUnderlyingByShard(shardId);
    LoadStorageKVPairCallback *load_cb =
        new LoadStorageKVPairCallback(store, maybe_enable_traffic,
                                      state.getState());
    shared_ptr<Callback<GetValue> > cb(load_cb);
    shared_ptr<Callback<CacheLookup> >
        cl(new LoadValueCallback(store->vbMap, state.getState()));

    std::vector<uint16_t>::iterator itr = shardVbIds[shardId].begin();
    for (; itr != shardVbIds[shardId].end(); ++itr) {
        ScanContext* ctx = kvstore->initScanContext(cb, cl, *itr, 0, false,
                                                    true, false);
        if (ctx) {
            kvstore->scan(ctx);
            kvstore->destroyScanContext(ctx);
        }
    }
    if (++threadtask_count == store->vbMap.numShards) {
        transition(WarmupState::Done);
    }
}

void Warmup::scheduleLoadingData()
{
    size_t estimatedCount = store->getEPEngine().getEpStats().warmedUpKeys;
    setEstimatedWarmupCount(estimatedCount);

    threadtask_count = 0;
    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        ExTask task = new WarmupLoadingData(*store, this,
                                            i, Priority::WarmupPriority);
        taskId = task->getId();
        ExecutorPool::get()->schedule(task, READER_TASK_IDX);
    }
}

void Warmup::loadDataforShard(uint16_t shardId)
{
    KVStore* kvstore = store->getROUnderlyingByShard(shardId);
    LoadStorageKVPairCallback *load_cb =
        new LoadStorageKVPairCallback(store, true, state.getState());
    shared_ptr<Callback<GetValue> > cb(load_cb);
    shared_ptr<Callback<CacheLookup> >
        cl(new LoadValueCallback(store->vbMap, state.getState()));

    std::vector<uint16_t>::iterator itr = shardVbIds[shardId].begin();
    for (; itr != shardVbIds[shardId].end(); ++itr) {
        ScanContext* ctx = kvstore->initScanContext(cb, cl, *itr, 0, false,
                                                    true, false);
        if (ctx) {
            kvstore->scan(ctx);
            kvstore->destroyScanContext(ctx);
        }
    }

    if (++threadtask_count == store->vbMap.numShards) {
        transition(WarmupState::Done);
    }
}

void Warmup::scheduleCompletion() {
    ExTask task = new WarmupCompletion(*store, this,
                                       Priority::WarmupPriority);
    taskId = task->getId();
    ExecutorPool::get()->schedule(task, READER_TASK_IDX);
}

void Warmup::done()
{
    if (setComplete()) {
        setWarmupTime();
        store->warmupCompleted();
        LOG(EXTENSION_LOG_WARNING, "warmup completed in %s",
                                   hrtime2text(warmup).c_str());
    }
}

void Warmup::step() {
    try {
        switch (state.getState()) {
        case WarmupState::Initialize:
            scheduleInitialize();
            break;
        case WarmupState::CreateVBuckets:
            scheduleCreateVBuckets();
            break;
        case WarmupState::EstimateDatabaseItemCount:
            scheduleEstimateDatabaseItemCount();
            break;
        case WarmupState::KeyDump:
            scheduleKeyDump();
            break;
        case WarmupState::CheckForAccessLog:
            scheduleCheckForAccessLog();
            break;
        case WarmupState::LoadingAccessLog:
            scheduleLoadingAccessLog();
            break;
        case WarmupState::LoadingKVPairs:
            scheduleLoadingKVPairs();
            break;
        case WarmupState::LoadingData:
            scheduleLoadingData();
            break;
        case WarmupState::Done:
            scheduleCompletion();
            break;
        default:
            LOG(EXTENSION_LOG_WARNING,
                "Internal error.. Illegal warmup state %d", state.getState());
            abort();
        }
    } catch(std::runtime_error &e) {
        std::stringstream ss;
        ss << "Exception in warmup loop: " << e.what() << std::endl;
        LOG(EXTENSION_LOG_WARNING, "%s", ss.str().c_str());
        abort();
    }
}

void Warmup::transition(int to, bool force) {
    int old = state.getState();
    if (old != WarmupState::Done) {
        state.transition(to, force);
        step();
    }
}

template <typename T>
void Warmup::addStat(const char *nm, const T &val, ADD_STAT add_stat,
                     const void *c) const {
    std::string name = "ep_warmup";
    if (nm != NULL) {
        name.append("_");
        name.append(nm);
    }

    std::stringstream value;
    value << val;
    add_casted_stat(name.data(), value.str().data(), add_stat, c);
}

void Warmup::addStats(ADD_STAT add_stat, const void *c) const
{
    if (store->getEPEngine().getConfiguration().isWarmup()) {
        EPStats &stats = store->getEPEngine().getEpStats();
        addStat(NULL, "enabled", add_stat, c);
        const char *stateName = state.toString();
        addStat("state", stateName, add_stat, c);
        if (warmupComplete.load()) {
            addStat("thread", "complete", add_stat, c);
        } else {
            addStat("thread", "running", add_stat, c);
        }
        addStat("key_count", stats.warmedUpKeys, add_stat, c);
        addStat("value_count", stats.warmedUpValues, add_stat, c);
        addStat("dups", stats.warmDups, add_stat, c);
        addStat("oom", stats.warmOOM, add_stat, c);
        addStat("item_expired", stats.warmupExpired, add_stat, c);
        addStat("min_memory_threshold",
                stats.warmupMemUsedCap * 100.0, add_stat, c);
        addStat("min_item_threshold",
                stats.warmupNumReadCap * 100.0, add_stat, c);

        if (metadata > 0) {
            addStat("keys_time", metadata / 1000, add_stat, c);
        }

        if (warmup > 0) {
            addStat("time", warmup / 1000, add_stat, c);
        }

        if (estimatedItemCount == std::numeric_limits<size_t>::max()) {
            addStat("estimated_key_count", "unknown", add_stat, c);
        } else {
            if (estimateTime != 0) {
                addStat("estimate_time", estimateTime / 1000, add_stat, c);
            }
            addStat("estimated_key_count", estimatedItemCount, add_stat, c);
        }

        if (corruptAccessLog) {
            addStat("access_log", "corrupt", add_stat, c);
        }

        if (estimatedWarmupCount ==  std::numeric_limits<size_t>::max()) {
            addStat("estimated_value_count", "unknown", add_stat, c);
        } else {
            addStat("estimated_value_count", estimatedWarmupCount,
            add_stat, c);
        }
   } else {
        addStat(NULL, "disabled", add_stat, c);
    }
}

void Warmup::populateShardVbStates()
{
    for (uint16_t vb = 0; vb < allVbStates.size(); vb++) {
        if (!allVbStates[vb]) {
            continue;
        }
        std::map<uint16_t, vbucket_state> &shardVB =
            shardVbStates[vb % store->vbMap.numShards];
        shardVB.insert(std::pair<uint16_t, vbucket_state>(vb,
                                                          *(allVbStates[vb])));
    }

    for (size_t i = 0; i < store->vbMap.shards.size(); i++) {
        std::vector<uint16_t> activeVBs, replicaVBs;
        std::map<uint16_t, vbucket_state>::const_iterator it;
        for (it = shardVbStates[i].begin(); it != shardVbStates[i].end(); ++it) {
            uint16_t vbid = it->first;
            vbucket_state vbs = it->second;
            if (vbs.state == vbucket_state_active) {
                activeVBs.push_back(vbid);
            } else if (vbs.state == vbucket_state_replica) {
                replicaVBs.push_back(vbid);
            }
        }

        // Order the vbucket ids into the vector for each shard in such a
        // way, that active vbuckets get 60% preference and replica vbuckets
        // get 40% preference.

        Couchbase::RandomGenerator provider(true);
        std::vector<uint16_t>::iterator it1 = activeVBs.begin();
        std::vector<uint16_t>::iterator it2 = replicaVBs.begin();
        while (it1 != activeVBs.end() || it2 != replicaVBs.end()) {
            uint64_t num = provider.next();
            if ((num % 2 == 0 || num % 5 == 0) && it1 != activeVBs.end()) {
                shardVbIds[i].push_back(*it1);
                ++it1;
            } else if (it2 != replicaVBs.end()) {
                shardVbIds[i].push_back(*it2);
                ++it2;
            }
        }

    }
}
