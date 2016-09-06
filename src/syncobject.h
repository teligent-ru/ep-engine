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

#ifndef SRC_SYNCOBJECT_H_
#define SRC_SYNCOBJECT_H_ 1

#include "config.h"

#include "utility.h"

#include <condition_variable>

/**
 * Abstraction built on top of std::condition_variable & std::mutex
 */
class SyncObject : public std::mutex {
public:
    SyncObject() {
    }

    ~SyncObject() {
    }

    void wait(std::unique_lock<std::mutex>& lock) {
        cond.wait(lock);
    }

    void wait_for(std::unique_lock<std::mutex>& lock,
                  const double secs) {
        cond.wait_for(lock, std::chrono::milliseconds(int64_t(secs * 1000.0)));
    }

    void wait_for(std::unique_lock<std::mutex>& lock,
                  const hrtime_t nanoSecs) {
        cond.wait_for(lock, std::chrono::nanoseconds(nanoSecs));
    }

    void notify_all() {
        cond.notify_all();
    }

    void notify_one() {
        cond.notify_one();
    }

private:
    std::condition_variable cond;

    DISALLOW_COPY_AND_ASSIGN(SyncObject);
};

#endif  // SRC_SYNCOBJECT_H_
