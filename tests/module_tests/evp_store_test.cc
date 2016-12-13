/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
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

/*
 * Unit tests for the EventuallyPersistentStore class.
 *
 * Note that these test do *not* have the normal Tasks running (BGFetcher,
 * flusher etc) as we do not initialise EPEngine. This means that such tasks
 * need to be manually run. This can be very helpful as it essentially gives us
 * synchronous control of EPStore.
 */

#include "evp_store_test.h"

#include "bgfetcher.h"
#include "checkpoint.h"
#include "checkpoint_remover.h"
#include "connmap.h"
#include "dcp/flow-control-manager.h"
#include "ep_engine.h"
#include "flusher.h"
#include "../mock/mock_dcp_producer.h"
#include "replicationthrottle.h"

#include "programs/engine_testapp/mock_server.h"
#include <platform/dirutils.h>
#include <thread>

SynchronousEPEngine::SynchronousEPEngine(const std::string& extra_config)
    : EventuallyPersistentEngine(get_mock_server_api) {
    maxFailoverEntries = 1;

    // Merge any extra config into the main configuration.
    if (extra_config.size() > 0) {
        if (!configuration.parseConfiguration(extra_config.c_str(),
                                              serverApi)) {
            throw std::invalid_argument("Unable to parse config string: " +
                                        extra_config);
        }
    }

    // workload is needed by EPStore's constructor (to construct the
    // VBucketMap).
    workload = new WorkLoadPolicy(/*workers*/1, /*shards*/1);

    // dcpConnMap_ is needed by EPStore's constructor.
    dcpConnMap_ = new DcpConnMap(*this);

    // tapConnMap is needed by queueDirty.
    tapConnMap = new TapConnMap(*this);

    // checkpointConfig is needed by CheckpointManager (via EPStore).
    checkpointConfig = new CheckpointConfig(*this);

    dcpFlowControlManager_ = new DcpFlowControlManager(*this);

    replicationThrottle = new ReplicationThrottle(configuration, stats);

    tapConfig = new TapConfig(*this);
}

void SynchronousEPEngine::setEPStore(EventuallyPersistentStore* store) {
    cb_assert(epstore == nullptr);
    epstore = store;
}

MockEPStore::MockEPStore(EventuallyPersistentEngine &theEngine)
    : EventuallyPersistentStore(theEngine) {
    // Perform a limited set of setup (normally done by EPStore::initialize) -
    // enough such that objects which are assumed to exist are present.

    // Create the closed checkpoint removed task. Note we do _not_ schedule
    // it, unlike EPStore::initialize
    chkTask = new ClosedUnrefCheckpointRemoverTask
            (&engine, stats, theEngine.getConfiguration().getChkRemoverStime());
}

VBucketMap& MockEPStore::getVbMap() {
    return vbMap;
}

/* Mock Task class. Doesn't actually run() or snooze() - they both do nothing.
 */
class MockGlobalTask : public GlobalTask {
public:
    MockGlobalTask(Taskable& t, TaskId id)
        : GlobalTask(t, id) {}

    bool run() override { return false; }
    std::string getDescription() override { return "MockGlobalTask"; }

    void snooze(const double secs) override {}
};

void EventuallyPersistentStoreTest::SetUp() {
    // Paranoia - kill any existing files in case they are left over
    // from a previous run.
    CouchbaseDirectoryUtilities::rmrf(test_dbname);

    // Add dbname to config string.
    std::string config = config_string;
    if (config.size() > 0) {
        config += ";";
    }
    config += "dbname=" + std::string(test_dbname);

    engine.reset(new SynchronousEPEngine(config));
    ObjectRegistry::onSwitchThread(engine.get());

    store = new MockEPStore(*engine);
    engine->setEPStore(store);

    // Ensure that EPEngine is hold about necessary server callbacks
    // (client disconnect, bucket delete).
    engine->public_initializeEngineCallbacks();

    // Need to initialize ep_real_time and friends.
    initialize_time_functions(get_mock_server_api()->core);

    cookie = create_mock_cookie();
}

