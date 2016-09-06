/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
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

#include <algorithm>
#include <queue>
#include <sstream>

#include "statwriter.h"
#include "taskqueue.h"
#include "executorpool.h"
#include "executorthread.h"

Mutex ExecutorPool::initGuard;
ExecutorPool *ExecutorPool::instance = NULL;

static const size_t EP_MIN_NUM_THREADS    = 10;
static const size_t EP_MIN_READER_THREADS = 4;
static const size_t EP_MIN_WRITER_THREADS = 4;

static const size_t EP_MAX_READER_THREADS = 12;
static const size_t EP_MAX_WRITER_THREADS = 8;
static const size_t EP_MAX_AUXIO_THREADS  = 8;
static const size_t EP_MAX_NONIO_THREADS  = 8;

size_t ExecutorPool::getNumCPU(void) {
    size_t numCPU;
#ifdef WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    numCPU = (size_t)sysinfo.dwNumberOfProcessors;
#else
    numCPU = (size_t)sysconf(_SC_NPROCESSORS_ONLN);
#endif

    return (numCPU < 256) ? numCPU : 0;
}

size_t ExecutorPool::getNumNonIO(void) {
    // 1. compute: ceil of 10% of total threads
    size_t count = maxGlobalThreads / 10;
    if (!count || maxGlobalThreads % 10) {
        count++;
    }
    // 2. adjust computed value to be within range
    if (count > EP_MAX_NONIO_THREADS) {
        count = EP_MAX_NONIO_THREADS;
    }
    // 3. pick user's value if specified
    if (maxWorkers[NONIO_TASK_IDX]) {
        count = maxWorkers[NONIO_TASK_IDX];
    }
    return count;
}

size_t ExecutorPool::getNumAuxIO(void) {
    // 1. compute: ceil of 10% of total threads
    size_t count = maxGlobalThreads / 10;
    if (!count || maxGlobalThreads % 10) {
        count++;
    }
    // 2. adjust computed value to be within range
    if (count > EP_MAX_AUXIO_THREADS) {
        count = EP_MAX_AUXIO_THREADS;
    }
    // 3. Override with user's value if specified
    if (maxWorkers[AUXIO_TASK_IDX]) {
        count = maxWorkers[AUXIO_TASK_IDX];
    }
    return count;
}

size_t ExecutorPool::getNumWriters(void) {
    size_t count = 0;
    // 1. compute: floor of Half of what remains after nonIO, auxIO threads
    if (maxGlobalThreads > (getNumAuxIO() + getNumNonIO())) {
        count = maxGlobalThreads - getNumAuxIO() - getNumNonIO();
        count = count >> 1;
    }
    // 2. adjust computed value to be within range
    if (count > EP_MAX_WRITER_THREADS) {
        count = EP_MAX_WRITER_THREADS;
    } else if (count < EP_MIN_WRITER_THREADS) {
        count = EP_MIN_WRITER_THREADS;
    }
    // 3. Override with user's value if specified
    if (maxWorkers[WRITER_TASK_IDX]) {
        count = maxWorkers[WRITER_TASK_IDX];
    }
    return count;
}

size_t ExecutorPool::getNumReaders(void) {
    size_t count = 0;
    // 1. compute: what remains after writers, nonIO & auxIO threads are taken
    if (maxGlobalThreads >
            (getNumWriters() + getNumAuxIO() + getNumNonIO())) {
        count = maxGlobalThreads
              - getNumWriters() - getNumAuxIO() - getNumNonIO();
    }
    // 2. adjust computed value to be within range
    if (count > EP_MAX_READER_THREADS) {
        count = EP_MAX_READER_THREADS;
    } else if (count < EP_MIN_READER_THREADS) {
        count = EP_MIN_READER_THREADS;
    }
    // 3. Override with user's value if specified
    if (maxWorkers[READER_TASK_IDX]) {
        count = maxWorkers[READER_TASK_IDX];
    }
    return count;
}

