/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "forest-kvstore/forest-kvstore.h"

#include "common.h"

#include <sys/stat.h>
#include <platform/dirutils.h>
#include <vbucket.h>
#include <JSON_checker.h>
#include <locks.h>

#define STATWRITER_NAMESPACE forestdb_engine
#include "statwriter.h"
#undef STATWRITER_NAMESPACE

using namespace CouchbaseDirectoryUtilities;

std::mutex ForestKVStore::initLock;
int ForestKVStore::numGlobalFiles = 0;

extern "C" {
    static fdb_compact_decision compaction_cb_c(fdb_file_handle* fhandle,
                            fdb_compaction_status status, const char* kv_name,
                            fdb_doc* doc, uint64_t old_offset,
                            uint64_t new_offset, void* ctx) {
        return ForestKVStore::compaction_cb(fhandle, status, kv_name, doc,
                                            old_offset, new_offset, ctx);
    }

    static void errorlog_cb(int err_code, const char* err_msg, void* ctx_data) {
        LOG(EXTENSION_LOG_WARNING, "%s with error: %s",
            err_msg, fdb_error_msg(static_cast<fdb_status>(err_code)));
    }
}


void ForestKVStore::initForestDb() {
    LockHolder lh(initLock);
    if (numGlobalFiles == 0) {
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        fdb_status status = fdb_init(&fileConfig);
        if (status != FDB_RESULT_SUCCESS) {
            throw std::logic_error("ForestKVStore::initForestDb: failed "
                    "with status:" + std::to_string(status));
        }
        ObjectRegistry::onSwitchThread(epe);
    }
    ++numGlobalFiles;
}

void ForestKVStore::shutdownForestDb() {
   LockHolder lh(initLock);
   if (--numGlobalFiles == 0) {
       EventuallyPersistentEngine* epe = ObjectRegistry::onSwitchThread(NULL, true);
       fdb_status status = fdb_shutdown();
       if (status != FDB_RESULT_SUCCESS) {
           LOG(EXTENSION_LOG_WARNING,
               "ForestKVStore::shutdownForestDb: Shutting down forestdb failed "
               "with error: %s", fdb_error_msg(status));
       }
       ObjectRegistry::onSwitchThread(epe);
   }
}

ForestKVStore::ForestKVStore(KVStoreConfig &config) :
    KVStore(config), intransaction(false),
    dbname(config.getDBName()), dbFileRevNum(1), scanCounter(0) {

    /* create the data directory */
    createDataDir(dbname);
    fdb_status status;

    uint16_t shardId = config.getShardId();
    uint16_t maxVbuckets = config.getMaxVBuckets();
    uint16_t maxShards = config.getMaxShards();

    std::stringstream dbFile;
    dbFile << dbname << "/" << shardId << ".fdb";

    std::stringstream prefix;
    prefix << shardId << ".fdb";

    const std::vector<std::string> files = findFilesContaining(dbname,
                                           prefix.str().c_str());

    for (auto& filename : files) {
        size_t secondDot = filename.rfind(".");
        std::string revNumStr = filename.substr(secondDot + 1);
        char *ptr = NULL;
        uint64_t revNum = strtoull(revNumStr.c_str(), &ptr, 10);
        if (revNum == 0) {
            LOG(EXTENSION_LOG_WARNING,
                "Invalid revision number obtained for database file");
            abort();
        }

        if (revNum > dbFileRevNum) {
            dbFileRevNum = revNum;
        }
    }

    dbFileNameStr = dbFile.str();

    dbFile << "." << dbFileRevNum;

    fileConfig = fdb_get_default_config();
    kvsConfig = fdb_get_default_kvs_config();

    /* Set the purge interval to the maximum possible value to ensure
     * that deleted items don't get removed immediately. The tombstone
     * items will be purged in the expiry callback.
     */
    fileConfig.purging_interval = std::numeric_limits<uint32_t>::max();

    /* Since DCP requires sequence based iteration, enable sequence tree
     * indexes in forestdb
     */
    fileConfig.seqtree_opt = FDB_SEQTREE_USE;

    /* Enable compression of document body in order to occupy less
     * space on disk
     */
    fileConfig.compress_document_body = true;

    /* Disable breakpad. The memcached binary already has breakpad
     * initialized. ForestDB is already part of the memcached
     * binary and thus doesn't have to be initialized again
     * for ForestDB.
     */
    fileConfig.breakpad_minidump_dir = nullptr;

    statCollectingFileOps = getForestStatOps(&st.fsStats);

    fileConfig.custom_file_ops = &statCollectingFileOps;

    initForestDb();

    status = fdb_open(&readDBFileHandle, dbFile.str().c_str(), &fileConfig);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::ForestKVStore: Opening the "
            "read database file instance failed with error: " +
            std::string(fdb_error_msg(status)));
    }

    status = fdb_open(&writeDBFileHandle, dbFile.str().c_str(), &fileConfig);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::ForestKVStore: Opening the "
            "write database file instance failed with error: " +
            std::string(fdb_error_msg(status)));
    }

    fdb_kvs_handle* kvsHandle = NULL;
    /* Initialize the reader and writer handle map for all the vbuckets in
     * the shard
     */
    for (uint16_t i = shardId; i < maxVbuckets; i += maxShards) {
        writeHandleMap.insert(std::make_pair(i, kvsHandle));
        readHandleMap.insert(std::make_pair(i, kvsHandle));
    }

    // Initialize the handle for the vbucket state information
    status = fdb_kvs_open_default(readDBFileHandle,
                                  &readVbStateHandle, &kvsConfig);

    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::ForestKVStore: Opening the "
            "read vbucket state KV store instance failed with error: " +
            std::string(fdb_error_msg(status)));
    }

    status = fdb_set_log_callback(readVbStateHandle, errorlog_cb, nullptr);

    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::ForestKVStore: Setting the "
            "log callback failed with error: " + std::string(fdb_error_msg(status)));
    }

    status = fdb_kvs_open_default(writeDBFileHandle,
                                  &writeVbStateHandle, &kvsConfig);

    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::ForestKVStore: Opening the "
            "write vbucket state KV store instance failed with error: " +
            std::string(fdb_error_msg(status)));
    }

    status = fdb_set_log_callback(writeVbStateHandle, errorlog_cb, nullptr);

    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::ForestKVStore: Setting the "
            "log callback failed with error: " + std::string(fdb_error_msg(status)));
    }

    cachedVBStates.reserve(maxVbuckets);
    cachedValidVBCount = 0;
    for (uint16_t i = 0; i < maxVbuckets; ++i) {
        cachedVBStates.push_back((vbucket_state *)NULL);
        if (i == shardId) {
            if (files.size() != 0) {
                readVBState(i);
                if (cachedVBStates[i] && cachedVBStates[i]->state != vbucket_state_dead) {
                    cachedValidVBCount++;
                    st.numLoadedVb++;
                }

                updateFileInfo();
            }
            shardId += maxShards; // jump to next vbucket in shard
        }
    }

    cachedDocCount.assign(maxVbuckets, Couchbase::RelaxedAtomic<size_t>(-1));
    cachedDeleteCount.assign(maxVbuckets, Couchbase::RelaxedAtomic<size_t>(-1));
}

fdb_config ForestKVStore::getFileConfig() {
    return fileConfig;
}

fdb_kvs_config ForestKVStore::getKVConfig() {
    return kvsConfig;
}

ForestKVStore::ForestKVStore(const ForestKVStore &copyFrom) :
    KVStore(copyFrom), intransaction(false),
    dbname(copyFrom.dbname) {

    /* create the data directory */
    createDataDir(dbname);
    LockHolder lh(initLock);
    ++numGlobalFiles;
}

ForestKVStore::~ForestKVStore() {
   close();

   /* delete all the cached vbucket states */
   std::vector<vbucket_state *>::iterator stateItr;
   for (stateItr = cachedVBStates.begin(); stateItr != cachedVBStates.end();
        stateItr++) {
        vbucket_state* vbstate = *stateItr;
        if (vbstate) {
            delete vbstate;
            *stateItr = NULL;
        }
   }

   /* Close all the KV store instances */
   std::unordered_map<uint16_t, fdb_kvs_handle *>::iterator handleItr;
   for (handleItr = writeHandleMap.begin(); handleItr != writeHandleMap.end();
        handleItr++) {
       fdb_kvs_handle* kvsHandle = handleItr->second;

       if (kvsHandle) {
           fdb_kvs_close(kvsHandle);
       }
   }

   writeHandleMap.clear();

   for (handleItr = readHandleMap.begin(); handleItr != readHandleMap.end();
        handleItr++) {
       fdb_kvs_handle* kvsHandle = handleItr->second;

       if (kvsHandle) {
           fdb_kvs_close(kvsHandle);
       }
   }

   readHandleMap.clear();

   /* Close the database file instance */
   fdb_close(readDBFileHandle);
   fdb_close(writeDBFileHandle);

   shutdownForestDb();
}

