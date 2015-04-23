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

#ifndef SRC_NOP_KVSTORE_COUCH_KVSTORE_H_
#define SRC_NOP_KVSTORE_COUCH_KVSTORE_H_ 1

#include "config.h"

#include <map>
#include <string>
#include <vector>

#include "configuration.h"
#include "couch-kvstore/couch-notifier.h"
#include "histo.h"
#include "item.h"
#include "kvstore.h"
#include "stats.h"
#include "tasks.h"

#define COUCHSTORE_NO_OPTIONS 0

class EventuallyPersistentEngine;
class EPStats;

/**
 * KVStore with couchstore as the underlying storage system
 */
class NopKVStore : public KVStore
{
public:
    /**
     * Constructor
     *
     * @param theEngine EventuallyPersistentEngine instance
     * @param read_only flag indicating if this kvstore instance is for read-only operations
     */
    NopKVStore(EPStats &stats, Configuration &config, bool read_only = false);

    /**
     * Copy constructor
     *
     * @param from the source kvstore instance
     */
    NopKVStore(const NopKVStore &from);

    /**
     * Deconstructor
     */
    ~NopKVStore();

    /**
     * Reset database to a clean state.
     */
    void reset(uint16_t vbucketId);

    /**
     * Begin a transaction (if not already in one).
     *
     * @return true if the transaction is started successfully
     */
    bool begin(void) {
        return true;
    }

    /**
     * Commit a transaction (unless not currently in one).
     *
     * @return true if the commit is completed successfully.
     */
    bool commit(Callback<kvstats_ctx> *cb, uint64_t snapStartSeqno,
                uint64_t snapEndSeqno);

    /**
     * Rollback a transaction (unless not currently in one).
     */
    void rollback(void) {
    }

    /**
     * Query the properties of the underlying storage.
     *
     * @return properties of the underlying storage system
     */
    StorageProperties getStorageProperties(void);

    /**
     * Insert or update a given document.
     *
     * @param itm instance representing the document to be inserted or updated
     * @param cb callback instance for SET
     */
    void set(const Item &itm, Callback<mutation_result> &cb);

    /**
     * Retrieve the document with a given key from the underlying storage system.
     *
     * @param key the key of a document to be retrieved
     * @param rowid the sequence number of a document
     * @param vb vbucket id of a document
     * @param cb callback instance for GET
     * @param fetchDelete True if we want to retrieve a deleted item if it not
     *        purged yet.
     */
    void get(const std::string &key, uint64_t rowid,
             uint16_t vb, Callback<GetValue> &cb, bool fetchDelete = false);

    void getWithHeader(void *dbHandle, const std::string &key,
                       uint16_t vb, Callback<GetValue> &cb,
                       bool fetchDelete = false);

    /**
     * Retrieve the multiple documents from the underlying storage system at once.
     *
     * @param vb vbucket id of a document
     * @param itms list of items whose documents are going to be retrieved
     */
    void getMulti(uint16_t vb, vb_bgfetch_queue_t &itms);

    /**
     * Delete a given document from the underlying storage system.
     *
     * @param itm instance representing the document to be deleted
     * @param cb callback instance for DELETE
     */
    void del(const Item &itm, Callback<int> &cb);

    /**
     * Delete a given vbucket database instance from the underlying storage system
     *
     * @param vbucket vbucket id
     * @param recreate true if we need to create an empty vbucket after deletion
     * @return true if the vbucket deletion is completed successfully.
     */
    bool delVBucket(uint16_t vbucket, bool recreate);

    /**
     * Retrieve the list of persisted vbucket states
     *
     * @return vbucket state vector instance where key is vbucket id and
     * value is vbucket state
     */
   std::vector<vbucket_state *>  listPersistedVbuckets(void);

    /**
     * Persist a snapshot of the engine stats in the underlying storage.
     *
     * @param engine_stats map instance that contains all the engine stats
     * @return true if the snapshot is done successfully
     */
    bool snapshotStats(const std::map<std::string, std::string> &engine_stats);