ExecutorPool *ExecutorPool::get(void) {
    if (!instance) {
        LockHolder lh(initGuard);
        if (!instance) {
            Configuration &config =
                ObjectRegistry::getCurrentEngine()->getConfiguration();
            EventuallyPersistentEngine *epe =
                                   ObjectRegistry::onSwitchThread(NULL, true);
            instance = new ExecutorPool(config.getMaxThreads(),
                    NUM_TASK_GROUPS, config.getMaxNumReaders(),
                    config.getMaxNumWriters(), config.getMaxNumAuxio(),
                    config.getMaxNumNonio());
            ObjectRegistry::onSwitchThread(epe);
        }
    }
    return instance;
}

ExecutorPool::ExecutorPool(size_t maxThreads, size_t nTaskSets,
                           size_t maxReaders, size_t maxWriters,
                           size_t maxAuxIO,   size_t maxNonIO) :
                  numTaskSets(nTaskSets), totReadyTasks(0),
                  isHiPrioQset(false), isLowPrioQset(false), numBuckets(0),
                  numSleepers(0) {
    size_t numCPU = getNumCPU();
    size_t numThreads = (size_t)((numCPU * 3)/4);
    numThreads = (numThreads < EP_MIN_NUM_THREADS) ?
                        EP_MIN_NUM_THREADS : numThreads;
    maxGlobalThreads = maxThreads ? maxThreads : numThreads;
    curWorkers  = new AtomicValue<uint16_t>[nTaskSets];
    maxWorkers  = new AtomicValue<uint16_t>[nTaskSets];
    numReadyTasks  = new AtomicValue<size_t>[nTaskSets];
    for (size_t i = 0; i < nTaskSets; i++) {
        curWorkers[i] = 0;
        numReadyTasks[i] = 0;
    }
    maxWorkers[WRITER_TASK_IDX] = maxWriters;
    maxWorkers[READER_TASK_IDX] = maxReaders;
    maxWorkers[AUXIO_TASK_IDX]  = maxAuxIO;
    maxWorkers[NONIO_TASK_IDX]  = maxNonIO;
}

ExecutorPool::~ExecutorPool(void) {
    delete [] curWorkers;
    free(maxWorkers);
    if (isHiPrioQset) {
        for (size_t i = 0; i < numTaskSets; i++) {
            delete hpTaskQ[i];
        }
    }
    if (isLowPrioQset) {
        for (size_t i = 0; i < numTaskSets; i++) {
            delete lpTaskQ[i];
        }
    }
}

// To prevent starvation of low priority queues, we define their
// polling frequencies as follows ...
#define LOW_PRIORITY_FREQ 5 // 1 out of 5 times threads check low priority Q

TaskQueue *ExecutorPool::_nextTask(ExecutorThread &t, uint8_t tick) {
    if (!tick) {
        return NULL;
    }

    unsigned int myq = t.startIndex;
    TaskQueue *checkQ; // which TaskQueue set should be polled first
    TaskQueue *checkNextQ; // which set of TaskQueue should be polled next
    TaskQueue *toggle = NULL;
    if ( !(tick % LOW_PRIORITY_FREQ)) { // if only 1 Q set, both point to it
        checkQ = isLowPrioQset ? lpTaskQ[myq] :
                (isHiPrioQset ? hpTaskQ[myq] : NULL);
        checkNextQ = isHiPrioQset ? hpTaskQ[myq] : checkQ;
    } else {
        checkQ = isHiPrioQset ? hpTaskQ[myq] :
                (isLowPrioQset ? lpTaskQ[myq] : NULL);
        checkNextQ = isLowPrioQset ? lpTaskQ[myq] : checkQ;
    }
    while (t.state == EXECUTOR_RUNNING) {
        if (checkQ &&
            checkQ->fetchNextTask(t, false)) {
            return checkQ;
        }
        if (toggle || checkQ == checkNextQ) {
            TaskQueue *sleepQ = getSleepQ(myq);
            if (sleepQ->fetchNextTask(t, true)) {
                return sleepQ;
            } else {
                return NULL;
            }
        }
        toggle = checkQ;
        checkQ = checkNextQ;
        checkNextQ = toggle;
    }
    return NULL;
}

