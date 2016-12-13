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

#ifndef SRC_TAPCONNMAP_H_
#define SRC_TAPCONNMAP_H_ 1

#include "config.h"

#include <climits>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ep_engine.h"
#include "locks.h"
#include "syncobject.h"
#include "tapconnection.h"
#include "atomicqueue.h"
#include "dcp/consumer.h"
#include "dcp/producer.h"

// Forward declaration
class ConnNotifier;
class TapConsumer;
class TapProducer;
class Item;
class EventuallyPersistentEngine;

typedef SingleThreadedRCPtr<ConnHandler> connection_t;
/**
 * Base class for operations performed on tap connections.
 *
 * @see TapConnMap::performTapOp
 */
template <typename V>
class TapOperation {
public:
    virtual ~TapOperation() {}
    virtual void perform(TapProducer *tc, V arg) = 0;
};

/**
 * Indicate the tap operation is complete.
 */
class CompleteBackfillTapOperation : public TapOperation<void*> {
public:
    void perform(TapProducer *tc, void* arg);
};

/**
 * Indicate that we are going to schedule a tap disk backfill for a given vbucket.
 */
class ScheduleDiskBackfillTapOperation : public TapOperation<void*> {
public:
    void perform(TapProducer *tc, void* arg);
};

/**
 * Indicate the tap backfill disk stream thing is complete for a given vbucket.
 */
class CompleteDiskBackfillTapOperation : public TapOperation<void*> {
public:
    void perform(TapProducer *tc, void* arg);
};

/**
 * Complete a bg fetch job and give the item to the given tap connection.
 */
class CompletedBGFetchTapOperation : public TapOperation<Item*> {
public:
    CompletedBGFetchTapOperation(hrtime_t token, uint16_t vb, bool ie=false) :
        connToken(token), vbid(vb), implicitEnqueue(ie) {}

    void perform(TapProducer *tc, Item* arg);
private:
    hrtime_t connToken;
    uint16_t vbid;
    bool implicitEnqueue;
};

class TAPSessionStats {
public:
    TAPSessionStats() : normalShutdown(true) {}

    bool wasReplicationCompleted(const std::string &name) const;

    void clearStats(const std::string &name);

    bool normalShutdown;
    std::map<std::string, std::string> stats;
};


/**
 * Connection notifier type.
 */
enum conn_notifier_type {
    TAP_CONN_NOTIFIER, //!< TAP connection notifier
    DCP_CONN_NOTIFIER  //!< DCP connection notifier
};

/**
 * A collection of tap or dcp connections.
 */
class ConnMap {
public:
    ConnMap(EventuallyPersistentEngine &theEngine);
    virtual ~ConnMap();

    void initialize(conn_notifier_type ntype);

    Consumer *newConsumer(const void* c);

    /**
     * Disconnect a connection by its cookie.
     */
    virtual void disconnect(const void *cookie) = 0;

    /**
     * Call a function on each connection.
     */
    template <typename Fun>
    void each(Fun f) {
        LockHolder lh(connsLock);
        each_UNLOCKED(f);
    }

    /**
     * Call a function on each connection *without* a lock.
     */
    template <typename Fun>
    void each_UNLOCKED(Fun f) {
        std::for_each(all.begin(), all.end(), f);
    }

    /**
     * Return the number of connections for which this predicate is true.
     */
    template <typename Fun>
    size_t count_if(Fun f) {
        LockHolder lh(connsLock);
        return count_if_UNLOCKED(f);
    }

    /**
     * Return the number of connections for which this predicate is
     * true *without* a lock.
     */
    template <typename Fun>
    size_t count_if_UNLOCKED(Fun f) {
        return static_cast<size_t>(std::count_if(all.begin(), all.end(), f));
    }

    /**
     * Purge dead connections or identify paused connections that should send
     * NOOP messages to their destinations.
     */
    virtual void manageConnections() = 0;

