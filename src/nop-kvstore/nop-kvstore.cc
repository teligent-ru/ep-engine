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

#ifdef _MSC_VER
#include <direct.h>
#define mkdir(a, b) _mkdir(a)
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <platform/dirutils.h>
#include <cJSON.h>

#include "common.h"
#include "nop-kvstore/nop-kvstore.h"

#include <JSON_checker.h>
#include <snappy-c.h>

NopKVStore::NopKVStore(EPStats &stats, Configuration &config, bool read_only) :
    KVStore(read_only), last_modified_vbid(0), epStats(stats), configuration(config), couchNotifier(NULL)
{
    open();

    // init db file map with default revision number, 1
    uint16_t numDbFiles = static_cast<uint16_t>(configuration.getMaxVbuckets());
    cachedVBStates.reserve(numDbFiles);
    for (uint16_t i = 0; i < numDbFiles; i++) {
        vbucket_state *state = new vbucket_state(vbucket_state_active,
            0, // _chkid,
            0, // _maxDelSeqNum,
            0); // _highSeqno)
        cachedVBStates.push_back(state);
    }
}

NopKVStore::NopKVStore(const NopKVStore &copyFrom) :
    KVStore(copyFrom), last_modified_vbid(0), epStats(copyFrom.epStats),
    configuration(copyFrom.configuration), couchNotifier(NULL)
{
    open();

}

NopKVStore::~NopKVStore() {
    close();

    for (std::vector<vbucket_state *>::iterator it = cachedVBStates.begin();
         it != cachedVBStates.end(); it++) {
        vbucket_state *vbstate = *it;
        if (vbstate) {
            delete vbstate;
            *it = NULL;
        }
    }
}

void NopKVStore::reset(uint16_t vbucketId)
{
    //cb_assert(!isReadOnly());
    // TODO CouchKVStore::flush() when couchstore api ready

    if (vbucketId == 0) {
//Notify just for first vbucket
        RememberingCallback<bool> cb;
        couchNotifier->flush(cb);
        cb.waitForValue();
    }

    vbucket_state *state = cachedVBStates[vbucketId];
    if (state) {
        state->checkpointId = 0;
        state->maxDeletedSeqno = 0;
        state->highSeqno = 0;
        state->lastSnapStart = 0;
        state->lastSnapEnd = 0;
        resetVBucket(vbucketId, *state);
    } else {
        LOG(EXTENSION_LOG_WARNING, "No entry in cached states "
                "for vbucket %u", vbucketId);
        cb_assert(false);
    }
}

void NopKVStore::set(const Item &itm, Callback<mutation_result> &cb)
{
    vbucket_state *state = cachedVBStates[itm.getVBucketId()];
            LOG(EXTENSION_LOG_WARNING,
                    "%s itm.getVBucketId[%u] state->highSeqno[%llu]\n",
                    __FUNCTION__, itm.getVBucketId(), state? state->highSeqno: -1);

    static const int MUTATION_SUCCESS = 1;

    saveDocs(itm.getVBucketId());
    mutation_result mr(MUTATION_SUCCESS, true);
    cb.callback(mr);
}

void NopKVStore::get(const std::string &key, uint64_t, uint16_t vb,
                       Callback<GetValue> &cb, bool fetchDelete)
{
    LOG(EXTENSION_LOG_WARNING,
        "%s: not implemented. key[%s] vb[%u]",
        __PRETTY_FUNCTION__, key.c_str(), vb);

    GetValue rv;
    rv.setStatus(ENGINE_KEY_ENOENT);
    cb.callback(rv);
}

void NopKVStore::getWithHeader(void *dbHandle, const std::string &key,
                                 uint16_t vb, Callback<GetValue> &cb,
                                 bool fetchDelete) {
    LOG(EXTENSION_LOG_WARNING,
        "%s: not implemented. key[%s] vb[%u]",
        __PRETTY_FUNCTION__, key.c_str(), vb);

    GetValue rv;
    rv.setStatus(ENGINE_KEY_ENOENT);
    cb.callback(rv);
}

void NopKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t &itms)
{
        vb_bgfetch_queue_t::iterator itr = itms.begin();
        for (; itr != itms.end(); ++itr) {
            std::list<VBucketBGFetchItem *> &fetches = (*itr).second;
            std::list<VBucketBGFetchItem *>::iterator fitr = fetches.begin();
            for (; fitr != fetches.end(); ++fitr) {
                (*fitr)->value.setStatus(ENGINE_NOT_MY_VBUCKET);
            }
        }
}

void NopKVStore::del(const Item &itm,
                       Callback<int> &cb)
{
    vbucket_state *state = cachedVBStates[itm.getVBucketId()];
            LOG(EXTENSION_LOG_WARNING,
                    "%s itm.getVBucketId[%u] state->highSeqno[%llu]\n",
                    __FUNCTION__, itm.getVBucketId(), state? state->highSeqno: -1);

    saveDocs(itm.getVBucketId());

    int success = 0;
    cb.callback(success);
}