TaskQueue *ExecutorPool::nextTask(ExecutorThread &t, uint8_t tick) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    TaskQueue *tq = _nextTask(t, tick);
    ObjectRegistry::onSwitchThread(epe);
    return tq;
}

void ExecutorPool::addWork(size_t newWork, task_type_t qType) {
    if (newWork) {
        totReadyTasks.fetch_add(newWork);
        numReadyTasks[qType].fetch_add(newWork);
    }
}

void ExecutorPool::lessWork(task_type_t qType) {
    cb_assert(numReadyTasks[qType].load());
    numReadyTasks[qType]--;
    totReadyTasks--;
}

void ExecutorPool::doneWork(task_type_t &curTaskType) {
    if (curTaskType != NO_TASK_TYPE) {
        // Record that a thread is done working on a particular queue type
        LOG(EXTENSION_LOG_DEBUG, "Done with Task Type %d capacity = %d",
                curTaskType, curWorkers[curTaskType].load());
        curWorkers[curTaskType]--;
        curTaskType = NO_TASK_TYPE;
    }
}

task_type_t ExecutorPool::tryNewWork(task_type_t newTaskType) {
    task_type_t ret = newTaskType;
    curWorkers[newTaskType]++; // atomic increment
    // Test if a thread can take up task from the target Queue type
    if (curWorkers[newTaskType] <= maxWorkers[newTaskType]) {
        // Ok to proceed as limit not hit
        LOG(EXTENSION_LOG_DEBUG,
                "Taking up work in task type %d capacity = %d, max=%d",
                newTaskType, curWorkers[newTaskType].load(),
                maxWorkers[newTaskType].load());
    } else {
        curWorkers[newTaskType]--; // do not exceed the limit at maxWorkers
        LOG(EXTENSION_LOG_DEBUG, "Limiting from taking up work in task "
                "type %d capacity = %d, max = %d", newTaskType,
                curWorkers[newTaskType].load(),
                maxWorkers[newTaskType].load());
        ret = NO_TASK_TYPE;
    }

    return ret;
}

bool ExecutorPool::_cancel(size_t taskId, bool eraseTask) {
    LockHolder lh(tMutex);
    std::map<size_t, TaskQpair>::iterator itr = taskLocator.find(taskId);
    if (itr == taskLocator.end()) {
        LOG(EXTENSION_LOG_DEBUG, "Task id %d not found");
        return false;
    }

    ExTask task = itr->second.first;
    LOG(EXTENSION_LOG_DEBUG, "Cancel task %s id %d on bucket %s %s",
            task->getDescription().c_str(), task->getId(),
            task->getEngine()->getName(), eraseTask ? "final erase" : "!");

    task->cancel(); // must be idempotent, just set state to dead

    if (eraseTask) { // only internal threads can erase tasks
        cb_assert(task->isdead());
        taskLocator.erase(itr);
        tMutex.notify();
    } else { // wake up the task from the TaskQ so a thread can safely erase it
             // otherwise we may race with unregisterBucket where a unlocated
             // task runs in spite of its bucket getting unregistered
        itr->second.second->wake(task);
    }
    return true;
}

bool ExecutorPool::cancel(size_t taskId, bool eraseTask) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    bool rv = _cancel(taskId, eraseTask);
    ObjectRegistry::onSwitchThread(epe);
    return rv;
}

bool ExecutorPool::_wake(size_t taskId) {
    LockHolder lh(tMutex);
    std::map<size_t, TaskQpair>::iterator itr = taskLocator.find(taskId);
    if (itr != taskLocator.end()) {
        itr->second.second->wake(itr->second.first);
        return true;
    }
    return false;
}

