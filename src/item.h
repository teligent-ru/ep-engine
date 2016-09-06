/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#ifndef SRC_ITEM_H_
#define SRC_ITEM_H_

#include "config.h"

#include <memcached/engine.h>
#include <stdio.h>
#include <string.h>

#include <cstring>
#include <string>

#include "atomic.h"
#include "compress.h"
#include "ep_time.h"
#include "locks.h"
#include "objectregistry.h"
#include "stats.h"

enum queue_operation {
    queue_op_set,
    queue_op_del,
    queue_op_flush,
    queue_op_empty,
    queue_op_checkpoint_start,
    queue_op_checkpoint_end
};

// Max Value for NRU bits
const uint8_t MAX_NRU_VALUE = 3;
// Initial value for NRU bits
const uint8_t INITIAL_NRU_VALUE = 2;
//Min value for NRU bits
const uint8_t MIN_NRU_VALUE = 0;

/**
 * A blob is a minimal sized storage for data up to 2^32 bytes long.
 */
class Blob : public RCValue {
public:

    // Constructors.

    /**
     * Create a new Blob holding the given data.
     *
     * @param start the beginning of the data to copy into this blob
     * @param len the amount of data to copy in
     * @param ext_meta pointer to the extended meta section to be added
     * @param ext_len length of the extended meta section
     *
     * @return the new Blob instance
     */
    static Blob* New(const char *start, const size_t len, uint8_t *ext_meta,
                     uint8_t ext_len) {
        size_t total_len = len + sizeof(Blob) + FLEX_DATA_OFFSET + ext_len;
        Blob *t = new (::operator new(total_len)) Blob(start, len, ext_meta,
                                                       ext_len);
        return t;
    }

    /**
     * Create a new Blob of the given size, with ext_meta set to the specified
     * extended metadata
     *
     * @param len the size of the blob
     * @param ext_meta pointer to the extended meta section to be copied in.
     * @param ext_len length of the extended meta section
     *
     * @return the new Blob instance
     */
    static Blob* New(const size_t len, uint8_t *ext_meta, uint8_t ext_len) {
        size_t total_len = len + sizeof(Blob) + FLEX_DATA_OFFSET + ext_len;
        Blob *t = new (::operator new(total_len)) Blob(NULL, len, ext_meta,
                                                       ext_len);
        return t;
    }

    /**
     * Create a new Blob of the given size.
     * (Used for appends/prepends)
     *
     * @param len the size of the blob
     * @param ext_len length of the extended meta section
     *
     * @return the new Blob instance
     */
    static Blob* New(const size_t len, uint8_t ext_len) {
        size_t total_len = len + sizeof(Blob) + FLEX_DATA_OFFSET + ext_len;
        Blob *t = new (::operator new(total_len)) Blob(len, ext_len);
        return t;
    }

    /**
     * Creates an exact copy of the specified Blob.
     */
    static Blob* Copy(const Blob& other) {
        Blob *t = new (::operator new(other.getSize())) Blob(other);
        return t;
    }

    // Actual accessorish things.

    /**
     * Get the pointer to the contents of the Value part of this Blob.
     */
    const char* getData() const {
        return data + FLEX_DATA_OFFSET + extMetaLen;
    }

    /**
     * Get the pointer to the contents of Blob.
     */
    const char* getBlob() const {
        return data;
    }

    /**
     * Return datatype stored in Value Blob.
     */
    const protocol_binary_datatypes getDataType() const {
        return extMetaLen > 0 ? protocol_binary_datatypes(*(data + FLEX_DATA_OFFSET)) :
            PROTOCOL_BINARY_RAW_BYTES;
    }

    /**
     * Set datatype for the value Blob.
     */
    void setDataType(uint8_t datatype) {
        std::memcpy(data + FLEX_DATA_OFFSET, &datatype, sizeof(uint8_t));
    }

    /**
     * Return the pointer to exteneded metadata, stored in the Blob.
     */
    const char* getExtMeta() const {
        return extMetaLen > 0 ? data + FLEX_DATA_OFFSET : NULL;
    }

    /**
     * Get the length of this Blob value.
     */
    size_t length() const {
        return size;
    }