bool NopKVStore::delVBucket(uint16_t vbucket, bool recreate)
{
    cb_assert(!isReadOnly());
    cb_assert(couchNotifier);
    RememberingCallback<bool> cb;

    couchNotifier->delVBucket(vbucket, cb);
    cb.waitForValue();

    vbucket_state *vbstate = new vbucket_state(vbucket_state_dead, 0, 0, 0);
    if (recreate) {
        if (cachedVBStates[vbucket]) {
            vbstate->state = cachedVBStates[vbucket]->state;
            delete cachedVBStates[vbucket];
        }
        cachedVBStates[vbucket] = vbstate;
        resetVBucket(vbucket, *vbstate);
    } else {
        if (cachedVBStates[vbucket]) {
            delete cachedVBStates[vbucket];
        }
        cachedVBStates[vbucket] = vbstate;
    }

    return cb.val;
}

std::vector<vbucket_state *> NopKVStore::listPersistedVbuckets()
{
    return cachedVBStates;
}

bool NopKVStore::compactVBucket(const uint16_t vbid,
                                  compaction_ctx *hook_ctx,
                                  Callback<compaction_ctx> &cb,
                                  Callback<kvstats_ctx> &kvcb) {
    // Notify MCCouch that compaction is Done...
    uint64_t                   new_rev = 0; //maybe it will not mind it actually not changes? fileRev + 1;
    uint64_t newHeaderPos = 9; //maybe that'll make it happy? couchstore_get_header_position(targetDb);
    bool retVal = notifyCompaction(vbid, new_rev, VB_COMPACTION_DONE,
                                   newHeaderPos);

    //if (hook_ctx->expiredItems.size()) {
        cb.callback(*hook_ctx);
    //}

    return retVal;
}

bool NopKVStore::notifyCompaction(const uint16_t vbid, uint64_t new_rev,
                                    uint32_t result, uint64_t header_pos) {
    RememberingCallback<uint16_t> lcb;

    VBStateNotification vbs(0, 0, result, vbid);

    // keeping this, looks like they are not happy without this notification, CB-3
    couchNotifier->notify_update(vbs, new_rev, header_pos, lcb);
    if (lcb.val != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "Warning: compactor failed to notify mccouch on vbucket "
                    "%d. err %d", vbid, lcb.val);
        return false;
    }
    return true;
}

bool NopKVStore::snapshotVBucket(uint16_t vbucketId, vbucket_state &vbstate,
                                   Callback<kvstats_ctx> *cb)
{
    vbucket_state *state = cachedVBStates[vbucketId];
            LOG(EXTENSION_LOG_WARNING,
                    "%s vbucketId[%u] state->highSeqno[%llu] vbstate.highSeqno[%llu]\n",
                    __FUNCTION__, vbucketId, state? state->highSeqno: -1, vbstate.highSeqno);

    //cb_assert(!isReadOnly());
    bool success = true;

    bool notify = false;
    uint32_t vb_change_type = VB_NO_CHANGE;
    if (state) {
        if (state->state != vbstate.state) {
            vb_change_type |= VB_STATE_CHANGED;
            notify = true;
        }
        if (state->checkpointId != vbstate.checkpointId) {
            vb_change_type |= VB_CHECKPOINT_CHANGED;
            notify = true;
        }

        if (state->failovers.compare(vbstate.failovers) == 0 &&
            vb_change_type == VB_NO_CHANGE) {
            return true; // no changes
        }
        state->state = vbstate.state;
        state->checkpointId = vbstate.checkpointId;
        state->failovers = vbstate.failovers;
        // Note that max deleted seq number is maintained within CouchKVStore
        vbstate.maxDeletedSeqno = state->maxDeletedSeqno;
    } else {
        state = new vbucket_state();
        *state = vbstate;
        vb_change_type = VB_STATE_CHANGED;
        cachedVBStates[vbucketId] = state;
        notify = true;
    }

    success = setVBucketState(vbucketId, vbstate, vb_change_type, cb,
                              notify);

    if (!success) {
        LOG(EXTENSION_LOG_WARNING,
            "Warning: failed to set new state, %s, for vbucket %d\n",
            VBucket::toString(vbstate.state), vbucketId);
        return false;
    }
    return success;
}