bool ExecutorPool::wake(size_t taskId) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    bool rv = _wake(taskId);
    ObjectRegistry::onSwitchThread(epe);
    return rv;
}

bool ExecutorPool::_snooze(size_t taskId, double tosleep) {
    LockHolder lh(tMutex);
    std::map<size_t, TaskQpair>::iterator itr = taskLocator.find(taskId);
    if (itr != taskLocator.end()) {
        itr->second.first->snooze(tosleep);
        return true;
    }
    return false;
}

bool ExecutorPool::snooze(size_t taskId, double tosleep) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    bool rv = _snooze(taskId, tosleep);
    ObjectRegistry::onSwitchThread(epe);
    return rv;
}

TaskQueue* ExecutorPool::_getTaskQueue(EventuallyPersistentEngine *e,
                                       task_type_t qidx) {
    TaskQueue         *q             = NULL;
    size_t            curNumThreads  = 0;
    bucket_priority_t bucketPriority = e->getWorkloadPriority();

    cb_assert(0 <= (int)qidx && (size_t)qidx < numTaskSets);

    curNumThreads = threadQ.size();

    if (!bucketPriority) {
        LOG(EXTENSION_LOG_WARNING, "Trying to schedule task for unregistered "
            "bucket %s", e->getName());
        return q;
    }

    if (curNumThreads < maxGlobalThreads) {
        if (isHiPrioQset) {
            q = hpTaskQ[qidx];
        } else if (isLowPrioQset) {
            q = lpTaskQ[qidx];
        }
    } else { // Max capacity Mode scheduling ...
        if (bucketPriority == LOW_BUCKET_PRIORITY) {
            cb_assert(lpTaskQ.size() == numTaskSets);
            q = lpTaskQ[qidx];
        } else {
            cb_assert(hpTaskQ.size() == numTaskSets);
            q = hpTaskQ[qidx];
        }
    }
    return q;
}

size_t ExecutorPool::_schedule(ExTask task, task_type_t qidx) {
    LockHolder lh(tMutex);
    TaskQueue *q = _getTaskQueue(task->getEngine(), qidx);
    TaskQpair tqp(task, q);
    taskLocator[task->getId()] = tqp;

    q->schedule(task);

    return task->getId();
}

size_t ExecutorPool::schedule(ExTask task, task_type_t qidx) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    size_t rv = _schedule(task, qidx);
    ObjectRegistry::onSwitchThread(epe);
    return rv;
}

void ExecutorPool::_registerBucket(EventuallyPersistentEngine *engine) {
    TaskQ *taskQ;
    bool *whichQset;
    const char *queueName;
    WorkLoadPolicy &workload = engine->getWorkLoadPolicy();
    bucket_priority_t priority = workload.getBucketPriority();

    if (priority < HIGH_BUCKET_PRIORITY) {
        engine->setWorkloadPriority(LOW_BUCKET_PRIORITY);
        taskQ = &lpTaskQ;
        whichQset = &isLowPrioQset;
        queueName = "LowPrioQ_";
        LOG(EXTENSION_LOG_WARNING, "Bucket %s registered with low priority",
            engine->getName());
    } else {
        engine->setWorkloadPriority(HIGH_BUCKET_PRIORITY);
        taskQ = &hpTaskQ;
        whichQset = &isHiPrioQset;
        queueName = "HiPrioQ_";
        LOG(EXTENSION_LOG_WARNING, "Bucket %s registered with high priority",
            engine->getName());
    }

    LockHolder lh(tMutex);

    if (!(*whichQset)) {
        taskQ->reserve(numTaskSets);
        for (size_t i = 0; i < numTaskSets; i++) {
            taskQ->push_back(new TaskQueue(this, (task_type_t)i, queueName));
        }
        *whichQset = true;
    }

    buckets.insert(engine);
    numBuckets++;

    _startWorkers();
}

void ExecutorPool::registerBucket(EventuallyPersistentEngine *engine) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    _registerBucket(engine);
    ObjectRegistry::onSwitchThread(epe);
}