    connection_t findByName(const std::string &name);

    virtual void shutdownAllConnections() = 0;

    virtual bool isDeadConnectionsEmpty() {
        return true;
    }

    bool isAllEmpty() {
        LockHolder lh(connsLock);
        return all.empty();
    }

    void updateVBConnections(connection_t &conn,
                             const std::vector<uint16_t> &vbuckets);

    virtual void removeVBConnections(connection_t &conn);

    void addVBConnByVBId(connection_t &conn, int16_t vbid);

    void removeVBConnByVBId_UNLOCKED(connection_t &conn, int16_t vbid);

    void removeVBConnByVBId(connection_t &conn, int16_t vbid);

    /**
     * Notify a given paused connection.
     *
     * @param tc connection to be notified
     * @param schedule true if a notification event is pushed into a queue.
     *        Otherwise, directly notify the paused connection.
     */
    void notifyPausedConnection(connection_t conn, bool schedule = false);

    void notifyAllPausedConnections();
    bool notificationQueueEmpty();

    EventuallyPersistentEngine& getEngine() {
        return engine;
    }

protected:

    connection_t findByName_UNLOCKED(const std::string &name);

    // Synchronises notifying and releasing connections.
    // Guards modifications to connection_t objects in {map_} / {all}.
    // See also: {connLock}
    Mutex                                    releaseLock;

    // Synchonises access to the {map_} and {all} members, i.e. adding
    // removing connections.
    // Actual modification of the underlying
    // ConnHandler objects is guarded by {releaseLock}.
    Mutex                                    connsLock;

    using CookieToConnectionMap = std::map<const void*, connection_t>;
    CookieToConnectionMap map_;
    std::list<connection_t>                  all;

    SpinLock *vbConnLocks;
    std::vector<std::list<connection_t> > vbConns;

    /* Handle to the engine who owns us */
    EventuallyPersistentEngine &engine;

    AtomicQueue<connection_t> pendingNotifications;
    ConnNotifier *connNotifier_;

    static size_t vbConnLockNum;
};

/**
 * Connection notifier that wakes up paused connections.
 */
class ConnNotifier {
public:
    ConnNotifier(conn_notifier_type ntype, ConnMap &cm)
        : notifier_type(ntype), connMap(cm), task(0),
          pendingNotification(false)  { }

    void start();

    void stop();

    void wake();

    void notifyMutationEvent();

    bool notifyConnections();

    conn_notifier_type getNotifierType() const {
        return notifier_type;
    }

private:
    static const double DEFAULT_MIN_STIME;

    conn_notifier_type notifier_type;
    ConnMap &connMap;
    AtomicValue<size_t> task;
    AtomicValue<bool> pendingNotification;
};

class TapConnMap : public ConnMap {

public:

    TapConnMap(EventuallyPersistentEngine &theEngine);

    /**
     * Find or build a tap connection for the given cookie and with
     * the given name.
     */
    TapProducer *newProducer(const void* cookie,
                             const std::string &name,
                             uint32_t flags,
                             uint64_t backfillAge,
                             int tapKeepAlive,
                             const std::vector<uint16_t> &vbuckets,
                             const std::map<uint16_t, uint64_t> &lastCheckpointIds);


    /**
     * Create a new consumer and add it in the list of TapConnections
     * @param e the engine
     * @param c the cookie representing the client
     * @return Pointer to the nw tap connection
     */
    TapConsumer *newConsumer(const void* c);

    void manageConnections();

    /**
     * Notify the paused connections that are responsible for replicating
     * a given vbucket.
     * @param vbid vbucket id
     */
    void notifyVBConnections(uint16_t vbid);

    void resetReplicaChain();

    /**
     * Get the size of the named backfill queue.
     *
     * @return the size, or -1 if we can't find the queue
     */
    ssize_t backfillQueueDepth(const std::string &name);

    void incrBackfillRemaining(const std::string &name,
                               size_t num_backfill_items);

