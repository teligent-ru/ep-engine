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

#ifndef SRC_VBUCKETMAP_H_
#define SRC_VBUCKETMAP_H_ 1

#include "config.h"

#include <vector>

#include "kvshard.h"
#include "vbucket.h"
#include "utility.h"

/**
 * A map of known vbuckets.
 */
class VBucketMap {
friend class EventuallyPersistentStore;
friend class Warmup;

public:

    // This class uses the same id_type as VBucket
    typedef VBucket::id_type id_type;

    VBucketMap(Configuration &config, EventuallyPersistentStore &store);
    ~VBucketMap();

    ENGINE_ERROR_CODE addBucket(const RCPtr<VBucket> &b);
    void removeBucket(id_type id);
    void addBuckets(const std::vector<VBucket*> &newBuckets);
    RCPtr<VBucket> getBucket(id_type id) const;

    // Returns the size of the map, i.e. the total number of VBuckets it can
    // contain.
    id_type getSize() const;
    std::vector<id_type> getBuckets(void) const;
    std::vector<id_type> getBucketsSortedByState(void) const;
    std::vector<std::pair<id_type, size_t> > getActiveVBucketsSortedByChkMgrMem(void) const;
    bool isBucketDeletion(id_type id) const;
    bool setBucketDeletion(id_type id, bool delBucket);
    bool isBucketCreation(id_type id) const;
    bool setBucketCreation(id_type id, bool rv);
    uint64_t getPersistenceCheckpointId(id_type id) const;
    void setPersistenceCheckpointId(id_type id, uint64_t checkpointId);
    uint64_t getPersistenceSeqno(id_type id) const;
    void setPersistenceSeqno(id_type id, uint64_t seqno);
    KVShard* getShardByVbId(id_type id) const;
    KVShard* getShard(KVShard::id_type shardId) const;
    size_t getNumShards() const;

private:

    std::vector<KVShard*> shards;
    AtomicValue<bool> *bucketDeletion;
    AtomicValue<bool> *bucketCreation;
    AtomicValue<uint64_t> *persistenceCheckpointIds;
    AtomicValue<uint64_t> *persistenceSeqnos;
    const id_type size;

    DISALLOW_COPY_AND_ASSIGN(VBucketMap);
};

#endif  // SRC_VBUCKETMAP_H_