void ForestKVStore::reset(uint16_t vbucketId) {
    vbucket_state* state = cachedVBStates[vbucketId];
    if (!state) {
        throw std::invalid_argument("ForestKVStore::reset::No entry "
                "in cached states for vbucket " +
                std::to_string(vbucketId));
    }

    state->reset();

    cachedDocCount[vbucketId] = 0;
    cachedDeleteCount[vbucketId] = 0;

    fdb_kvs_handle* kvsHandle = getKvsHandle(vbucketId, handleType::READER);
    fdb_status status;

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::reset:fdb_kvs_close API call failed for "
                "vbucket %" PRIu16" with error: %s", vbucketId,
                fdb_error_msg(status));
        }
    }

    readHandleMap[vbucketId] = nullptr;

    kvsHandle = getKvsHandle(vbucketId, handleType::WRITER);

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::reset:fdb_kvs_close API call failed for "
                "vbucket %" PRIu16" with error: %s", vbucketId,
                fdb_error_msg(status));
        }
    }

    writeHandleMap[vbucketId] = nullptr;

    char kvsName[20];
    sprintf(kvsName,"partition%" PRIu16, vbucketId);

    LockHolder lh(writerLock);

    status = fdb_kvs_remove(writeDBFileHandle, kvsName);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::reset: ForestDB KV Store remove failed for "
            "vbucket :%" PRIu16" with error: %s", vbucketId,
            fdb_error_msg(status));
    }

    std::string stateStr = state->toJSON();

    if (!stateStr.empty()) {
        char keybuf[20];
        fdb_doc statDoc;
        memset(&statDoc, 0, sizeof(statDoc));
        sprintf(keybuf, "partition%d", vbucketId);
        statDoc.key = keybuf;
        statDoc.keylen = strlen(keybuf);
        statDoc.meta = NULL;
        statDoc.metalen = 0;
        statDoc.body = const_cast<char *>(stateStr.c_str());
        statDoc.bodylen = stateStr.length();
        fdb_status status = fdb_set(writeVbStateHandle, &statDoc);

        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::reset:Failed to save "
                "vbucket state for vbucket=%" PRIu16" error=%s", vbucketId,
                fdb_error_msg(status));
        }
    }

    updateFileInfo();
}

ForestRequest::ForestRequest(const Item &it, MutationRequestCallback &cb,
                             bool del, size_t itmDataSize = 0)
    : IORequest(it.getVBucketId(), cb, del, it.getKey()),
      status(MUTATION_SUCCESS),
      dataSize(itmDataSize) { }

ForestRequest::~ForestRequest() { }

void ForestKVStore::close() {
    intransaction = false;
}

ENGINE_ERROR_CODE ForestKVStore::readVBState(uint16_t vbId) {
    fdb_status status = FDB_RESULT_SUCCESS;
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    std::string failovers;
    int64_t highSeqno = 0;
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t driftCounter = INITIAL_DRIFT;

    fdb_kvs_info kvsInfo;

    std::unique_ptr<ForestKvsHandle> fkvsHandle;
    try {
        fkvsHandle = createKvsHandle(vbId);
    } catch (std::exception& e) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::readVBState: creating a new ForestDB "
            "KVStore handle failed for vbucket: %" PRIu16" with error: %s",
            vbId, e.what());
        return ENGINE_FAILED;
    }

    status = fdb_get_kvs_info(fkvsHandle->getKvsHandle(), &kvsInfo);
    //TODO: Update the purge sequence number
    if (status == FDB_RESULT_SUCCESS) {
        highSeqno = static_cast<int64_t>(kvsInfo.last_seqnum);
    } else {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::readVBState: Failed to "
            "read KV Store info for vbucket: %d with error: %s", vbId,
            fdb_error_msg(status));
        return forestErr2EngineErr(status);
    }

    char keybuf[20];
    fdb_doc* statDoc;
    sprintf(keybuf, "partition%d", vbId);
    fdb_doc_create(&statDoc, (void *)keybuf, strlen(keybuf), NULL, 0, NULL, 0);
    status = fdb_get(readVbStateHandle, statDoc);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_DEBUG,
            "ForestKVStore::readVBState: Failed to retrieve vbucket state for vBucket=%d "
            "with error=%s", vbId , fdb_error_msg(status));
    } else {
        cJSON* jsonObj = cJSON_Parse((char *)statDoc->body);

        if (!jsonObj) {
            fdb_doc_free(statDoc);
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::readVBState: Failed to "
                "parse the vbstat json doc for vbucket: %d: %s", vbId,
                (char *)statDoc->body);
            return forestErr2EngineErr(status);
        }

        const std::string vb_state = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "state"));

        const std::string checkpoint_id = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj,"checkpoint_id"));

        const std::string max_deleted_seqno = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "max_deleted_seqno"));

        const std::string snapStart = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "snap_start"));

        const std::string snapEnd = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "snap_end"));

        const std::string maxCasValue = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "max_cas"));

        const std::string driftCount = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "drift_counter"));

        cJSON* failover_json = cJSON_GetObjectItem(jsonObj, "failover_table");
        if (vb_state.compare("") == 0 || checkpoint_id.compare("") == 0
               || max_deleted_seqno.compare("") == 0) {
             LOG(EXTENSION_LOG_WARNING,
                 "ForestKVStore::readVBState: State JSON "
                 "doc for vbucket: %d is in the wrong format: %s,"
                 "vb state: %s, checkpoint id: %s and max deleted seqno: %s",
                 vbId, (char *)statDoc->body, vb_state.c_str(),
                 checkpoint_id.c_str(), max_deleted_seqno.c_str());
        } else {
            state = VBucket::fromString(vb_state.c_str());
            parseUint64(max_deleted_seqno.c_str(), &maxDeletedSeqno);
            parseUint64(checkpoint_id.c_str(), &checkpointId);

            if (snapStart.compare("")) {
                parseUint64(snapStart.c_str(), &lastSnapStart);
            }

            if (snapEnd.compare("")) {
                parseUint64(snapEnd.c_str(), &lastSnapEnd);
            }

            if (maxCasValue.compare("")) {
                parseUint64(maxCasValue.c_str(), &maxCas);
            }

            if (driftCount.compare("")) {
                parseInt64(driftCount.c_str(), &driftCounter);
            }

            if (failover_json) {
                char* json = cJSON_PrintUnformatted(failover_json);
                failovers.assign(json);
                free(json);
            }
        }

        cJSON_Delete(jsonObj);
    }

    if (failovers.empty()) {
        failovers.assign("[{\"id\":0,\"seq\":0}]");
    }

    delete cachedVBStates[vbId];
    cachedVBStates[vbId] = new vbucket_state(state, checkpointId,
                                             maxDeletedSeqno, highSeqno, 0,
                                             lastSnapStart, lastSnapEnd,
                                             maxCas, driftCounter,
                                             failovers);
    fdb_doc_free(statDoc);
    return forestErr2EngineErr(status);
}

void ForestKVStore::updateFileInfo(void) {
    fdb_file_info finfo;
    fdb_status status = fdb_get_file_info(writeDBFileHandle, &finfo);
    if (status == FDB_RESULT_SUCCESS) {
        cachedFileSize = finfo.file_size;
        cachedSpaceUsed = finfo.space_used;
    } else {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::updateFileInfo: Getting file info"
            " failed with error: %s for shard id: %" PRIu16,
            fdb_error_msg(status), configuration.getShardId());
    }
}

bool ForestKVStore::delVBucket(uint16_t vbucket) {

    fdb_kvs_handle* kvsHandle = getKvsHandle(vbucket, handleType::READER);
    fdb_status status;

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::delVBucket: fdb_kvs_close API call failed for "
                "vbucket %" PRIu16 "with error: %s", vbucket, fdb_error_msg(status));
        }
    }

    readHandleMap[vbucket] = nullptr;

    kvsHandle = getKvsHandle(vbucket, handleType::WRITER);

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::delVBucket: fdb_kvs_close API call failed for "
                "vbucket %" PRIu16 "with error: %s", vbucket, fdb_error_msg(status));
        }
    }

    writeHandleMap[vbucket] = nullptr;

    char kvsName[20];
    sprintf(kvsName,"partition%d", vbucket);

    LockHolder lh(writerLock);

    status = fdb_kvs_remove(writeDBFileHandle, kvsName);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::delVBucket: KV Store remove failed for vbucket "
            "%" PRIu16 "with error: %s", vbucket, fdb_error_msg(status));

        /* If the forestdb handle is busy or fails because compaction is
         * running at the same time, reschedule the vbucket deletion.
         * This is a workaround until forestdb supports this */
        if (status == FDB_RESULT_FAIL_BY_COMPACTION ||
            status == FDB_RESULT_HANDLE_BUSY) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::delVBucket: rescheduling vbucket deletion for "
                "vbucket %" PRIu16, vbucket);
            return false;
        }
    }

    updateFileInfo();

    if (cachedVBStates[vbucket]) {
        delete cachedVBStates[vbucket];
    }

    std::string failovers("[{\"id\":0, \"seq\":0}]");
    cachedVBStates[vbucket] = new vbucket_state(vbucket_state_dead, 0, 0, 0, 0,
                                                0, 0, 0, INITIAL_DRIFT,
                                                failovers);

    vbucket_state* state = cachedVBStates[vbucket];
    std::string stateStr = state->toJSON();

    if (!stateStr.empty()) {
        fdb_doc statDoc;
        memset(&statDoc, 0, sizeof(statDoc));
        statDoc.key = kvsName;
        statDoc.keylen = strlen(kvsName);
        statDoc.meta = NULL;
        statDoc.metalen = 0;
        statDoc.body = const_cast<char *>(stateStr.c_str());
        statDoc.bodylen = stateStr.length();
        fdb_status status = fdb_set(writeVbStateHandle, &statDoc);

        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::delVBucket: Failed to "
                    "save vbucket state for vbucket=%" PRIu16" error=%s",
                    vbucket, fdb_error_msg(status));
        }
    }

    cachedValidVBCount--;

    return true;
}