bool NopKVStore::setVBucketState(uint16_t vbucketId, vbucket_state &vbstate,
                                   uint32_t vb_change_type,
                                   Callback<kvstats_ctx> *kvcb,
                                   bool notify)
{
    vbucket_state *state = cachedVBStates[vbucketId];
    cb_assert(state);
            LOG(EXTENSION_LOG_WARNING,
                    "%s vbucketId[%u] state->highSeqno[%llu] vbstate.highSeqno[%llu]\n",
                    __FUNCTION__, vbucketId, state->highSeqno, vbstate.highSeqno);

    vbstate.highSeqno = state->highSeqno;
    vbstate.lastSnapStart = state->lastSnapStart;
    vbstate.lastSnapEnd = state->lastSnapEnd;
    vbstate.maxDeletedSeqno = state->maxDeletedSeqno;
/*
    // they originally save state.
    // God knows if they assume vbstate.highSeqno will change
    // will bump up highSeqno just in case (enough of failures)
    saveDocs(vbucketId);

        uint64_t newHeaderPos = state->highSeqno;
        RememberingCallback<uint16_t> lcb;

        VBStateNotification vbs(0/*vbstate.checkpointId* /, vbstate.state,
                vb_change_type, vbucketId);

        couchNotifier->notify_update(vbs, 1/*fileRev* /, newHeaderPos, lcb);
        if (lcb.val != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            cb_assert(lcb.val != PROTOCOL_BINARY_RESPONSE_ETMPFAIL);
            LOG(EXTENSION_LOG_WARNING,
                    "Warning: failed to notify CouchDB of update, "
                    "vbid=%u error=0x%x\n",
                    vbucketId, lcb.val);
            return false;
        }
*/
    return true;
}

bool NopKVStore::snapshotStats(const std::map<std::string, std::string> &stats)
{
    return true;
}

void NopKVStore::dump(std::vector<uint16_t> &vbids,
                        shared_ptr<Callback<GetValue> > cb,
                        shared_ptr<Callback<CacheLookup> > cl)
{
}

void NopKVStore::dump(uint16_t vb, uint64_t stSeqno,
                        shared_ptr<Callback<GetValue> > cb,
                        shared_ptr<Callback<CacheLookup> > cl,
                        shared_ptr<Callback<SeqnoRange> > sr)
{
}

void NopKVStore::dumpKeys(std::vector<uint16_t> &vbids,  shared_ptr<Callback<GetValue> > cb)
{
}

void NopKVStore::dumpDeleted(uint16_t vb, uint64_t stSeqno, uint64_t enSeqno,
                               shared_ptr<Callback<GetValue> > cb)
{
}

StorageProperties NopKVStore::getStorageProperties()
{
    StorageProperties rv(true, true, true, true);
    return rv;
}

bool NopKVStore::begin(void) {
    last_modified_vbid = 0;
    return true;
}

bool NopKVStore::commit(Callback<kvstats_ctx> *cb, uint64_t snapStartSeqno,
                          uint64_t snapEndSeqno)
{
    if(!last_modified_vbid)
        return true;
    
    vbucket_state *state = cachedVBStates[last_modified_vbid];
    cb_assert(state);
    //state->maxDeletedSeqno = TODO paf?
    state->lastSnapStart = snapStartSeqno;
    state->lastSnapEnd = snapEndSeqno;

    // originally they write VBState to storage to retrive it in case of rollback
    // that structure tases up some space, so +1 below
    ///moved from here to set/del  state->highSeqno = snapEndSeqno+1;

    // cb call is not needed, it only updates some file size stats which we do not have
    return true;
}

uint64_t NopKVStore::getLastPersistedSeqno(uint16_t vbid) {
    vbucket_state *state = cachedVBStates[vbid];
    if (state) {
        return state->highSeqno;
    }
    return 0;
}

void NopKVStore::open()
{
    // TODO intransaction, is it needed?
    {//if (!isReadOnly()) {
        couchNotifier = CouchNotifier::create(epStats, configuration);
    }
}

void NopKVStore::close()
{
    {//if (!isReadOnly()) {
        CouchNotifier::deleteNotifier();
    }
    couchNotifier = NULL;
}

RollbackResult NopKVStore::rollback(uint16_t vbid, uint64_t rollbackSeqno,
                                      shared_ptr<RollbackCB> cb) {

    LOG(EXTENSION_LOG_WARNING,
        "%s: not implemented. vbid[%d] rollbackSeqno[%lu]",
        __PRETTY_FUNCTION__, vbid, rollbackSeqno);
    return RollbackResult(false, 0, 0, 0);
}

ENGINE_ERROR_CODE NopKVStore::getAllKeys(uint16_t vbid,
                                           std::string &start_key,
                                           uint32_t count,
                                           AllKeysCB *cb) {
    LOG(EXTENSION_LOG_WARNING,
        "%s: not implemented. vbid[%d] start_key[%s] count[%u]",
        __PRETTY_FUNCTION__, vbid, start_key.c_str(), count);

    return ENGINE_SUCCESS;
}

void NopKVStore::saveDocs(uint16_t vbid) {
    last_modified_vbid = vbid;
    vbucket_state *state = cachedVBStates[vbid];
    cb_assert(state);
    state->highSeqno++;

    // saveDocs had CouchNotifier logposition notification
    // lets hope nobody was expecting this notification, not doint it
    // our logposition never changes anyway, since we're not storing anything
}

/* end of nop-kvstore.cc */