void EventuallyPersistentStoreTest::TearDown() {
    destroy_mock_cookie(cookie);
    destroy_mock_event_callbacks();
    engine->getDcpConnMap().manageConnections();
    engine.reset();

    // Shutdown the ExecutorPool singleton (initialized when we create
    // an EventuallyPersistentStore object). Must happen after engine
    // has been destroyed (to allow the tasks the engine has
    // registered a chance to be unregistered).
    ExecutorPool::shutdown();
}

void EventuallyPersistentStoreTest::store_item(uint16_t vbid,
                                               const std::string& key,
                                               const std::string& value) {
    Item item(key.c_str(), key.size(), /*flags*/0, /*exp*/0, value.c_str(),
              value.size());
    item.setVBucketId(vbid);
    EXPECT_EQ(ENGINE_SUCCESS, store->set(item, nullptr));
}


// Verify that when handling a bucket delete with open DCP
// connections, we don't deadlock when notifying the front-end
// connection.
// This is a potential issue because notify_IO_complete
// needs to lock the worker thread mutex the connection is assigned
// to, to update the event list for that connection, which the worker
// thread itself will have locked while it is running. Normally
// deadlock is avoided by using a background thread (ConnNotifier),
// which only calls notify_IO_complete and isnt' involved with any
// other mutexes, however we cannot use that task as it gets shut down
// during shutdownAllConnections.
// This test requires ThreadSanitizer or similar to validate;
// there's no guarantee we'll actually deadlock on any given run.
TEST_F(EventuallyPersistentStoreTest, test_mb20751_deadlock_on_disconnect_delete) {

    // Create a new Dcp producer, reserving its cookie.
    get_mock_server_api()->cookie->reserve(cookie);
    dcp_producer_t producer = engine->getDcpConnMap().newProducer(
        cookie, "mb_20716r", /*notifyOnly*/false);

    // Check preconditions.
    EXPECT_TRUE(producer->isPaused());

    // 1. To check that there's no potential data-race with the
    //    concurrent connection disconnect on another thread
    //    (simulating a front-end thread).
    std::thread frontend_thread_handling_disconnect{[this](){
            // Frontend thread always runs with the cookie locked, so
            // lock here to match.
            lock_mock_cookie(cookie);
            engine->handleDisconnect(cookie);
            unlock_mock_cookie(cookie);
        }};

    // 2. Trigger a bucket deletion.
    engine->handleDeleteBucket(cookie);

    frontend_thread_handling_disconnect.join();
}

class EPStoreEvictionTest : public EventuallyPersistentStoreTest,
                             public ::testing::WithParamInterface<std::string> {
    void SetUp() override {
        config_string += std::string{"item_eviction_policy="} + GetParam();
        EventuallyPersistentStoreTest::SetUp();

        // Have all the objects, activate vBucket zero so we can store data.
        store->setVBucketState(vbid, vbucket_state_active, false);

    }
};

// getKeyStats tests //////////////////////////////////////////////////////////

// Check that keystats on resident items works correctly.
TEST_P(EPStoreEvictionTest, GetKeyStatsResident) {
    key_stats kstats;

    // Should start with key not existing.
    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->getKeyStats("key", 0, cookie, kstats, /*bgfetch*/true,
                                 /*wantsDeleted*/false));

    store_item(0, "key", "value");
    EXPECT_EQ(ENGINE_SUCCESS,
              store->getKeyStats("key", 0, cookie, kstats, /*bgfetch*/true,
                                 /*wantsDeleted*/false))
        << "Expected to get key stats on existing item";
    EXPECT_EQ(vbucket_state_active, kstats.vb_state);
    EXPECT_FALSE(kstats.logically_deleted);
}