bool ExecutorPool::_startWorkers(void) {
    if (threadQ.size()) {
        return false;
    }

    size_t numReaders = getNumReaders();
    size_t numWriters = getNumWriters();
    size_t numAuxIO   = getNumAuxIO();
    size_t numNonIO   = getNumNonIO();

    std::stringstream ss;
    ss << "Spawning " << numReaders << " readers, " << numWriters <<
    " writers, " << numAuxIO << " auxIO, " << numNonIO << " nonIO threads";
    LOG(EXTENSION_LOG_WARNING, ss.str().c_str());

    for (size_t tidx = 0; tidx < numReaders; ++tidx) {
        std::stringstream ss;
        ss << "reader_worker_" << tidx;

        threadQ.push_back(new ExecutorThread(this, READER_TASK_IDX, ss.str()));
        threadQ.back()->start();
    }
    for (size_t tidx = 0; tidx < numWriters; ++tidx) {
        std::stringstream ss;
        ss << "writer_worker_" << numReaders + tidx;

        threadQ.push_back(new ExecutorThread(this, WRITER_TASK_IDX, ss.str()));
        threadQ.back()->start();
    }
    for (size_t tidx = 0; tidx < numAuxIO; ++tidx) {
        std::stringstream ss;
        ss << "auxio_worker_" << numReaders + numWriters + tidx;

        threadQ.push_back(new ExecutorThread(this, AUXIO_TASK_IDX, ss.str()));
        threadQ.back()->start();
    }
    for (size_t tidx = 0; tidx < numNonIO; ++tidx) {
        std::stringstream ss;
        ss << "nonio_worker_" << numReaders + numWriters + numAuxIO + tidx;

        threadQ.push_back(new ExecutorThread(this, NONIO_TASK_IDX, ss.str()));
        threadQ.back()->start();
    }

    if (!maxWorkers[WRITER_TASK_IDX]) {
        // MB-12279: Limit writers to 4 for faster bgfetches in DGM by default
        numWriters = 4;
    }
    maxWorkers[WRITER_TASK_IDX] = numWriters;
    maxWorkers[READER_TASK_IDX] = numReaders;
    maxWorkers[AUXIO_TASK_IDX]  = numAuxIO;
    maxWorkers[NONIO_TASK_IDX]  = numNonIO;

    return true;
}

bool ExecutorPool::_stopTaskGroup(EventuallyPersistentEngine *e,
                                  task_type_t taskType,
                                  bool force) {
    bool unfinishedTask;
    bool retVal = false;
    std::map<size_t, TaskQpair>::iterator itr;

    LockHolder lh(tMutex);
    LOG(EXTENSION_LOG_DEBUG, "Stopping %d type tasks in bucket %s", taskType,
            e->getName());
    do {
        ExTask task;
        unfinishedTask = false;
        for (itr = taskLocator.begin(); itr != taskLocator.end(); itr++) {
            task = itr->second.first;
            TaskQueue *q = itr->second.second;
            if (task->getEngine() == e &&
                (taskType == NO_TASK_TYPE || q->queueType == taskType)) {
                LOG(EXTENSION_LOG_DEBUG, "Stopping Task id %d %s ",
                        task->getId(), task->getDescription().c_str());
                // If force flag is set during shutdown, cancel all tasks
                // without considering the blockShutdown status of the task.
                if (force || !task->blockShutdown) {
                    task->cancel(); // Must be idempotent
                }
                q->wake(task);
                unfinishedTask = true;
                retVal = true;
            }
        }
        if (unfinishedTask) {
            struct timeval waktime;
            gettimeofday(&waktime, NULL);
            advance_tv(waktime, MIN_SLEEP_TIME);
            tMutex.wait(waktime); // Wait till task gets cancelled
        }
    } while (unfinishedTask);

    return retVal;
}