ENGINE_ERROR_CODE ForestKVStore::forestErr2EngineErr(fdb_status errCode) {
    switch (errCode) {
    case FDB_RESULT_SUCCESS:
        return ENGINE_SUCCESS;
    case FDB_RESULT_ALLOC_FAIL:
        return ENGINE_ENOMEM;
    case FDB_RESULT_KEY_NOT_FOUND:
        return ENGINE_KEY_ENOENT;
    case FDB_RESULT_NO_SUCH_FILE:
    case FDB_RESULT_NO_DB_HEADERS:
    default:
        // same as the general error return code of
        // EventuallyPersistentStore::getInternal
        return ENGINE_TMPFAIL;
    }
}

void ForestKVStore::getWithHeader(void* handle, const std::string& key,
                                  uint16_t vb, Callback<GetValue>& cb,
                                  bool fetchDelete) {
    fdb_kvs_handle* kvsHandle = static_cast<fdb_kvs_handle*>(handle);
    if (!kvsHandle) {
        throw std::invalid_argument("ForestKVStore::getWithHeader: "
                                    "KVS Handle is NULL for vbucket id:" +
                                    std::to_string(vb));
    }

    hrtime_t start = gethrtime();
    RememberingCallback<GetValue> *rc =
                       dynamic_cast<RememberingCallback<GetValue> *>(&cb);
    bool getMetaOnly = rc && rc->val.isPartial();
    GetValue rv;
    fdb_status status;

    fdb_doc rdoc;
    memset(&rdoc, 0, sizeof(rdoc));
    rdoc.key = const_cast<char *>(key.c_str());
    rdoc.keylen = key.length();

    if (!getMetaOnly) {
        status = fdb_get(kvsHandle, &rdoc);
    } else {
        status = fdb_get_metaonly(kvsHandle, &rdoc);
    }

    if (status != FDB_RESULT_SUCCESS) {
        if (!getMetaOnly) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::getWithHeader: Failed to retrieve metadata from "
                "database, vbucketId:%d key:%s error:%s\n",
                vb, key.c_str(), fdb_error_msg(status));
        } else {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::getWithHeader: Failed to retrieve key value from database,"
                "vbucketId:%d key:%s error:%s deleted:%s", vb, key.c_str(),
                fdb_error_msg(status), rdoc.deleted ? "yes" : "no");
        }
        ++st.numGetFailure;
    } else {
        rv = docToItem(kvsHandle, &rdoc, vb, getMetaOnly, fetchDelete);
    }

    rdoc.key = NULL;
    fdb_free_block(rdoc.meta);
    fdb_free_block(rdoc.body);

    st.readTimeHisto.add((gethrtime() - start) / 1000);
    if (status == FDB_RESULT_SUCCESS) {
        st.readSizeHisto.add(key.length() + rv.getValue()->getNBytes());
    }

    rv.setStatus(forestErr2EngineErr(status));
    cb.callback(rv);
}

static ForestMetaData forestMetaDecode(const fdb_doc *rdoc) {
    char* metadata = static_cast<char *>(rdoc->meta);
    ForestMetaData forestMetaData;

    memcpy(&forestMetaData.cas, metadata + forestMetaOffset(cas),
           sizeof(forestMetaData.cas));
    memcpy(&forestMetaData.rev_seqno, metadata + forestMetaOffset(rev_seqno),
           sizeof(forestMetaData.rev_seqno));
    memcpy(&forestMetaData.exptime, metadata + forestMetaOffset(exptime),
           sizeof(forestMetaData.exptime));
    memcpy(&forestMetaData.texptime, metadata + forestMetaOffset(texptime),
           sizeof(forestMetaData.texptime));
    memcpy(&forestMetaData.flags, metadata + forestMetaOffset(flags),
           sizeof(forestMetaData.flags));
    memcpy(&forestMetaData.ext_meta, metadata + forestMetaOffset(ext_meta),
           EXT_META_LEN);
    memcpy(&forestMetaData.confresmode, metadata + forestMetaOffset(confresmode),
           CONFLICT_RES_META_LEN);

    forestMetaData.cas = ntohll(forestMetaData.cas);
    forestMetaData.rev_seqno = ntohll(forestMetaData.rev_seqno);
    forestMetaData.exptime = ntohl(forestMetaData.exptime);
    forestMetaData.texptime = ntohl(forestMetaData.texptime);

    return forestMetaData;
}

fdb_changes_decision ForestKVStore::recordChanges(fdb_kvs_handle* handle,
                                                  fdb_doc* doc,
                                                  void* ctx) {

    ScanContext* sctx = static_cast<ScanContext*>(ctx);

    const uint64_t byseqno = doc->seqnum;
    const uint16_t vbucketId = sctx->vbid;

    std::string docKey((char*)doc->key, doc->keylen);
    CacheLookup lookup(docKey, byseqno, vbucketId);

    std::shared_ptr<Callback<CacheLookup> > lookup_cb = sctx->lookup;
    lookup_cb->callback(lookup);

    switch (lookup_cb->getStatus()) {
        case ENGINE_SUCCESS:
            /* Go ahead and create an entry */
            break;
        case ENGINE_KEY_EEXISTS:
            /* Key already exists */
            sctx->lastReadSeqno = byseqno;
            return FDB_CHANGES_CLEAN;
        case ENGINE_ENOMEM:
            /* High memory pressure, cancel execution */
            sctx->logger->log(EXTENSION_LOG_WARNING,
                              "ForestKVStore::recordChanges: "
                              "Out of memory, vbucket: %" PRIu16
                              ", cancelling the iteration!", vbucketId);
            return FDB_CHANGES_CANCEL;
        default:
            std::string err("ForestKVStore::recordChanges: Invalid response: "
                            + std::to_string(lookup_cb->getStatus()));
            throw std::logic_error(err);
    }

    ForestMetaData meta = forestMetaDecode(doc);

    void* valuePtr = nullptr;
    size_t valueLen = 0;

    if (sctx->valFilter != ValueFilter::KEYS_ONLY && !doc->deleted) {
        valuePtr = doc->body;
        valueLen = doc->bodylen;
    }

    Item* it = new Item(doc->key, doc->keylen,
                        meta.flags,
                        meta.exptime,
                        valuePtr, valueLen,
                        meta.ext_meta, EXT_META_LEN,
                        meta.cas,
                        byseqno,
                        vbucketId,
                        meta.rev_seqno);
    if (doc->deleted) {
        it->setDeleted();
    }

    it->setConflictResMode(
                static_cast<enum conflict_resolution_mode>(meta.confresmode));

    bool onlyKeys = (sctx->valFilter == ValueFilter::KEYS_ONLY) ? true : false;
    GetValue rv(it, ENGINE_SUCCESS, -1, onlyKeys);

    std::shared_ptr<Callback<GetValue> > getval_cb = sctx->callback;
    getval_cb->callback(rv);

    switch (getval_cb->getStatus()) {
        case ENGINE_SUCCESS:
            /* Success */
            break;
        case ENGINE_KEY_ENOENT:
            /* If in case of rollback's CB, if an item to delete
               isn't in the in-memory hash table, then an ENOENT
               is returned, is benign. */
            break;
        case ENGINE_ENOMEM:
            /* High memory pressure, cancel execution */
            sctx->logger->log(EXTENSION_LOG_WARNING,
                              "ForestKVStore::recordChanges: "
                              "Out of memory, vbucket: %" PRIu16
                              ", cancelling iteration!", vbucketId);
            return FDB_CHANGES_CANCEL;
        default:
            std::string err("ForestKVStore::recordChanges: "
                            "Unexpected error code: " +
                            std::to_string(getval_cb->getStatus()));
            throw std::logic_error(err);
    }

    sctx->lastReadSeqno = byseqno;
    return FDB_CHANGES_CLEAN;
}

GetValue ForestKVStore::docToItem(fdb_kvs_handle* kvsHandle, fdb_doc* rdoc,
                                  uint16_t vbId, bool metaOnly, bool fetchDelete) {
    ForestMetaData forestMetaData;

    //TODO: handle metadata upgrade?
    forestMetaData = forestMetaDecode(rdoc);

    Item* it = NULL;
    if (metaOnly || (fetchDelete && rdoc->deleted)) {
        it = new Item((char *)rdoc->key, rdoc->keylen, forestMetaData.flags,
                      forestMetaData.exptime, NULL, 0, forestMetaData.ext_meta,
                      EXT_META_LEN, forestMetaData.cas,
                      (uint64_t)rdoc->seqnum, vbId);
        if (rdoc->deleted) {
            it->setDeleted();
        }

        /* update ep-engine IO stat read_bytes */
        st.io_read_bytes += rdoc->keylen;
    } else {
        size_t valuelen = rdoc->bodylen;
        void* valuePtr = rdoc->body;
        uint8_t ext_meta[EXT_META_LEN];

        if (checkUTF8JSON((const unsigned char *)valuePtr, valuelen)) {
            ext_meta[0] = PROTOCOL_BINARY_DATATYPE_JSON;
        } else {
            ext_meta[0] = PROTOCOL_BINARY_RAW_BYTES;
        }

        it = new Item((char *)rdoc->key, rdoc->keylen, forestMetaData.flags,
                      forestMetaData.exptime, valuePtr, valuelen,
                      ext_meta, EXT_META_LEN, forestMetaData.cas,
                      (uint64_t)rdoc->seqnum, vbId);

        /* update ep-engine IO stat read_bytes */
        st.io_read_bytes += (rdoc->keylen + valuelen);
    }

    it->setConflictResMode(
            static_cast<enum conflict_resolution_mode>(forestMetaData.confresmode));
    it->setRevSeqno(forestMetaData.rev_seqno);

    /* increment ep-engine IO stat num_read */
    ++st.io_num_read;

    return GetValue(it);
}

