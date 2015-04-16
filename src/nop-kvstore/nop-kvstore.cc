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
    KVStore(read_only), epStats(stats)
{
}

NopKVStore::NopKVStore(const NopKVStore &copyFrom) :
    KVStore(copyFrom), epStats(copyFrom.epStats)
{
}

NopKVStore::~NopKVStore() {
}

void NopKVStore::reset(uint16_t vbucketId)
{
}

void NopKVStore::set(const Item &itm, Callback<mutation_result> &cb)
{
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
    return true;
}

std::vector<vbucket_state *> NopKVStore::listPersistedVbuckets()
{
    return cachedVBStates;
}

bool NopKVStore::compactVBucket(const uint16_t vbid,
                                  compaction_ctx *hook_ctx,
                                  Callback<compaction_ctx> &cb,
                                  Callback<kvstats_ctx> &kvcb) {
        return false;
}

bool NopKVStore::snapshotVBucket(uint16_t vbucketId, vbucket_state &vbstate,
                                   Callback<kvstats_ctx> *cb)
{
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
    return 0; // TODO paf: 1?
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