    /**
     * Get the length of just the value part in the Blob.
     */
    size_t vlength() const {
        return size - extMetaLen - FLEX_DATA_OFFSET;
    }

    /**
     * Get the size of this Blob instance.
     */
    size_t getSize() const {
        return size + sizeof(Blob);
    }

    /**
     * Get extended meta data length, after subtracting the
     * size of FLEX_META_CODE.
     */
    uint8_t getExtLen() const {
        return extMetaLen;
    }

    /**
     * Returns how old this Blob is (how many epochs have passed since it was
     * created).
     */
    uint8_t getAge() const {
        return age;
    }

    /**
     * Increment the age of the Blob. Saturates at 255.
     */
    void incrementAge() {
        age++;
        // Saturate the result at 255 if we wrapped.
        if (age == 0) {
            age = 255;
        }
    }

    /**
     * Get a std::string representation of this blob.
     */
    const std::string to_s() const {
        return std::string(data + extMetaLen + FLEX_DATA_OFFSET,
                           vlength());
    }

    // This is necessary for making C++ happy when I'm doing a
    // placement new on fairly "normal" c++ heap allocations, just
    // with variable-sized objects.
    void operator delete(void* p) { ::operator delete(p); }

    ~Blob() {
        ObjectRegistry::onDeleteBlob(this);
    }

private:

    /* Constructor.
     * @param start If non-NULL, pointer to array which will be copied into
     *              the newly-created Blob.
     * @param len   Size of the data the Blob object will hold, and size of
     *              the data at {start}.
     * @param ext_meta Pointer to any extended metadata, which will be copied
     *                 into the newly created Blob.
     * @param ext_len Size of the data pointed to by {ext_meta}
     */
    explicit Blob(const char *start, const size_t len, uint8_t* ext_meta,
                  uint8_t ext_len) :
        size(static_cast<uint32_t>(len + FLEX_DATA_OFFSET + ext_len)),
        extMetaLen(static_cast<uint8_t>(ext_len)),
        age(0)
    {
        *(data) = FLEX_META_CODE;
        std::memcpy(data + FLEX_DATA_OFFSET, ext_meta, ext_len);
        if (start != NULL) {
            std::memcpy(data + FLEX_DATA_OFFSET + ext_len, start, len);
#ifdef VALGRIND
        } else {
            memset(data + FLEX_DATA_OFFSET + ext_len, 0, len);
#endif
        }
        ObjectRegistry::onCreateBlob(this);
    }

    explicit Blob(const size_t len, uint8_t ext_len) :
        size(static_cast<uint32_t>(len + FLEX_DATA_OFFSET + ext_len)),
        extMetaLen(static_cast<uint8_t>(ext_len)),
        age(0)
    {
#ifdef VALGRIND
        memset(data, 0, len);
#endif
        ObjectRegistry::onCreateBlob(this);
    }

    explicit Blob(const Blob& other)
      : size(other.size),
        extMetaLen(other.extMetaLen),
        // While this is a copy, it is a new allocation therefore reset age.
        age(0)
    {
        std::memcpy(data, other.data, size);
        ObjectRegistry::onCreateBlob(this);
    }

    const uint32_t size;
    const uint8_t extMetaLen;

    // The age of this Blob, in terms of some unspecified units of time.
    uint8_t age;
    char data[1];

    DISALLOW_ASSIGN(Blob);
};

typedef SingleThreadedRCPtr<Blob> value_t;

const uint64_t DEFAULT_REV_SEQ_NUM = 1;

/**
 * The ItemMetaData structure is used to pass meta data information of
 * an Item.
 */
class ItemMetaData {
public:
    ItemMetaData() :
        cas(0), revSeqno(DEFAULT_REV_SEQ_NUM), flags(0), exptime(0) {
    }

    ItemMetaData(uint64_t c, uint64_t s, uint32_t f, time_t e) :
        cas(c), revSeqno(s == 0 ? DEFAULT_REV_SEQ_NUM : s), flags(f),
        exptime(e) {
    }

    uint64_t cas;
    uint64_t revSeqno;
    uint32_t flags;
    time_t exptime;
};

/**
 * Conflict Resolution Modes
 */