vbucket_state* ForestKVStore::getVBucketState(uint16_t vbucketId) {
    return cachedVBStates[vbucketId];
}

void ForestKVStore::commitCallback(std::vector<ForestRequest *> &committedReqs) {
    size_t commitSize = committedReqs.size();

    for (size_t index = 0; index < commitSize; index++) {
        size_t dataSize = committedReqs[index]->getDataSize();
        size_t keySize = committedReqs[index]->getKey().length();
        /* update ep stats */
        ++st.io_num_write;
        st.io_write_bytes += (keySize + dataSize);

        int rv = committedReqs[index]->getStatus();
        if (committedReqs[index]->isDelete()) {
            if (rv != MUTATION_SUCCESS) {
                ++st.numDelFailure;
            } else {
                st.delTimeHisto.add(committedReqs[index]->getDelta() / 1000);
                st.writeSizeHisto.add(keySize + FORESTDB_METADATA_SIZE);
            }
            committedReqs[index]->getDelCallback()->callback(rv);
        } else {
            if (rv != MUTATION_SUCCESS) {
               ++st.numSetFailure;
            } else {
               st.writeTimeHisto.add(committedReqs[index]->getDelta() / 1000);
               st.writeSizeHisto.add(dataSize + keySize);
            }
            //TODO: For now, all mutations are passed in as insertions.
            //This needs to be revisited in order to update stats.
            mutation_result p(rv, true);
            committedReqs[index]->getSetCallback()->callback(p);
        }
    }
}

bool ForestKVStore::save2forestdb() {
    size_t pendingCommitCnt = pendingReqsQ.size();
    if (pendingCommitCnt == 0) {
        return true;
    }

    uint16_t maxVbuckets = configuration.getMaxVBuckets();
    uint16_t numShards = configuration.getMaxShards();
    uint16_t shardId = configuration.getShardId();

    LockHolder lh(writerLock);

    for (uint16_t vbid = shardId; vbid < maxVbuckets; vbid += numShards) {
        vbucket_state* state = cachedVBStates[vbid];
        if (state != nullptr) {
            std::string stateStr = state->toJSON();
            if (!stateStr.empty()) {
                fdb_doc statDoc;
                memset(&statDoc, 0, sizeof(statDoc));
                char kvsName[20];
                int keylen = snprintf(kvsName, sizeof(kvsName),"partition%d", vbid);
                if (keylen <= 0 || keylen >= static_cast<int>(sizeof(kvsName))) {
                    throw std::runtime_error("ForestKVStore::save2forestdb: "
                                                 "Failed to build partition id");
                }
                statDoc.keylen = static_cast<size_t>(keylen);
                statDoc.key = kvsName;
                statDoc.meta = NULL;
                statDoc.metalen = 0;
                statDoc.body = const_cast<char *>(stateStr.c_str());
                statDoc.bodylen = stateStr.length();
                fdb_status status = fdb_set(writeVbStateHandle, &statDoc);

                if (status != FDB_RESULT_SUCCESS) {
                    throw std::runtime_error("ForestKVStore::save2forestdb: "
                        "Failed to save vbucket state for vbucket id: " +
                        std::to_string(vbid) + " with error: " +
                        std::string(fdb_error_msg(status)));
                }
            }
        }
    }

    hrtime_t start = gethrtime();
    fdb_status status = fdb_commit(writeDBFileHandle, FDB_COMMIT_NORMAL);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::save2forestdb: "
            "fdb_commit failed for shard id: " + std::to_string(shardId) +
            "with error: " + std::string(fdb_error_msg(status)));
    }

    st.commitHisto.add((gethrtime() - start) / 1000);

    for (uint16_t vbId = shardId; vbId < maxVbuckets; vbId += numShards) {
        fdb_kvs_handle* kvsHandle = getOrCreateKvsHandle(vbId, handleType::WRITER);
        fdb_kvs_info kvsInfo;
        fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);

        if (status == FDB_RESULT_SUCCESS) {
            cachedDeleteCount[vbId] = kvsInfo.deleted_count;
            cachedDocCount[vbId] = kvsInfo.doc_count;
            vbucket_state* state = cachedVBStates[vbId];
            if (state) {
                state->highSeqno = kvsInfo.last_seqnum;
            }
        } else {
            throw std::runtime_error("ForestKVStore::save2forestdb: "
                "Failed to get KV store info for vbucket id: " +
                std::to_string(vbId) + " with error: " +
                std::string(fdb_error_msg(status)));
        }
    }

    commitCallback(pendingReqsQ);

    for (size_t i = 0; i < pendingCommitCnt; i++) {
        delete pendingReqsQ[i];
    }

    updateFileInfo();

    pendingReqsQ.clear();

    st.docsCommitted = pendingCommitCnt;

    return true;
}

bool ForestKVStore::commit() {
    if (intransaction) {
        if (save2forestdb()) {
            intransaction = false;
        }
    }

    return !intransaction;
}

StorageProperties ForestKVStore::getStorageProperties(void) {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::Yes,
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::Yes);
    return rv;
}

fdb_kvs_handle* ForestKVStore::openKvsHandle(fdb_file_handle& fileHandle, char *kvsName) {
    fdb_status status;
    fdb_kvs_handle* kvsHandle = nullptr;

    status = fdb_kvs_open(&fileHandle, &kvsHandle, kvsName, &kvsConfig);

    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::createKvsHandle: Failed to "
            "create KVStore handle for partition:" + std::string(kvsName));
    }

    status = fdb_set_log_callback(kvsHandle, errorlog_cb, nullptr);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::createKvsHandle: Setting the log callback for "
                                 "KV Store instance failed with error:" +
                                 std::string(fdb_error_msg(status)));
    }

    return kvsHandle;
}

std::unique_ptr<ForestKvsHandle> ForestKVStore::createKvsHandle(uint16_t vbucketId) {
    std::stringstream dbFile;
    fdb_file_handle* newDBFileHandle = nullptr;
    fdb_kvs_handle* newKvsHandle = nullptr;
    fdb_status status;

    {
        LockHolder lh(handleLock);

        dbFile << dbFileNameStr << "." << dbFileRevNum;

        status = fdb_open(&newDBFileHandle, dbFile.str().c_str(), &fileConfig);
        if (status != FDB_RESULT_SUCCESS) {
            throw std::runtime_error("ForestKVStore::createKvsHandle: Opening a "
                "database file instance failed with error: " +
                std::string(fdb_error_msg(status)));
        }
    }

    char kvsName[20];
    int len = snprintf(kvsName, sizeof(kvsName),"partition%d", vbucketId);
    if (len < 0) {
        throw std::runtime_error("ForestKVStore::createKvsHandle: Formatting "
            "error in generating the KVStore name for vbucket:" +
            std::to_string(vbucketId));
    }

    newKvsHandle = openKvsHandle(*newDBFileHandle, kvsName);

    return std::unique_ptr<ForestKvsHandle>(new ForestKvsHandle(newDBFileHandle,
                                                                newKvsHandle));
}

fdb_kvs_handle* ForestKVStore::getKvsHandle(uint16_t vbucketId, handleType htype) {

    std::unordered_map<uint16_t, fdb_kvs_handle*>::iterator found;

    switch (htype) {
        case handleType::WRITER:
            found = writeHandleMap.find(vbucketId);
            if (found == writeHandleMap.end()) {
                throw std::invalid_argument("ForestKVStore::getKvsHandle: Failed "
                    "to find vb (which is " + std::to_string(vbucketId) +
                    ") in writeHandleMap");
            }
            break;
        case handleType::READER:
            found = readHandleMap.find(vbucketId);
            if (found == readHandleMap.end()) {
                throw std::invalid_argument("ForestKVStore::getKvsHandle: Failed "
                    "to find vb (which is " + std::to_string(vbucketId) +
                    ") in readHandleMap");
            }
            break;
    }

    return found->second;
}

fdb_kvs_handle* ForestKVStore::getOrCreateKvsHandle(uint16_t vbucketId, handleType htype) {

    fdb_kvs_handle* kvsHandle = getKvsHandle(vbucketId, htype);

    if (!kvsHandle) {
        char kvsName[20];
        sprintf(kvsName, "partition%d", vbucketId);
        if (htype == handleType::READER) {
            kvsHandle = openKvsHandle(*readDBFileHandle, kvsName);
            readHandleMap[vbucketId] = kvsHandle;
        } else {
            kvsHandle = openKvsHandle(*writeDBFileHandle, kvsName);
            writeHandleMap[vbucketId] = kvsHandle;
        }
    }

    return kvsHandle;
}

