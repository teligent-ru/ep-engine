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
    KVStore(read_only), epStats(stats), couchNotifier(NULL)
{
    open();
}

NopKVStore::NopKVStore(const NopKVStore &copyFrom) :
    KVStore(copyFrom), epStats(copyFrom.epStats), couchNotifier(NULL)
{
    open();
}

NopKVStore::~NopKVStore() {
    close();
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
}

void NopKVStore::set(const Item &itm, Callback<mutation_result> &cb)
{
    // saveDocs had CouchNotifier logposition notification
    // lets hope nobody was expecting this notification, not doint it
    // our logposition never changes anyway, since we're not storing anything
}

void NopKVStore::get(const std::string &key, uint64_t, uint16_t vb,
                       Callback<GetValue> &cb, bool fetchDelete)
{
    GetValue rv;
    rv.setStatus(ENGINE_KEY_ENOENT);
    cb.callback(rv);
}

void NopKVStore::getWithHeader(void *dbHandle, const std::string &key,
                                 uint16_t vb, Callback<GetValue> &cb,
                                 bool fetchDelete) {
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
    newHeaderPos = 9; //maybe that'll make it happy? couchstore_get_header_position(targetDb);
    bool retVal = notifyCompaction(vbid, new_rev, VB_COMPACTION_DONE,
                                   newHeaderPos);

    //if (hook_ctx->expiredItems.size()) {
        cb.callback(*hook_ctx);
    //}

    return retVal;
}

bool CouchKVStore::notifyCompaction(const uint16_t vbid, uint64_t new_rev,
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
    // here they were were comparing our vbstate versus passed in argument
    // when changed, they called setVBucketState which called notifier
    // not sure if this is ever used, so writing to log:
    LOG(EXTENSION_LOG_WARNING,
        "Warning: snapshotVBucket called, returning true blindly, doing no notifications. is this wrong?"
        " vbucketId[%d], vbstate[%d]",
        (int)vbucketId, (int)vbstate);
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

bool NopKVStore::commit(Callback<kvstats_ctx> *cb, uint64_t snapStartSeqno,
                          uint64_t snapEndSeqno)
{
    return true;
}

uint64_t NopKVStore::getLastPersistedSeqno(uint16_t vbid) {
    return 9; // TODO paf: 1?
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

        return RollbackResult(false, 0, 0, 0);
}

ENGINE_ERROR_CODE NopKVStore::getAllKeys(uint16_t vbid,
                                           std::string &start_key,
                                           uint32_t count,
                                           AllKeysCB *cb) {
            return ENGINE_SUCCESS;
}

/* end of nop-kvstore.cc */