enum conflict_resolution_mode {
    revision_seqno = 0,
    last_write_wins
};

/**
 * The Item structure we use to pass information between the memcached
 * core and the backend. Please note that the kvstore don't store these
 * objects, so we do have an extra layer of memory copying :(
 */
class Item : public RCValue {
public:

    /* Constructor (existing value_t).
     * Used when a value already exists, and the Item should refer to that
     * value.
     */
    Item(const std::string &k, const uint32_t fl, const time_t exp,
         const value_t &val, uint64_t theCas = 0,  int64_t i = -1,
         uint16_t vbid = 0, uint64_t sno = 1, uint8_t nru_value = INITIAL_NRU_VALUE,
         uint8_t conflict_res_value = revision_seqno) :
        metaData(theCas, sno, fl, exp),
        value(val),
        key(k),
        bySeqno(i),
        queuedTime(ep_current_time()),
        vbucketId(vbid),
        op(queue_op_set),
        nru(nru_value),
        conflictResMode(conflict_res_value)
    {
        if (bySeqno == 0) {
            throw std::invalid_argument("Item(): bySeqno must be non-zero");
        }
        ObjectRegistry::onCreateItem(this);
    }

    /* Constructor (new value).
     * {k, nk}   specify the item's key, k must be non-null and point to an
     *           array of bytes of length nk, where nk must be >0.
     * fl        Item flags.
     * exp       Item expiry.
     * {dta, nb} specify the item's value. nb specifies how much memory will be
     *           allocated for the value. If dta is non-NULL then the value
     *           is set from the memory pointed to by dta. If dta is NULL,
     *           then no data is copied in.
     *  The remaining arguments specify various optional attributes.
     */
    Item(const void *k, uint16_t nk, const uint32_t fl, const time_t exp,
         const void *dta, const size_t nb, uint8_t* ext_meta = NULL,
         uint8_t ext_len = 0, uint64_t theCas = 0, int64_t i = -1,
         uint16_t vbid = 0, uint64_t sno = 1, uint8_t nru_value = INITIAL_NRU_VALUE,
         uint8_t conflict_res_value = revision_seqno) :
        metaData(theCas, sno, fl, exp),
        key(static_cast<const char*>(k), nk),
        bySeqno(i),
        queuedTime(ep_current_time()),
        vbucketId(vbid),
        op(queue_op_set),
        nru(nru_value),
        conflictResMode(conflict_res_value)
    {
        if (bySeqno == 0) {
            throw std::invalid_argument("Item(): bySeqno must be non-zero");
        }
        setData(static_cast<const char*>(dta), nb, ext_meta, ext_len);
        ObjectRegistry::onCreateItem(this);
    }

   Item(const std::string &k, const uint16_t vb,
        enum queue_operation o, const uint64_t revSeq,
        const int64_t bySeq, uint8_t nru_value = INITIAL_NRU_VALUE,
        uint8_t conflict_res_value = revision_seqno) :
       metaData(),
       key(k),
       bySeqno(bySeq),
       queuedTime(ep_current_time()),
       vbucketId(vb),
       op(static_cast<uint16_t>(o)),
       nru(nru_value),
       conflictResMode(conflict_res_value)
    {
       if (bySeqno < 0) {
           throw std::invalid_argument("Item(): bySeqno must be non-negative");
       }
       metaData.revSeqno = revSeq;
       ObjectRegistry::onCreateItem(this);
    }

    /* Copy constructor */
    Item(const Item& other, bool copyKeyOnly = false) :
        metaData(other.metaData),
        key(other.key),
        bySeqno(other.bySeqno),
        queuedTime(other.queuedTime),
        vbucketId(other.vbucketId),
        op(other.op),
        nru(other.nru),
        conflictResMode(other.conflictResMode)
    {
        if (copyKeyOnly) {
            setData(nullptr, 0, nullptr, 0);
        } else {
            value = other.value;
        }
        ObjectRegistry::onCreateItem(this);
    }

    ~Item() {
        ObjectRegistry::onDeleteItem(this);
    }