    /**
     * Persist a snapshot of the vbucket states in the underlying storage system.
     *
     * @param vbucketId vbucket id
     * @param vbstate vbucket state
     * @param cb - call back for updating kv stats
     * @return true if the snapshot is done successfully
     */
    bool snapshotVBucket(uint16_t vbucketId, vbucket_state &vbstate,
                         Callback<kvstats_ctx> *cb);

     /**
     * Compact a vbucket in the underlying storage system.
     *
     * @param vbid   - which vbucket needs to be compacted
     * @param hook_ctx - details of vbucket which needs to be compacted
     * @param cb - callback to help process newly expired items
     * @param kvcb - callback to update kvstore stats
     * @return true if the snapshot is done successfully
     */
    bool compactVBucket(const uint16_t vbid, compaction_ctx *cookie,
                        Callback<compaction_ctx> &cb,
                        Callback<kvstats_ctx> &kvcb);

    /**
     * Retrieve selected documents from the underlying storage system.
     *
     * @param vbids list of vbucket ids whose document keys are going to be retrieved
     * @param cb callback instance to process each document retrieved
     * @param cl callback to see if we need to read the value from disk
     */
    void dump(std::vector<uint16_t> &vbids, shared_ptr<Callback<GetValue> > cb,
              shared_ptr<Callback<CacheLookup> > cl);

    /**
     * Retrieve all the documents for a given vbucket from the storage system.
     *
     * @param vb vbucket id
     * @param cb callback instance to process each document retrieved
     * @param cl callback to see if we need to read the value from disk
     * @param sr callback to notify the caller what the range of the backfill is
     */
    void dump(uint16_t vb, uint64_t stSeqno,
              shared_ptr<Callback<GetValue> > cb,
              shared_ptr<Callback<CacheLookup> > cl,
              shared_ptr<Callback<SeqnoRange> > sr);

    /**
     * Retrieve all the keys from the underlying storage system.
     *
     * @param vbids list of vbucket ids whose document keys are going to be retrieved
     * @param cb callback instance to process each key retrieved
     */
    void dumpKeys(std::vector<uint16_t> &vbids,  shared_ptr<Callback<GetValue> > cb);

    /**
     * Retrieve the list of keys and their meta data for a given
     * vbucket, which were deleted.
     * @param vb vbucket id
     * @param cb callback instance to process each key and its meta data
     */
    void dumpDeleted(uint16_t vb, uint64_t stSeqno, uint64_t enSeqno,
                     shared_ptr<Callback<GetValue> > cb);

    /**
     * Does the underlying storage system support key-only retrieval operations?
     *
     * @return true if key-only retrieval is supported
     */
    bool isKeyDumpSupported() {
        return true;
    }

    /**
     * Do a rollback to the specified seqNo on the particular vbucket
     *
     * @param vbid The vbucket of the file that's to be rolled back
     * @param rollbackSeqno The sequence number upto which the engine needs
     * to be rolled back
     * @param cb getvalue callback
     */
    RollbackResult rollback(uint16_t vbid, uint64_t rollbackSeqno,
                            shared_ptr<RollbackCB> cb);

    uint64_t getLastPersistedSeqno(uint16_t vbid);

   /**
     * Get all_docs API, to return the list of all keys in the store
     */
    ENGINE_ERROR_CODE getAllKeys(uint16_t vbid, std::string &start_key,
                                 uint32_t count, AllKeysCB *cb);

private:
    bool setVBucketState(uint16_t vbucketId, vbucket_state &vbstate,
                         uint32_t vb_change_type, Callback<kvstats_ctx> *cb,
                         bool notify = true);
    bool resetVBucket(uint16_t vbucketId, vbucket_state &vbstate) {
        return setVBucketState(vbucketId, vbstate, VB_STATE_CHANGED, NULL);
    }

private:
    bool notifyCompaction(const uint16_t vbid, uint64_t new_rev,
                          uint32_t result, uint64_t header_pos);

    void operator=(const NopKVStore &from);

    void open();
    void close();

    EPStats &epStats;
    Configuration &configuration;
    CouchNotifier *couchNotifier;

    /* vbucket state cache*/
    std::vector<vbucket_state *> cachedVBStates;

};

#endif  // SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_