static int8_t getMutationStatus(fdb_status errCode) {
    switch(errCode) {
        case FDB_RESULT_SUCCESS:
            return MUTATION_SUCCESS;
        case FDB_RESULT_NO_DB_HEADERS:
        case FDB_RESULT_NO_SUCH_FILE:
        case FDB_RESULT_KEY_NOT_FOUND:
            return DOC_NOT_FOUND;
        default:
            return MUTATION_FAILED;
    }
}

static void populateMetaData(const Item& itm, uint8_t* meta, bool deletion) {
    uint64_t cas = htonll(itm.getCas());
    uint64_t rev_seqno = htonll(itm.getRevSeqno());
    uint32_t flags = itm.getFlags();
    uint32_t exptime = itm.getExptime();
    uint32_t texptime = 0;
    uint8_t confresmode = static_cast<uint8_t>(itm.getConflictResMode());

    if (deletion) {
        texptime = ep_real_time();
    }

    exptime = htonl(exptime);
    texptime = htonl(texptime);

    memcpy(meta, &cas, sizeof(cas));
    memcpy(meta + forestMetaOffset(rev_seqno), &rev_seqno, sizeof(rev_seqno));
    memcpy(meta + forestMetaOffset(exptime), &exptime, sizeof(exptime));
    memcpy(meta + forestMetaOffset(texptime), &texptime, sizeof(texptime));
    memcpy(meta + forestMetaOffset(flags), &flags, sizeof(flags));

    *(meta + forestMetaOffset(flex_meta)) = FLEX_META_CODE;

    if (deletion) {
        *(meta + forestMetaOffset(ext_meta)) = PROTOCOL_BINARY_RAW_BYTES;
    } else {
        memcpy(meta + forestMetaOffset(ext_meta), itm.getExtMeta(),
               itm.getExtMetaLen());
    }

    memcpy(meta + forestMetaOffset(confresmode), &confresmode,
           CONFLICT_RES_META_LEN);
}

void ForestKVStore::set(const Item& itm, Callback<mutation_result>& cb) {
    if (isReadOnly()) {
        throw std::logic_error("ForestKVStore::set: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("ForestKVStore::set: intransaction must be "
                        "true to perform a set operation.");
    }
    MutationRequestCallback requestcb;

    // each req will be de-allocated after commit
    requestcb.setCb = &cb;
    ForestRequest *req = new ForestRequest(itm, requestcb, false,
                                           FORESTDB_METADATA_SIZE + itm.getNBytes());
    fdb_doc setDoc;
    fdb_kvs_handle* kvsHandle = NULL;
    fdb_status status;
    uint8_t meta[FORESTDB_METADATA_SIZE];

    memset(meta, 0, sizeof(meta));
    populateMetaData(itm, meta, false);

    setDoc.key = const_cast<char *>(itm.getKey().c_str());
    setDoc.keylen = itm.getNKey();
    setDoc.meta = meta;
    setDoc.metalen = sizeof(meta);
    setDoc.body = const_cast<char *>(itm.getData());
    setDoc.bodylen = itm.getNBytes();
    setDoc.deleted = false;

    fdb_doc_set_seqnum(&setDoc, itm.getBySeqno());
    kvsHandle = getOrCreateKvsHandle(req->getVBucketId(), handleType::WRITER);
    status = fdb_set(kvsHandle, &setDoc);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::set: fdb_set failed "
            "for key: %s and vbucketId: %" PRIu16 " with error: %s",
            req->getKey().c_str(), req->getVBucketId(), fdb_error_msg(status));
        req->setStatus(getMutationStatus(status));
    }
    setDoc.body = NULL;
    setDoc.bodylen = 0;

    pendingReqsQ.push_back(req);
}

void ForestKVStore::get(const std::string& key, uint16_t vb,
                        Callback<GetValue>& cb, bool fetchDelete) {
    getWithHeader(getOrCreateKvsHandle(vb, handleType::READER), key, vb,
                  cb, fetchDelete);
}

ENGINE_ERROR_CODE
ForestKVStore::getAllKeys(uint16_t vbid, std::string& start_key, uint32_t count,
                         std::shared_ptr<Callback<uint16_t&, char*&> > cb) {

    std::unique_ptr<ForestKvsHandle> fkvsHandle;
    try {
        fkvsHandle = createKvsHandle(vbid);
    } catch (std::exception& e) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::getAllKeys: creating a new KV handle "
            "failed with error: %s", e.what());
        return ENGINE_FAILED;
    }

    fdb_iterator* fdb_iter = NULL;
    fdb_status status = fdb_iterator_init(fkvsHandle->getKvsHandle(), &fdb_iter,
                                          start_key.c_str(), strlen(start_key.c_str()),
                                          NULL, 0, FDB_ITR_NO_DELETES);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::getAllKeys: iterator "
                   "initalization failed for vbucket id " + std::to_string(vbid) +
                   " and start key:" + start_key.c_str());
    }

    fdb_doc* rdoc = NULL;
    status = fdb_doc_create(&rdoc, NULL, 0, NULL, 0, NULL, 0);
    if (status != FDB_RESULT_SUCCESS) {
       fdb_iterator_close(fdb_iter);
       throw std::runtime_error("ForestKVStore::getAllKeys: creating "
                  "the document failed for vbucket id:" +
                  std::to_string(vbid) + " with error:" +
                  fdb_error_msg(status));
    }

    rdoc->key = malloc(MAX_KEY_LENGTH);
    rdoc->meta = malloc(FORESTDB_METADATA_SIZE);

    for (uint32_t curr_count = 0; curr_count < count; curr_count++) {
        status = fdb_iterator_get_metaonly(fdb_iter, &rdoc);
        if (status != FDB_RESULT_SUCCESS) {
            fdb_doc_free(rdoc);
            fdb_iterator_close(fdb_iter);
            throw std::runtime_error("ForestKVStore::getAllKeys: iterator "
                       "get failed for vbucket id " + std::to_string(vbid) +
                       " and start key:" + start_key.c_str());
        }
        uint16_t keylen = static_cast<uint16_t>(rdoc->keylen);
        char* key = static_cast<char *>(rdoc->key);
        cb->callback(keylen, key);

        if (fdb_iterator_next(fdb_iter) != FDB_RESULT_SUCCESS) {
            break;
        }
    }

    fdb_doc_free(rdoc);
    fdb_iterator_close(fdb_iter);

    return ENGINE_SUCCESS;
}

void ForestKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t& itms) {
    bool meta_only = true;
    vb_bgfetch_queue_t::iterator itr = itms.begin();
    for (; itr != itms.end(); ++itr) {
        vb_bgfetch_item_ctx_t& bg_itm_ctx = (*itr).second;
        meta_only = bg_itm_ctx.isMetaOnly;

        RememberingCallback<GetValue> gcb;
        if (meta_only) {
            gcb.val.setPartial();
        }

        const std::string& key = (*itr).first;
        get(key, vb, gcb);
        ENGINE_ERROR_CODE status = gcb.val.getStatus();
        if (status != ENGINE_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::getMulti: Failed to "
                "retrieve key: %s", key.c_str());
        }

        std::list<VBucketBGFetchItem *>& fetches = bg_itm_ctx.bgfetched_list;
        std::list<VBucketBGFetchItem *>:: iterator fitr = fetches.begin();

        for (fitr = fetches.begin(); fitr != fetches.end(); ++fitr) {
            (*fitr)->value = gcb.val;
            st.readTimeHisto.add((gethrtime() - (*fitr)->initTime) / 1000);
        }

        if (status == ENGINE_SUCCESS) {
            st.readSizeHisto.add(gcb.val.getValue()->getNKey() +
                                 gcb.val.getValue()->getNBytes());
        }
    }
}

void ForestKVStore::del(const Item& itm, Callback<int>& cb) {
    if (isReadOnly()) {
        throw std::logic_error("ForestKVStore::del: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("ForestKVStore::del: intransaction must be "
                        "true to perform a delete operation.");
    }
    MutationRequestCallback requestcb;
    requestcb.delCb = &cb;
    ForestRequest* req = new ForestRequest(itm, requestcb, true);
    fdb_doc delDoc;
    fdb_kvs_handle* kvsHandle = NULL;
    fdb_status status;
    uint8_t meta[FORESTDB_METADATA_SIZE];

    memset(meta, 0, sizeof(meta));
    populateMetaData(itm, meta, true);

    delDoc.key = const_cast<char *>(itm.getKey().c_str());
    delDoc.keylen = itm.getNKey();
    delDoc.meta = meta;
    delDoc.metalen = sizeof(meta);
    delDoc.deleted = true;

    fdb_doc_set_seqnum(&delDoc, itm.getBySeqno());
    kvsHandle = getOrCreateKvsHandle(req->getVBucketId(), handleType::WRITER);
    status = fdb_del(kvsHandle, &delDoc);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "ForesKVStore::del: fdb_del failed "
            "for key: %s and vbucketId: %" PRIu16 " with error: %s",
            req->getKey().c_str(), req->getVBucketId(), fdb_error_msg(status));
        req->setStatus(getMutationStatus(status));
    }

    pendingReqsQ.push_back(req);
}

std::vector<vbucket_state *> ForestKVStore::listPersistedVbuckets(void) {
    return cachedVBStates;
}