bool ExecutorPool::stopTaskGroup(EventuallyPersistentEngine *e,
                                 task_type_t taskType,
                                 bool force) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    bool rv = _stopTaskGroup(e, taskType, force);
    ObjectRegistry::onSwitchThread(epe);
    return rv;
}

void ExecutorPool::_unregisterBucket(EventuallyPersistentEngine *engine,
                                     bool force) {

    LOG(EXTENSION_LOG_WARNING, "Unregistering %s bucket %s",
            (numBuckets == 1)? "last" : "", engine->getName());

    _stopTaskGroup(engine, NO_TASK_TYPE, force);

    LockHolder lh(tMutex);

    buckets.erase(engine);
    if (!(--numBuckets)) {
        assert (!taskLocator.size());
        for (unsigned int idx = 0; idx < numTaskSets; idx++) {
            TaskQueue *sleepQ = getSleepQ(idx);
            size_t wakeAll = threadQ.size();
            numReadyTasks[idx]++; // this prevents woken workers from sleeping
            totReadyTasks++;
            sleepQ->doWake(wakeAll);
        }
        for (size_t tidx = 0; tidx < threadQ.size(); ++tidx) {
            threadQ[tidx]->stop(false); // only set state to DEAD
        }
        for (unsigned int idx = 0; idx < numTaskSets; idx++) {
            numReadyTasks[idx]--; // once woken reset the ready tasks
            totReadyTasks--;
        }

        for (size_t tidx = 0; tidx < threadQ.size(); ++tidx) {
            threadQ[tidx]->stop(/*wait for threads */);
            delete threadQ[tidx];
        }

        for (size_t i = 0; i < numTaskSets; i++) {
            curWorkers[i] = 0;
        }

        threadQ.clear();
        if (isHiPrioQset) {
            for (size_t i = 0; i < numTaskSets; i++) {
                delete hpTaskQ[i];
            }
            hpTaskQ.clear();
            isHiPrioQset = false;
        }
        if (isLowPrioQset) {
            for (size_t i = 0; i < numTaskSets; i++) {
                delete lpTaskQ[i];
            }
            lpTaskQ.clear();
            isLowPrioQset = false;
        }
    }
}

void ExecutorPool::unregisterBucket(EventuallyPersistentEngine *engine,
                                    bool force) {
    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    _unregisterBucket(engine, force);
    ObjectRegistry::onSwitchThread(epe);
}

void ExecutorPool::doTaskQStat(EventuallyPersistentEngine *engine,
                               const void *cookie, ADD_STAT add_stat) {
    if (engine->getEpStats().isShutdown) {
        return;
    }

    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    char statname[80] = {0};
    if (isHiPrioQset) {
        for (size_t i = 0; i < numTaskSets; i++) {
            snprintf(statname, sizeof(statname), "ep_workload:%s:InQsize",
                     hpTaskQ[i]->getName().c_str());
            add_casted_stat(statname, hpTaskQ[i]->getFutureQueueSize(), add_stat,
                            cookie);
            snprintf(statname, sizeof(statname), "ep_workload:%s:OutQsize",
                     hpTaskQ[i]->getName().c_str());
            add_casted_stat(statname, hpTaskQ[i]->getReadyQueueSize(), add_stat,
                            cookie);
            size_t pendingQsize = hpTaskQ[i]->getPendingQueueSize();
            if (pendingQsize > 0) {
                snprintf(statname, sizeof(statname), "ep_workload:%s:PendingQ",
                        hpTaskQ[i]->getName().c_str());
                add_casted_stat(statname, pendingQsize, add_stat, cookie);
            }
        }
    }
    if (isLowPrioQset) {
        for (size_t i = 0; i < numTaskSets; i++) {
            snprintf(statname, sizeof(statname), "ep_workload:%s:InQsize",
                     lpTaskQ[i]->getName().c_str());
            add_casted_stat(statname, lpTaskQ[i]->getFutureQueueSize(), add_stat,
                            cookie);
            snprintf(statname, sizeof(statname), "ep_workload:%s:OutQsize",
                     lpTaskQ[i]->getName().c_str());
            add_casted_stat(statname, lpTaskQ[i]->getReadyQueueSize(), add_stat,
                            cookie);
            size_t pendingQsize = lpTaskQ[i]->getPendingQueueSize();
            if (pendingQsize > 0) {
                snprintf(statname, sizeof(statname), "ep_workload:%s:PendingQ",
                        lpTaskQ[i]->getName().c_str());
                add_casted_stat(statname, pendingQsize, add_stat, cookie);
            }
        }
    }
    ObjectRegistry::onSwitchThread(epe);
}

