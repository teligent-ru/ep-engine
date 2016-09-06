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

#include "config.h"

#include "atomic_unordered_map.h"

#include <thread>

#include <gtest/gtest.h>

/* Test class which inherits from RCValue */
struct DummyValue : public RCValue {
public:
    DummyValue(size_t value_)
        : value(value_) {}

    bool operator==(const DummyValue& other) const {
        return value == other.value;
    }

    size_t value;
};

class AtomicUnorderedMapTest : public ::testing::Test {
public:
    typedef AtomicUnorderedMap<int, SingleThreadedRCPtr<DummyValue>> TestMap;

    // Add N items to a map starting from the given offset.
    static void insert_into_map(TestMap& map, size_t n, size_t offset) {
        for (unsigned int ii = 0; ii < n; ii++) {
            TestMap::mapped_type val{new DummyValue(ii * 10)};
            map.insert({offset + ii, val});
        }
    }

protected:
    TestMap map;
};

/* Basic functionality sanity checks. */

TEST_F(AtomicUnorderedMapTest, Empty) {
    ASSERT_EQ(0u, map.size());
    EXPECT_FALSE(map.find(0).second) << "Should start with empty map";
}

TEST_F(AtomicUnorderedMapTest, InsertOne) {
    TestMap::mapped_type ptr{new DummyValue(10)};
    map.insert({0, ptr});

    EXPECT_EQ(1, map.size());
    EXPECT_EQ(ptr, map.find(0).first);
}

TEST_F(AtomicUnorderedMapTest, ReplaceOne) {

    TestMap::mapped_type ptr{new DummyValue(10)};
    TestMap::mapped_type ptr2{new DummyValue(20)};

    EXPECT_TRUE(map.insert({0, ptr}));
    EXPECT_TRUE(map.insert({1, ptr2}));
    EXPECT_EQ(2, map.size()) << "Adding another item should succeed";
    EXPECT_EQ(ptr2, map.find(1).first);

    TestMap::mapped_type ptr3{new DummyValue(30)};
    EXPECT_FALSE(map.insert({1, ptr3}))
        << "Inserting a key which already exists should fail";
    auto erased1 = map.erase(1);
    EXPECT_TRUE(erased1.second) << "Erasing key 1 should succeed";
    EXPECT_EQ(ptr2, erased1.first) << "Erasing key 1 should return value 1";

    EXPECT_TRUE(map.insert({1, ptr3}))
        << "Inserting a key which has been erased should succeed";
    EXPECT_EQ(2, map.size()) << "Replacing an item should keep size the same";
    EXPECT_EQ(ptr3, map.find(1).first);

    auto erased = map.erase(0);
    EXPECT_TRUE(erased.second) << "Failed to erase key 0";
    EXPECT_EQ(ptr, erased.first) << "Erasing key 0 should return value 0";

    map.clear();
    EXPECT_EQ(0, map.size()) << "Clearing map should remove all items";
    EXPECT_FALSE(map.find(0).second) << "Should end with empty map";
}

// Test that performing concurrent, disjoint insert (different keys) is thread-safe.
TEST_F(AtomicUnorderedMapTest, ConcurrentDisjointInsert) {
    // Add 10 elements from two threads, with the second starting from offset 10.
    const size_t n_elements{10};
    std::thread t1{insert_into_map, std::ref(map), n_elements, 0};
    std::thread t2{insert_into_map, std::ref(map), n_elements, n_elements};
    t1.join();
    t2.join();

    EXPECT_EQ(n_elements * 2, map.size());
}
// Test that performing concurrent, overlapping insert (same) is thread-safe.
TEST_F(AtomicUnorderedMapTest, ConcurrentOverlappingInsert) {
    // Add 10 elements from two threads, but starting from the same offset.
    // Should result in only 10 elements existing at the end.
    const size_t n_elements{10};
    std::thread t1{insert_into_map, std::ref(map), n_elements, 0};
    std::thread t2{insert_into_map, std::ref(map), n_elements, 0};
    t1.join();
    t2.join();

    EXPECT_EQ(n_elements, map.size());
}