size_t ForestKVStore::getNumPersistedDeletes(uint16_t vbid) {
    size_t delCount = cachedDeleteCount[vbid];
    if (delCount != (size_t) -1) {
        return delCount;
    }

    std::unique_ptr<ForestKvsHandle> fkvsHandle;
    try {
        fkvsHandle = createKvsHandle(vbid);
    } catch (std::exception& e) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::getNumPersistedDeletes: creating "
            "a new forestdb handle failed with error: %s", e.what());
        return 0;
    }

    fdb_kvs_info kvsInfo;
    fdb_status status = fdb_get_kvs_info(fkvsHandle->getKvsHandle(), &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::getNumPersistedDeletes:Failed to "
            "retrieve KV store info with error:" +
            std::string(fdb_error_msg(status)) + " for vbucket id:" +
            std::to_string(static_cast<int>(vbid)));
        throw std::runtime_error(err);
    }

    return kvsInfo.deleted_count;
}

DBFileInfo ForestKVStore::getDbFileInfo(uint16_t vbId) {
    DBFileInfo dbInfo;

    /* In order to get the file size and space used for a particular
     * vbucket on disk, the total file size and space usage for the
     * shard file needs to be divided by the total valid vbucket count and
     * the default KV Store that accounts for the vbucket state information
     */
    dbInfo.fileSize =  cachedFileSize.load()/(cachedValidVBCount.load() + 1);
    dbInfo.spaceUsed = cachedSpaceUsed.load()/(cachedValidVBCount.load() + 1);

    return dbInfo;
}

DBFileInfo ForestKVStore::getAggrDbFileInfo() {
    DBFileInfo dbInfo;

    dbInfo.fileSize = cachedFileSize.load();
    dbInfo.spaceUsed = cachedSpaceUsed.load();

    return dbInfo;
}

bool ForestKVStore::snapshotVBucket(uint16_t vbucketId, vbucket_state& vbstate,
                                    VBStatePersist options) {

    hrtime_t start = gethrtime();

    LockHolder lh(writerLock);

    if (updateCachedVBState(vbucketId, vbstate) &&
         (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
          options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT)) {
        std::string stateStr = cachedVBStates[vbucketId]->toJSON();
        char keybuf[20];
        fdb_doc statDoc;
        memset(&statDoc, 0, sizeof(statDoc));
        sprintf(keybuf, "partition%d", vbucketId);
        statDoc.key = keybuf;
        statDoc.keylen = strlen(keybuf);
        statDoc.meta = NULL;
        statDoc.metalen = 0;
        statDoc.body = const_cast<char *>(stateStr.c_str());
        statDoc.bodylen = stateStr.length();

        fdb_status status = fdb_set(writeVbStateHandle, &statDoc);

        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::snapshotVBucket: Failed to "
                "save vbucket state for vbucket: %" PRIu16 " with error: %s",
                vbucketId, fdb_error_msg(status));
            return false;
        }

        if (options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT) {
            status = fdb_commit(writeDBFileHandle, FDB_COMMIT_NORMAL);

            if (status != FDB_RESULT_SUCCESS) {
                LOG(EXTENSION_LOG_WARNING, "ForestKVStore::snapshotVBucket: Failed "
                    "to commit vbucket state for vbucket: %" PRIu16 " with error: %s",
                    vbucketId, fdb_error_msg(status));
                return false;
            }
        }

        updateFileInfo();
    }

    st.snapshotHisto.add((gethrtime() - start) / 1000);

    return true;
}

ScanContext* ForestKVStore::initScanContext(std::shared_ptr<Callback<GetValue> > cb,
                                           std::shared_ptr<Callback<CacheLookup> > cl,
                                           uint16_t vbid, uint64_t startSeqno,
                                           DocumentFilter options,
                                           ValueFilter valOptions) {
    std::unique_ptr<ForestKvsHandle> fkvsHandle;
    try {
        fkvsHandle = createKvsHandle(vbid);
    } catch (std::exception& e) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::initScanContext: creating a "
            "new forestdb kvstore handle failed with error: %s", e.what());
        return nullptr;
    }

    fdb_kvs_info kvsInfo;
    fdb_status status = fdb_get_kvs_info(fkvsHandle->getKvsHandle(), &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::initScanContext: Failed to retrieve "
            "KV store info with error:" + std::string(fdb_error_msg(status)) +
            "vbucket id:" + std::to_string(static_cast<int>(vbid)));
        throw std::runtime_error(err);
    }

    size_t scanId = scanCounter++;
    size_t count = getNumItems(fkvsHandle->getKvsHandle(),
                               startSeqno,
                               std::numeric_limits<uint64_t>::max());

    {
        LockHolder lh(scanLock);
        scans[scanId] = std::move(fkvsHandle);
    }

    return new ScanContext(cb, cl, vbid, scanId, startSeqno,
                           (uint64_t)kvsInfo.last_seqnum, options,
                           valOptions, count);
}

scan_error_t ForestKVStore::scan(ScanContext* ctx) {
    if (!ctx) {
        return scan_failed;
    }

    if (ctx->lastReadSeqno == ctx->maxSeqno) {
        return scan_success;
    }

    fdb_kvs_handle* kvsHandle = nullptr;
    {
        LockHolder lh(scanLock);
        auto itr = scans.find(ctx->scanId);
        if (itr == scans.end()) {
            return scan_failed;
        }

        kvsHandle = (itr->second)->getKvsHandle();
    }

    fdb_iterator_opt_t options;

    switch (ctx->docFilter) {
        case DocumentFilter::NO_DELETES:
            options = FDB_ITR_NO_DELETES;
            break;
        case DocumentFilter::ALL_ITEMS:
            options = FDB_ITR_NONE;
            break;
        default:
            std::string err("ForestKVStore::scan: Illegal document filter!" +
                            std::to_string(static_cast<int>(ctx->docFilter)));
            throw std::logic_error(err);
    }

    switch (ctx->valFilter) {
        case ValueFilter::KEYS_ONLY:
            options |= FDB_ITR_NO_VALUES;
            break;
        case ValueFilter::VALUES_COMPRESSED:
            // TODO: NOT SUPPORTED YET (MB-19682)
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::scan: "
                "Getting compressed data - Not supported yet with forestdb");
            return scan_failed;
        case ValueFilter::VALUES_DECOMPRESSED:
            break;
        default:
            std::string err("ForestKVStore::scan: Illegal value filter!" +
                            std::to_string(static_cast<int>(ctx->valFilter)));
            throw std::logic_error(err);
    }

    fdb_seqnum_t start = ctx->startSeqno;
    if (ctx->lastReadSeqno != 0) {
        start = static_cast<fdb_seqnum_t>(ctx->lastReadSeqno) + 1;
    }

    fdb_status status = fdb_changes_since(kvsHandle, start, options,
            recordChanges,
            static_cast<void*>(ctx));

    switch (status) {
        case FDB_RESULT_SUCCESS:
            /* Success */
            break;
        case FDB_RESULT_CANCELLED:
            /* Retry */
            return scan_again;
        default:
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::scan: "
                    "fdb_changes_since api failed, error: %s",
                    fdb_error_msg(status));
            return scan_failed;
    }

    return scan_success;
}

void ForestKVStore::destroyScanContext(ScanContext* ctx) {
    if (!ctx) {
        return;
    }

    LockHolder lh(scanLock);
    auto itr = scans.find(ctx->scanId);
    if (itr != scans.end()) {
        scans.erase(itr);
    }

    delete ctx;
}

fdb_compact_decision ForestKVStore::compaction_cb(fdb_file_handle* fhandle,
                                                  fdb_compaction_status comp_status,
                                                  const char* kv_name,
                                                  const fdb_doc* doc, uint64_t old_offset,
                                                  uint64_t new_offset, void* ctx) {
    /**
     * The default KV store holds the vbucket state information for all the vbuckets
     * in the shard.
     */
    if (strcmp(kv_name, "default") == 0) {
        return FDB_CS_KEEP_DOC;
    }

    compaction_ctx* comp_ctx = reinterpret_cast<compaction_ctx *>(ctx);
    ForestKVStore* store = reinterpret_cast<ForestKVStore *>(comp_ctx->store);

    /* Every KV store name for ep-engine has the format "partition<vbucket id>."
     * Extract the vbucket id from the kvstore name */
    std::string kvNameStr(kv_name);
    kvNameStr.erase(kvNameStr.begin(), kvNameStr.begin() + strlen("partition"));

    uint16_t vbid;
    try {
        vbid = std::stoi(kvNameStr);
    } catch (...) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compaction_cb: Failed to convert string to integer "
            "for KV store name: %s", kv_name);
        return FDB_CS_KEEP_DOC;
    }

    std::unique_ptr<ForestKvsHandle> fkvsHandle;
    try {
        fkvsHandle = store->createKvsHandle(vbid);
    } catch (std::exception& e) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compaction_cb: Failed to retrieve handle for "
            "vbucket %" PRIu16" with error: %s", vbid, e.what());
        return FDB_CS_KEEP_DOC;
    }

    fdb_kvs_info kvsInfo;
    fdb_status status = fdb_get_kvs_info(fkvsHandle->getKvsHandle(), &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compaction_cb: Failed to retrieve KV store information for "
            "vbucket %" PRIu16 " with error: %s", vbid, fdb_error_msg(status));
        return FDB_CS_KEEP_DOC;
    }

    ForestMetaData forestMetaData = forestMetaDecode(doc);

    std::string key((char *)doc->key, doc->keylen);
    if (doc->deleted) {
        uint64_t max_purge_seq = 0;
        auto it = comp_ctx->max_purged_seq.find(vbid);

        if (it == comp_ctx->max_purged_seq.end()) {
            comp_ctx->max_purged_seq[vbid] = 0;
        } else {
            max_purge_seq = it->second;
        }

        if (doc->seqnum != kvsInfo.last_seqnum) {
            if (comp_ctx->drop_deletes) {
                if (max_purge_seq < doc->seqnum) {
                    comp_ctx->max_purged_seq[vbid] = doc->seqnum;
                }

                return FDB_CS_DROP_DOC;
            }

            if (forestMetaData.texptime < comp_ctx->purge_before_ts &&
                    (!comp_ctx->purge_before_seq ||
                     doc->seqnum <= comp_ctx->purge_before_seq)) {
                if (max_purge_seq < doc->seqnum) {
                    comp_ctx->max_purged_seq[vbid] = doc->seqnum;
                }

                return FDB_CS_DROP_DOC;
            }
        }
    } else if (forestMetaData.exptime) {
        std::vector<vbucket_state *> vbStates = store->listPersistedVbuckets();
        vbucket_state* vbState = vbStates[vbid];
        if (vbState && vbState->state == vbucket_state_active) {
            time_t curr_time = ep_real_time();
            if (forestMetaData.exptime < curr_time) {
                comp_ctx->expiryCallback->callback(vbid, key,
                                                   forestMetaData.rev_seqno,
                                                   curr_time);
            }
        }
    }

    if (comp_ctx->bloomFilterCallback) {
        bool deleted = doc->deleted;
        comp_ctx->bloomFilterCallback->callback(vbid, key, deleted);
    }

    return FDB_CS_KEEP_DOC;
}