    /* Snappy compress value and update datatype */
    bool compressValue(float minCompressionRatio = 1.0) {
        uint8_t datatype = getDataType();
        if (datatype == PROTOCOL_BINARY_RAW_BYTES ||
            datatype == PROTOCOL_BINARY_DATATYPE_JSON) {
            // Attempt compression only if datatype indicates
            // that the value is not compressed already.
            snap_buf output;
            snap_ret_t ret = doSnappyCompress(getData(), getNBytes(),
                                              output);
            if (ret == SNAP_SUCCESS) {
                if (output.len > minCompressionRatio * getNBytes()) {
                    // No point doing the compression if the desired
                    // compression ratio isn't achieved.
                    return true;
                }
                setData(output.buf.get(), output.len,
                        (uint8_t *)(getExtMeta()),
                        getExtMetaLen());
                setDataType((datatype == PROTOCOL_BINARY_RAW_BYTES)
                            ? PROTOCOL_BINARY_DATATYPE_COMPRESSED
                            : PROTOCOL_BINARY_DATATYPE_COMPRESSED_JSON);
            } else {
                return false;
            }
        }
        return true;
    }

    /* Snappy uncompress value and update datatype */
    bool decompressValue() {
        uint8_t datatype = getDataType();
        if (datatype == PROTOCOL_BINARY_DATATYPE_COMPRESSED ||
            datatype == PROTOCOL_BINARY_DATATYPE_COMPRESSED_JSON) {
            // Attempt decompression only if datatype indicates
            // that the value is compressed.
            snap_buf output;
            snap_ret_t ret = doSnappyUncompress(getData(), getNBytes(),
                                                output);
            if (ret == SNAP_SUCCESS) {
                setData(output.buf.get(), output.len,
                        (uint8_t *)(getExtMeta()),
                        getExtMetaLen());
                setDataType((datatype == PROTOCOL_BINARY_DATATYPE_COMPRESSED)
                            ? PROTOCOL_BINARY_RAW_BYTES
                            : PROTOCOL_BINARY_DATATYPE_JSON);
            } else {
                return false;
            }
        }
        return true;
    }

    const char *getData() const {
        return value.get() ? value->getData() : NULL;
    }

    const char *getBlob() const {
        return value.get() ? value->getBlob() : NULL;
    }

    const value_t &getValue() const {
        return value;
    }

    const std::string &getKey() const {
        return key;
    }

    int64_t getBySeqno() const {
        return bySeqno;
    }

    void setBySeqno(int64_t to) {
        bySeqno = to;
    }

    int getNKey() const {
        return static_cast<int>(key.length());
    }

    uint32_t getNBytes() const {
        return value.get() ? static_cast<uint32_t>(value->vlength()) : 0;
    }

    size_t getValMemSize() const {
        return value.get() ? value->getSize() : 0;
    }

    time_t getExptime() const {
        return metaData.exptime;
    }

    uint32_t getFlags() const {
        return metaData.flags;
    }

    uint64_t getCas() const {
        return metaData.cas;
    }

    protocol_binary_datatypes getDataType() const {
        return value.get() ? value->getDataType() :
            PROTOCOL_BINARY_RAW_BYTES;
    }

    void setDataType(uint8_t datatype) {
        value->setDataType(datatype);
    }

    const char* getExtMeta() const {
        return value.get() ? value->getExtMeta() : NULL;
    }

    uint8_t getExtMetaLen() const {
        return value.get() ? value->getExtLen() : 0;
    }

    void setCas() {
        metaData.cas = nextCas();
    }

    void setCas(uint64_t ncas) {
        metaData.cas = ncas;
    }

    void setValue(const value_t &v) {
        value.reset(v);
    }

    void setFlags(uint32_t f) {
        metaData.flags = f;
    }

    void setExpTime(time_t exp_time) {
        metaData.exptime = exp_time;
    }

    /**
     * Append another item to this item
     *
     * @param item the item to append to this one
     * @param maxItemSize maximum item size permitted
     * @return ENGINE_SUCCESS if success
     */
    ENGINE_ERROR_CODE append(const Item &item, size_t maxItemSize);

    /**
     * Prepend another item to this item
     *
     * @param item the item to prepend to this one
     * @param maxItemSize maximum item size permitted
     * @return ENGINE_SUCCESS if success
     */
    ENGINE_ERROR_CODE prepend(const Item &item, size_t maxItemSize);