// Check that keystats on ejected items. When ejected should return ewouldblock
// until bgfetch completes.
TEST_P(EPStoreEvictionTest, GetKeyStatsEjected) {
    key_stats kstats;

    // Store then eject an item. Note we cannot forcefully evict as we have
    // to ensure it's own disk so we can later bg fetch from there :)
    store_item(0, "key", "value");

    // Trigger a flush to disk. We have to retry as the warmup may not be
    // complete.
    int result;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    do {
        result = store->flushVBucket(vbid);
        if (result != RETRY_FLUSH_VBUCKET) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    ASSERT_EQ(1,
              result) << "Failed to flush the one item we have stored.";

    const char* msg;
    size_t msg_size{sizeof(msg)};
    EXPECT_EQ(ENGINE_SUCCESS, store->evictKey("key", 0, &msg, &msg_size));
    EXPECT_EQ("Ejected.", std::string(msg));

    // Setup a lambda for how we want to call getKeyStats (saves repeating the
    // same arguments for each instance below).
    auto do_getKeyStats = [this, &kstats]() {
        return store->getKeyStats("key", vbid, cookie, kstats,
                                  /*bgfetch*/true, /*wantsDeleted*/false);
    };

    if (GetParam() == "value_only") {
        EXPECT_EQ(ENGINE_SUCCESS, do_getKeyStats())
            << "Expected to get key stats on evicted item";

    } else if (GetParam() == "full_eviction") {

        // Try to get key stats. This should return EWOULDBLOCK (as the whole
        // item is no longer resident). As we arn't running the full EPEngine
        // task system, then no BGFetch task will be automatically run, we'll
        // manually run it.

        EXPECT_EQ(ENGINE_EWOULDBLOCK, do_getKeyStats())
            << "Expected to need to go to disk to get key stats on fully evicted item";

        // Try a second time - this should detect the already-created temp
        // item, and re-schedule the bgfetch.
        EXPECT_EQ(ENGINE_EWOULDBLOCK, do_getKeyStats())
            << "Expected to need to go to disk to get key stats on fully evicted item (try 2)";

        // Manually run the BGFetcher task; to fetch the two outstanding
        // requests (for the same key).
        MockGlobalTask mockTask(engine->getTaskable(),
                                TaskId::MultiBGFetcherTask);
        store->getVBucket(vbid)->getShard()->getBgFetcher()->run(&mockTask);

        EXPECT_EQ(ENGINE_SUCCESS, do_getKeyStats())
            << "Expected to get key stats on evicted item after notify_IO_complete";

    } else {
        FAIL() << "Unhandled GetParam() value:" << GetParam();
    }
}

// Create then delete an item, checking we get keyStats reporting the item as
// deleted.
TEST_P(EPStoreEvictionTest, GetKeyStatsDeleted) {
    auto& epstore = *engine->getEpStore();
    key_stats kstats;

    store_item(0, "key", "value");
    uint64_t cas = 0;
    mutation_descr_t mut_info;
    EXPECT_EQ(ENGINE_SUCCESS,
              epstore.deleteItem("key", &cas, /*vbucket*/0, cookie,
                                 /*force*/false, /*itemMeta*/nullptr,
                                 &mut_info));

    // Should get ENOENT if we don't ask for deleted items.
    EXPECT_EQ(ENGINE_KEY_ENOENT,
              epstore.getKeyStats("key", 0, cookie, kstats, /*bgfetch*/false,
                                  /*wantsDeleted*/false));

    // Should get success (and item flagged as deleted) if we ask for deleted
    // items.
    EXPECT_EQ(ENGINE_SUCCESS,
              epstore.getKeyStats("key", 0, cookie, kstats, /*bgfetch*/true,
                                  /*wantsDeleted*/true));
    EXPECT_EQ(vbucket_state_active, kstats.vb_state);
    EXPECT_TRUE(kstats.logically_deleted);
}

// Check incorrect vbucket returns not-my-vbucket.
TEST_P(EPStoreEvictionTest, GetKeyStatsNMVB) {
    auto& epstore = *engine->getEpStore();
    key_stats kstats;

    EXPECT_EQ(ENGINE_NOT_MY_VBUCKET,
              epstore.getKeyStats("key", 1, cookie, kstats, /*bgfetch*/true,
                                  /*wantsDeleted*/false));
}


// Test cases which run in both Full and Value eviction
INSTANTIATE_TEST_CASE_P(FullAndValueEviction,
                        EPStoreEvictionTest,
                        ::testing::Values("value_only", "full_eviction"),
                        [] (const ::testing::TestParamInfo<std::string>& info) {
                            return info.param;
                        });


const char EventuallyPersistentStoreTest::test_dbname[] = "ep_engine_ep_unit_tests_db";