    void shutdownAllConnections();

    void disconnect(const void *cookie);

    void scheduleBackfill(const std::set<uint16_t> &backfillVBuckets);

    bool isBackfillCompleted(std::string &name);

    /**
     * Add an event to all tap connections telling them to flush their
     * items.
     */
    void addFlushEvent();

    /**
     * Change the vbucket filter for a given TAP producer
     * @param name TAP producer name
     * @param vbuckets a new vbucket filter
     * @param checkpoints last closed checkpoint ids for a new vbucket filter
     * @return true if the TAP producer's vbucket filter is changed successfully
     */
    bool changeVBucketFilter(const std::string &name,
                             const std::vector<uint16_t> &vbuckets,
                             const std::map<uint16_t, uint64_t> &checkpoints);

    /**
     * Load TAP-related stats from the previous engine sessions
     *
     * @param session_stats all the stats from the previous engine sessions
     */
    void loadPrevSessionStats(const std::map<std::string, std::string> &session_stats);

    /**
     * Check if the given TAP producer completed the replication before
     * shutdown or crash.
     *
     * @param name TAP producer's name
     * @return true if the replication from the given TAP producer was
     * completed before shutdown or crash.
     */
    bool prevSessionReplicaCompleted(const std::string &name) {
        return prevSessionStats.wasReplicationCompleted(name);
    }

    bool checkConnectivity(const std::string &name);

    bool closeConnectionByName(const std::string &name);

    bool mapped(connection_t &tc);

    /**
     * Perform a TapOperation for a named tap connection while holding
     * appropriate locks.
     *
     * @param name the name of the tap connection to run the op
     * @param tapop the operation to perform
     * @param arg argument for the tap operation
     *
     * @return true if the tap connection was valid and the operation
     *         was performed
     */
    template <typename V>
    bool performOp(const std::string &name, TapOperation<V> &tapop, V arg) {
        bool ret(true);
        LockHolder lh(connsLock);

        connection_t tc = findByName_UNLOCKED(name);
        if (tc.get()) {
            TapProducer *tp = dynamic_cast<TapProducer*>(tc.get());
            if (tp == nullptr) {
                throw std::logic_error(
                        "TapConnMap::performOp: name (which is " + name +
                        ") refers to a connection_t which is not a TapProducer. "
                        "Connection logHeader is '" + tc.get()->logHeader() + "'");
            }
            tapop.perform(tp, arg);
            lh.unlock();
            notifyPausedConnection(tp, false);
        } else {
            ret = false;
        }

        return ret;
    }

    size_t getNoopInterval() const {
        return noopInterval_;
    }

    void setNoopInterval(size_t value) {
        noopInterval_ = value;
        nextNoop_ = 0;
    }

private:

    /**
     * Clear all the session stats for a given TAP producer
     *
     * @param name TAP producer's name
     */
    void clearPrevSessionStats(const std::string &name) {
        prevSessionStats.clearStats(name);
    }

    void getExpiredConnections_UNLOCKED(std::list<connection_t> &deadClients);

    void removeTapCursors_UNLOCKED(TapProducer *tp);

    bool closeConnectionByName_UNLOCKED(const std::string &name);

    TAPSessionStats prevSessionStats;
    size_t noopInterval_;
    size_t nextNoop_;

};

class DcpConnMap : public ConnMap {
public:

    DcpConnMap(EventuallyPersistentEngine &engine);

    /**
     * Find or build a dcp connection for the given cookie and with
     * the given name.
     */
    DcpProducer *newProducer(const void* cookie, const std::string &name,
                             bool notifyOnly);


    /**
     * Create a new consumer and add it in the list of TapConnections
     * @param e the engine
     * @param c the cookie representing the client
     * @return Pointer to the new dcp connection
     */
    DcpConsumer *newConsumer(const void* cookie, const std::string &name);