    uint16_t getVBucketId(void) const {
        return vbucketId;
    }

    void setVBucketId(uint16_t to) {
        vbucketId = to;
    }

    /**
     * Check if this item is expired or not.
     *
     * @param asOf the time to be compared with this item's expiry time
     * @return true if this item's expiry time < asOf
     */
    bool isExpired(time_t asOf) const {
        if (metaData.exptime != 0 && metaData.exptime < asOf) {
            return true;
        }
        return false;
    }

    size_t size(void) const {
        return sizeof(Item) + key.size() + getValMemSize();
    }

    uint64_t getRevSeqno() const {
        return metaData.revSeqno;
    }

    void setRevSeqno(uint64_t to) {
        if (to == 0) {
            to = DEFAULT_REV_SEQ_NUM;
        }
        metaData.revSeqno = to;
    }

    static uint32_t getNMetaBytes() {
        return metaDataSize;
    }

    const ItemMetaData& getMetaData() const {
        return metaData;
    }

    bool isDeleted() {
        return op == queue_op_del;
    }

    void setDeleted() {
        op = queue_op_del;
    }

    uint32_t getQueuedTime(void) const { return queuedTime; }

    enum queue_operation getOperation(void) const {
        return static_cast<enum queue_operation>(op);
    }

    void setOperation(enum queue_operation o) {
        op = static_cast<uint8_t>(o);
    }

    bool isCheckPointMetaItem(void) const {
        queue_operation qOp = static_cast<enum queue_operation>(op);
        if ((queue_op_set == qOp) || (queue_op_del == qOp)) {
            return false;
        }
        return true;
    }

    void setNRUValue(uint8_t nru_value) {
        nru = nru_value;
    }

    uint8_t getNRUValue() const {
        return nru;
    }

    static uint64_t nextCas(void) {
        return gethrtime() + (++casCounter);
    }

    /* Returns true if the specified CAS is valid */
    static bool isValidCas(const uint64_t& itmCas) {
        if (itmCas == 0 || itmCas == static_cast<uint64_t>(-1)) {
            return false;
        }
        return true;
    }


    void setConflictResMode(enum conflict_resolution_mode conf_res_value) {
        conflictResMode = static_cast<uint8_t>(conf_res_value);
    }

    enum conflict_resolution_mode getConflictResMode(void) const {
        return static_cast<enum conflict_resolution_mode>(conflictResMode);
    }

private:
    /**
     * Set the item's data. This is only used by constructors, so we
     * make it private.
     */
    void setData(const char *dta, const size_t nb, uint8_t* ext_meta,
                 uint8_t ext_len) {
        Blob *data;
        if (dta == NULL) {
            data = Blob::New(nb, ext_meta, ext_len);
        } else {
            data = Blob::New(dta, nb, ext_meta, ext_len);
        }
        value.reset(data);
    }

    ItemMetaData metaData;
    value_t value;
    std::string key;
    int64_t bySeqno;
    uint32_t queuedTime;
    uint16_t vbucketId;
    uint8_t op;
    uint8_t nru  : 2;
    uint8_t conflictResMode : 2;

    static std::atomic<uint64_t> casCounter;
    static const uint32_t metaDataSize;
    DISALLOW_ASSIGN(Item);
};

typedef SingleThreadedRCPtr<Item> queued_item;

/**
 * Order queued_item objects pointed by std::shared_ptr by their keys.
 */
class CompareQueuedItemsByKey {
public:
    CompareQueuedItemsByKey() {}
    bool operator()(const queued_item &i1, const queued_item &i2) {
        return i1->getKey() < i2->getKey();
    }
};

/**
 * Order QueuedItem objects by their keys and by sequence numbers.
 */
class CompareQueuedItemsBySeqnoAndKey {
public:
    CompareQueuedItemsBySeqnoAndKey() {}
    bool operator()(const queued_item &i1, const queued_item &i2) {
        return i1->getKey() == i2->getKey()
            ? i1->getBySeqno() > i2->getBySeqno()
            : i1->getKey() < i2->getKey();
    }
};

#endif  // SRC_ITEM_H_