static void showJobLog(const char *logname, const char *prefix,
                       const std::vector<TaskLogEntry> &log,
                       const void *cookie, ADD_STAT add_stat) {
    char statname[80] = {0};
    for (size_t i = 0;i < log.size(); ++i) {
        snprintf(statname, sizeof(statname), "%s:%s:%d:task", prefix,
                logname, static_cast<int>(i));
        add_casted_stat(statname, log[i].getName().c_str(), add_stat,
                        cookie);
        snprintf(statname, sizeof(statname), "%s:%s:%d:type", prefix,
                logname, static_cast<int>(i));
        add_casted_stat(statname,
                        TaskQueue::taskType2Str(log[i].getTaskType()).c_str(),
                        add_stat, cookie);
        snprintf(statname, sizeof(statname), "%s:%s:%d:starttime",
                prefix, logname, static_cast<int>(i));
        add_casted_stat(statname, log[i].getTimestamp(), add_stat,
                cookie);
        snprintf(statname, sizeof(statname), "%s:%s:%d:runtime",
                prefix, logname, static_cast<int>(i));
        add_casted_stat(statname, log[i].getDuration(), add_stat,
                cookie);
    }
}

static void addWorkerStats(const char *prefix, ExecutorThread *t,
                           const void *cookie, ADD_STAT add_stat) {
    char statname[80] = {0};
    snprintf(statname, sizeof(statname), "%s:state", prefix);
    add_casted_stat(statname, t->getStateName().c_str(), add_stat, cookie);
    snprintf(statname, sizeof(statname), "%s:task", prefix);
    add_casted_stat(statname, t->getTaskName().c_str(), add_stat, cookie);

    if (strcmp(t->getStateName().c_str(), "running") == 0) {
        snprintf(statname, sizeof(statname), "%s:runtime", prefix);
        add_casted_stat(statname,
                (gethrtime() - t->getTaskStart()) / 1000, add_stat, cookie);
    }
    snprintf(statname, sizeof(statname), "%s:waketime", prefix);
    uint64_t abstime = t->getWaketime().tv_sec*1000000 +
                       t->getWaketime().tv_usec;
    add_casted_stat(statname, abstime, add_stat, cookie);
    snprintf(statname, sizeof(statname), "%s:cur_time", prefix);
    abstime = t->getCurTime().tv_sec*1000000 +
              t->getCurTime().tv_usec;
    add_casted_stat(statname, abstime, add_stat, cookie);
}

void ExecutorPool::doWorkerStat(EventuallyPersistentEngine *engine,
                               const void *cookie, ADD_STAT add_stat) {
    if (engine->getEpStats().isShutdown) {
        return;
    }

    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    //TODO: implement tracking per engine stats ..
    for (size_t tidx = 0; tidx < threadQ.size(); ++tidx) {
        addWorkerStats(threadQ[tidx]->getName().c_str(), threadQ[tidx],
                     cookie, add_stat);
        showJobLog("log", threadQ[tidx]->getName().c_str(),
                   threadQ[tidx]->getLog(), cookie, add_stat);
        showJobLog("slow", threadQ[tidx]->getName().c_str(),
                   threadQ[tidx]->getSlowLog(), cookie, add_stat);
    }
    ObjectRegistry::onSwitchThread(epe);
}