    void notifyVBConnections(uint16_t vbid, uint64_t bySeqno);

    void notifyBackfillManagerTasks();

    void removeVBConnections(connection_t &conn);

    /**
     * Close outbound (active) streams for a vbucket whenever a state
     * change is detected. In case of failovers, close inbound (passive)
     * streams as well.
     *
     * @param vbucket the vbucket id
     * @param state the new state of the vbucket
     * @closeInboundStreams bool flag indicating failover
     */
    void vbucketStateChanged(uint16_t vbucket, vbucket_state_t state,
                             bool closeInboundStreams = true);

    void shutdownAllConnections();

    bool isDeadConnectionsEmpty() {
        LockHolder lh(connsLock);
        return deadConnections.empty();
    }

    /**
     * Handles the slow stream with the specified name.
     * Returns true if the stream dropped its cursors on the
     * checkpoint.
     */
    bool handleSlowStream(uint16_t vbid, const std::string &name);

    void disconnect(const void *cookie);

    void manageConnections();

    bool canAddBackfillToActiveQ();

    void decrNumActiveSnoozingBackfills();

    void updateMaxActiveSnoozingBackfills(size_t maxDataSize);

    uint16_t getNumActiveSnoozingBackfills () {
        SpinLockHolder lh(&numBackfillsLock);
        return numActiveSnoozingBackfills;
    }

    uint16_t getMaxActiveSnoozingBackfills () {
        SpinLockHolder lh(&numBackfillsLock);
        return maxActiveSnoozingBackfills;
    }

    ENGINE_ERROR_CODE addPassiveStream(ConnHandler& conn, uint32_t opaque,
                                       uint16_t vbucket, uint32_t flags);

    /* Use this only for any quick direct stats from DcpConnMap. To collect
       individual conn stats from conn lists please use ConnStatBuilder */
    void addStats(ADD_STAT add_stat, const void *c);

    /* Updates the minimum compression ratio to be achieved for docs by
     * all the producers, which will be in effect if the producer side
     * value compression is enabled */
    void updateMinCompressionRatioForProducers(float value);

    float getMinCompressionRatio();

protected:
    /*
     * deadConnections is protected (as opposed to private) because
     * of the module test ep-engine_dead_connections_test
     */
    std::list<connection_t> deadConnections;

    /*
     * Change the value at which a DcpConsumer::Processor task will yield
     */
    void consumerYieldConfigChanged(size_t newValue);

    /*
     * Change the batchsize that the DcpConsumer::Processor operates with
     */
    void consumerBatchSizeConfigChanged(size_t newValue);

    bool isPassiveStreamConnected_UNLOCKED(uint16_t vbucket);

    /*
     * Closes all streams associated with each connection in `map`.
     */
    static void closeStreams(CookieToConnectionMap& map);

    /*
     * Cancels all tasks assocuated with each connection in `map`.
     */
    static void cancelTasks(CookieToConnectionMap& map);

    SpinLock numBackfillsLock;
    /* Db file memory */
    static const uint32_t dbFileMem;
    uint16_t numActiveSnoozingBackfills;
    uint16_t maxActiveSnoozingBackfills;
    /* Max num of backfills we want to have irrespective of memory */
    static const uint16_t numBackfillsThreshold;
    /* Max percentage of memory we want backfills to occupy */
    static const uint8_t numBackfillsMemThreshold;

    AtomicValue<float> minCompressionRatioForProducer;

    /* Total memory used by all DCP consumer buffers */
    AtomicValue<size_t> aggrDcpConsumerBufferSize;

    class DcpConfigChangeListener : public ValueChangedListener {
    public:
        DcpConfigChangeListener(DcpConnMap& connMap);
        virtual ~DcpConfigChangeListener() { }
        virtual void sizeValueChanged(const std::string &key, size_t value);
    private:
        DcpConnMap& myConnMap;
    };
};


#endif  // SRC_TAPCONNMAP_H_