bool ForestKVStore::compactDB(compaction_ctx* ctx) {
    hrtime_t start = gethrtime();
    uint16_t shardId = ctx->db_file_id;

    std::string dbFileBase = dbname + "/" + std::to_string(shardId) + ".fdb.";

    std::string prevDbFile = dbFileBase + std::to_string(dbFileRevNum);

    std::string newDbFile = dbFileBase + std::to_string(dbFileRevNum + 1);

    fileConfig.compaction_cb = compaction_cb_c;
    fileConfig.compaction_cb_ctx = ctx;
    fileConfig.compaction_cb_mask = FDB_CS_MOVE_DOC;

    ctx->store = this;

    fdb_file_handle* compactFileHandle = NULL;
    fdb_status status = fdb_open(&compactFileHandle, prevDbFile.c_str(),
                                 &fileConfig);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compactDB: Failed to open database file: %s"
            " with error: %s", prevDbFile.c_str(), fdb_error_msg(status));
        return false;
    }

    status = fdb_compact(compactFileHandle, newDbFile.c_str());

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compactDB: Failed to compact from database file: %s"
            " to database file: %s with error: %s", prevDbFile.c_str(), newDbFile.c_str(),
            fdb_error_msg(status));
        fdb_close(compactFileHandle);
        return false;
    }

    {
        LockHolder lh(handleLock);

        fdb_close(compactFileHandle);

        /* Compaction is complete at this point. Switch to the new file */
        ++dbFileRevNum;
    }

    st.compactHisto.add((gethrtime() - start) / 1000);

    fdb_file_handle* newDBFileHandle = nullptr;
    fdb_file_info fileInfo;

    status = fdb_open(&newDBFileHandle, newDbFile.c_str(), &fileConfig);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compactDB: Opening a new file handle after compaction "
            "failed with error: %s", fdb_error_msg(status));
    }
    status = fdb_get_file_info(newDBFileHandle, &fileInfo);
    if (status == FDB_RESULT_SUCCESS) {
        cachedFileSize = fileInfo.file_size;
        cachedSpaceUsed = fileInfo.space_used;
    } else {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compactDB: Getting file information failed after compaction "
            "with error: %s", fdb_error_msg(status));
    }

    fdb_close(newDBFileHandle);

    return true;
}

size_t ForestKVStore::getNumItems(uint16_t vbid, uint64_t min_seq,
                                  uint64_t max_seq) {
    fdb_kvs_handle* kvsHandle = NULL;
    kvsHandle = getOrCreateKvsHandle(vbid, handleType::READER);
    return getNumItems(kvsHandle, min_seq, max_seq);
}

size_t ForestKVStore::getNumItems(fdb_kvs_handle* kvsHandle,
                                  uint64_t min_seq,
                                  uint64_t max_seq) {
    // TODO: Replace this API's content with fdb_changes_count(),
    // needs MB-16563.
    size_t totalCount = 0;
    fdb_iterator* fdb_iter = nullptr;
    fdb_status status = fdb_iterator_sequence_init(kvsHandle, &fdb_iter, min_seq,
                                                   max_seq, FDB_ITR_NONE);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::getNumItems: ForestDB iterator "
            "initialization failed with error: " +
             std::string(fdb_error_msg(status)));
        throw std::runtime_error(err);
    }

    do {
        totalCount++;
    } while (fdb_iterator_next(fdb_iter) == FDB_RESULT_SUCCESS);

    fdb_iterator_close(fdb_iter);

    return totalCount;
}

size_t ForestKVStore::getItemCount(uint16_t vbid) {
    if (cachedDocCount.at(vbid) == static_cast<size_t>(-1)) {
        std::unique_ptr<ForestKvsHandle> fkvsHandle;
        fdb_status status;
        fdb_kvs_info kvsInfo;

        try {
            fkvsHandle = createKvsHandle(vbid);
        } catch (std::exception& e) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::getItemCount: creating a ForestDB "
                "KV store handle failed with error: %s", e.what());
            return 0;
        }

        status = fdb_get_kvs_info(fkvsHandle->getKvsHandle(), &kvsInfo);
        if (status != FDB_RESULT_SUCCESS) {
            std::string err("ForestKVStore::getItemCount::Failed to retrieve "
                "KV store info with error:" + std::string(fdb_error_msg(status)) +
                " vbucket id:" + std::to_string(static_cast<int>(vbid)));
            throw std::runtime_error(err);
        }

        cachedDocCount[vbid] = kvsInfo.doc_count;
    }

    return cachedDocCount.at(vbid);
}

RollbackResult ForestKVStore::rollback(uint16_t vbid, uint64_t rollbackSeqno,
                                       std::shared_ptr<RollbackCB> cb) {
    fdb_kvs_handle* kvsHandle = NULL;
    fdb_kvs_info kvsInfo;

    kvsHandle = getOrCreateKvsHandle(vbid, handleType::WRITER);

    fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::rollback: "
            "Failed to retrieve KV store info with error: %s for "
            "vbucket: %" PRIu16 " and rollback sequence number: %" PRIu64,
            fdb_error_msg(status), vbid, rollbackSeqno);
        return RollbackResult(false, 0, 0 ,0);
    }

    // Get closest available sequence number to rollback to
    uint64_t currentSeqno = fdb_get_available_rollback_seq(kvsHandle,
                                                           rollbackSeqno);
    if (currentSeqno == 0) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::rollback: "
            "Unable to find an available rollback sequence number "
            "for vbucket: %" PRIu16 " with rollback request sequence number:"
            " %" PRIu64, vbid, rollbackSeqno);
        return RollbackResult(false, 0, 0, 0);
    }

    // Create a new snap handle for the persisted snapshot up till the
    // point of rollback. This snapshot is needed to identify the earlier
    // revision of the items that are being rolled back.
    fdb_kvs_handle* snaphandle = NULL;
    status = fdb_snapshot_open(kvsHandle, &snaphandle, currentSeqno);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::rollback: "
            "Failed to retrieve persisted snapshot handle from the "
            "kvs handle, error: %s for "
            "vbucket: %" PRIu16 " and snapshot sequence number: %" PRIu64,
            fdb_error_msg(status), vbid, currentSeqno + 1);
        return RollbackResult(false, 0, 0 ,0);
    }

    // Set snaphandle as the callback's handle
    cb->setDbHeader(snaphandle);

    std::shared_ptr<Callback<CacheLookup> > cl(new NoLookupCallback);
    ScanContext* ctx = initScanContext(cb, cl, vbid, currentSeqno,
                                       DocumentFilter::ALL_ITEMS,
                                       ValueFilter::KEYS_ONLY);
    scan_error_t error = scan(ctx);
    destroyScanContext(ctx);

    // Close snap handle
    fdb_kvs_close(snaphandle);

    if (error != scan_success) {
        return RollbackResult(false, 0, 0, 0);
    }

    // Initiate disk rollback
    status = fdb_rollback(&kvsHandle, currentSeqno);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::rollback: "
            "ForestDB rollback failed on vbucket: %" PRIu16 " and rollback "
            "sequence number: %" PRIu64 "with error: %s\n",
            vbid, currentSeqno, fdb_error_msg(status));
        return RollbackResult(false, 0, 0, 0);
    }

    status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::rollback: "
            "Failed to retrieve KV store info after rollback with error: %s "
            "for vbucket: %" PRIu16 " and rollback sequence number: %" PRIu64,
            fdb_error_msg(status), vbid, rollbackSeqno);
        return RollbackResult(false, 0, 0 ,0);
    }

    writeHandleMap[vbid] = kvsHandle;

    readVBState(vbid);

    cachedDocCount[vbid] = kvsInfo.doc_count;
    cachedDeleteCount[vbid] = kvsInfo.deleted_count;

    vbucket_state *vb_state = cachedVBStates[vbid];
    return RollbackResult(true, vb_state->highSeqno,
                          vb_state->lastSnapStart, vb_state->lastSnapEnd);
}

extern "C" {
    static fdb_fileops_handle ffs_constructor(void *ctx);
    static fdb_status ffs_open(const char *pathname, fdb_fileops_handle *, int flags,
                               mode_t mode);
    static fdb_ssize_t ffs_pwrite(fdb_fileops_handle fops_handle, void *buf,
                                  size_t count, cs_off_t offset);
    static fdb_ssize_t ffs_pread(fdb_fileops_handle fops_handle, void *buf,
                                 size_t count, cs_off_t offset);
    static int ffs_close(fdb_fileops_handle fops_handle);
    static cs_off_t ffs_goto_eof(fdb_fileops_handle fops_handle);
    static cs_off_t ffs_file_size(fdb_fileops_handle fops_handle, const char *filename);
    static int ffs_fdatasync(fdb_fileops_handle fops_handle);
    static int ffs_fsync(fdb_fileops_handle fops_handle);
    static void ffs_get_errno_str(fdb_fileops_handle fops_handle, char *buf, size_t size);
    static void* ffs_mmap(fdb_fileops_handle fops_handle,
                          size_t length, void **aux);
    static int ffs_munmap(fdb_fileops_handle fops_handle,
                          void *addr, size_t length, void *aux);

    static int ffs_aio_init(fdb_fileops_handle fops_handle, struct async_io_handle *aio_handle);
    static int ffs_aio_prep_read(fdb_fileops_handle fops_handle,
                                 struct async_io_handle *aio_handle, size_t aio_idx,
                                 size_t read_size, uint64_t offset);
    static int ffs_aio_submit(fdb_fileops_handle fops_handle,
                              struct async_io_handle *aio_handle, int num_subs);
    static int ffs_aio_getevents(fdb_fileops_handle fops_handle,
                                 struct async_io_handle *aio_handle, int min,
                                 int max, unsigned int timeout);
    static int ffs_aio_destroy(fdb_fileops_handle fops_handle,
                               struct async_io_handle *aio_handle);
    static int ffs_get_fs_type(fdb_fileops_handle src_fops_handle);
    static int ffs_copy_file_range(int fs_type, fdb_fileops_handle src_fops_handle,
                                   fdb_fileops_handle dst_fops_handle,
                                   uint64_t src_off, uint64_t dst_off, uint64_t len);
    static void ffs_destructor(fdb_fileops_handle fops_handle);
}

fdb_filemgr_ops_t ForestKVStore::getForestStatOps(FileStats* stats) {
    fdb_filemgr_ops_t fileOps = {
        ffs_constructor,
        ffs_open,
        ffs_pwrite,
        ffs_pread,
        ffs_close,
        ffs_goto_eof,
        ffs_file_size,
        ffs_fdatasync,
        ffs_fsync,
        ffs_get_errno_str,
        ffs_mmap,
        ffs_munmap,
        ffs_aio_init,
        ffs_aio_prep_read,
        ffs_aio_submit,
        ffs_aio_getevents,
        ffs_aio_destroy,
        ffs_get_fs_type,
        ffs_copy_file_range,
        ffs_destructor,
        stats,
    };

    return fileOps;
}

struct ForestStatFile {
    ForestStatFile(fdb_filemgr_ops_t *_orig_ops, fdb_fileops_handle _orig_handle,
                   cs_off_t _last_offs) {
        orig_ops = _orig_ops;
        orig_handle = _orig_handle;
        last_offs = _last_offs;
    }
    FileStats *fs_stats;
    const fdb_filemgr_ops_t *orig_ops;
    fdb_fileops_handle orig_handle;
    cs_off_t last_offs;
};

extern "C" {
    static fdb_fileops_handle ffs_constructor(void *ctx) {
        fdb_filemgr_ops_t *orig_ops = fdb_get_default_file_ops();
        ForestStatFile *fsf = new ForestStatFile(orig_ops,
                                                 orig_ops->constructor(orig_ops->ctx), 0);
        fsf->fs_stats = static_cast<FileStats *>(ctx);
        return reinterpret_cast<fdb_fileops_handle>(fsf);
    }

    static fdb_status ffs_open(const char *pathname, fdb_fileops_handle *fops_handle,
                               int flags, mode_t mode) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(*fops_handle);
        if (fsf) {
            return fsf->orig_ops->open(pathname, &fsf->orig_handle, flags, mode);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static fdb_ssize_t ffs_pwrite(fdb_fileops_handle fops_handle, void *buf,
                                  size_t count, cs_off_t offset) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            fsf->fs_stats->writeSizeHisto.add(count);
            BlockTimer bt(&fsf->fs_stats->writeTimeHisto);
            ssize_t result = fsf->orig_ops->pwrite(fsf->orig_handle, buf, count,
                                                   offset);
            if (result > 0) {
                fsf->fs_stats->totalBytesWritten += result;
            }

            return result;
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static fdb_ssize_t ffs_pread(fdb_fileops_handle fops_handle, void *buf,
                                 size_t count, cs_off_t offset) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);

        if (fsf) {
            fsf->fs_stats->readSizeHisto.add(count);
            if (fsf->last_offs) {
                fsf->fs_stats->readSeekHisto.add(std::abs(offset - fsf->last_offs));
            }

            fsf->last_offs = offset;
            BlockTimer bt(&fsf->fs_stats->readTimeHisto);
            ssize_t result = fsf->orig_ops->pread(fsf->orig_handle, buf, count,
                                                  offset);
            if (result) {
                fsf->fs_stats->totalBytesRead += result;
            }

            return result;
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_close(fdb_fileops_handle fops_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->close(fsf->orig_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static cs_off_t ffs_goto_eof(fdb_fileops_handle fops_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->goto_eof(fsf->orig_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static cs_off_t ffs_file_size(fdb_fileops_handle fops_handle, const char *filename) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->file_size(fsf->orig_handle, filename);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_fdatasync(fdb_fileops_handle fops_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->fdatasync(fsf->orig_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_fsync(fdb_fileops_handle fops_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            BlockTimer bt(&fsf->fs_stats->syncTimeHisto);
            return fsf->orig_ops->fsync(fsf->orig_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static void ffs_get_errno_str(fdb_fileops_handle fops_handle, char *buf, size_t size) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            fsf->orig_ops->get_errno_str(fsf->orig_handle, buf, size);
        }
    }

    static void* ffs_mmap(fdb_fileops_handle fops_handle,
                          size_t length, void **aux) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->mmap(fsf->orig_handle, length, aux);
        }
        return nullptr;
    }

    static int ffs_munmap(fdb_fileops_handle fops_handle,
                          void *addr, size_t length, void *aux) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->munmap(fsf->orig_handle, addr, length, aux);
        }
        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_aio_init(fdb_fileops_handle fops_handle,
                            struct async_io_handle *aio_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->aio_init(fsf->orig_handle, aio_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_aio_prep_read(fdb_fileops_handle fops_handle,
                                 struct async_io_handle *aio_handle,
                                 size_t aio_idx, size_t read_size,
                                 uint64_t offset) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->aio_prep_read(fsf->orig_handle, aio_handle,
                                                aio_idx, read_size, offset);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_aio_submit(fdb_fileops_handle fops_handle,
                              struct async_io_handle *aio_handle,
                              int num_subs) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->aio_submit(fsf->orig_handle, aio_handle,
                                             num_subs);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_aio_getevents(fdb_fileops_handle fops_handle,
                                 struct async_io_handle *aio_handle,
                                 int min, int max, unsigned int timeout) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->aio_getevents(fsf->orig_handle,
                                                aio_handle, min, max, timeout);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_aio_destroy(fdb_fileops_handle fops_handle,
                               struct async_io_handle *aio_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        if (fsf) {
            return fsf->orig_ops->aio_destroy(fsf->orig_handle,
                                              aio_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_get_fs_type(fdb_fileops_handle src_fileops_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(src_fileops_handle);
        if (fsf) {
            return fsf->orig_ops->get_fs_type(fsf->orig_handle);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static int ffs_copy_file_range(int fs_type,
                                   fdb_fileops_handle src_fops_handle,
                                   fdb_fileops_handle dst_fops_handle,
                                   uint64_t src_off,
                                   uint64_t dst_off,
                                   uint64_t len) {
        ForestStatFile *src_fsf = reinterpret_cast<ForestStatFile *>(src_fops_handle);
        ForestStatFile *dst_fsf = reinterpret_cast<ForestStatFile *>(dst_fops_handle);

        if (src_fsf && dst_fsf) {
            return src_fsf->orig_ops->copy_file_range(fs_type, src_fsf->orig_handle,
                                                      dst_fsf->orig_handle,
                                                      src_off, dst_off, len);
        }

        return FDB_RESULT_INVALID_ARGS;
    }

    static void ffs_destructor(fdb_fileops_handle fops_handle) {
        ForestStatFile *fsf = reinterpret_cast<ForestStatFile *>(fops_handle);
        fsf->orig_ops->destructor(fsf->orig_handle);
        delete fsf;
    }
}
