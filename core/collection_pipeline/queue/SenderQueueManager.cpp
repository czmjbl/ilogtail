// Copyright 2024 iLogtail Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "collection_pipeline/queue/SenderQueueManager.h"

#include "collection_pipeline/queue/ExactlyOnceQueueManager.h"
#include "collection_pipeline/queue/QueueKeyManager.h"
#include "common/Flags.h"

DEFINE_FLAG_INT32(sender_queue_gc_threshold_sec, "30s", 30);
DEFINE_FLAG_INT32(sender_queue_capacity, "", 15);

using namespace std;

namespace logtail {

SenderQueueManager::SenderQueueManager() : mDefaultQueueParam(INT32_FLAG(sender_queue_capacity), 1.0) {
}

bool SenderQueueManager::CreateQueue(
    QueueKey key,
    const string& flusherId,
    const CollectionPipelineContext& ctx,
    std::unordered_map<std::string, std::shared_ptr<ConcurrencyLimiter>>&& concurrencyLimitersMap,
    uint32_t maxRate) {
    lock_guard<mutex> lock(mQueueMux);
    auto iter = mQueues.find(key);
    if (iter == mQueues.end()) {
        mQueues.try_emplace(key,
                            mDefaultQueueParam.GetCapacity(),
                            mDefaultQueueParam.GetLowWatermark(),
                            mDefaultQueueParam.GetHighWatermark(),
                            key,
                            flusherId,
                            ctx);
        iter = mQueues.find(key);
    }
    iter->second.SetConcurrencyLimiters(std::move(concurrencyLimitersMap));
    iter->second.SetRateLimiter(maxRate);
    return true;
}

SenderQueue* SenderQueueManager::GetQueue(QueueKey key) {
    lock_guard<mutex> lock(mQueueMux);
    auto iter = mQueues.find(key);
    if (iter != mQueues.end()) {
        return &iter->second;
    }
    return nullptr;
}

bool SenderQueueManager::DeleteQueue(QueueKey key) {
    {
        lock_guard<mutex> lock(mQueueMux);
        auto iter = mQueues.find(key);
        if (iter == mQueues.end()) {
            return false;
        }
    }
    {
        lock_guard<mutex> lock(mGCMux);
        if (mQueueDeletionTimeMap.find(key) != mQueueDeletionTimeMap.end()) {
            return false;
        }
        mQueueDeletionTimeMap[key] = time(nullptr);
    }
    return true;
}

bool SenderQueueManager::ReuseQueue(QueueKey key) {
    lock_guard<mutex> lock(mGCMux);
    auto iter = mQueueDeletionTimeMap.find(key);
    if (iter == mQueueDeletionTimeMap.end()) {
        return false;
    }
    mQueueDeletionTimeMap.erase(iter);
    return true;
}

int SenderQueueManager::PushQueue(QueueKey key, unique_ptr<SenderQueueItem>&& item) {
    {
        lock_guard<mutex> lock(mQueueMux);
        auto iter = mQueues.find(key);
        if (iter != mQueues.end()) {
            if (!iter->second.Push(std::move(item))) {
                return 1;
            }
        } else {
            int res = ExactlyOnceQueueManager::GetInstance()->PushSenderQueue(key, std::move(item));
            if (res != 0) {
                return res;
            }
        }
    }
    Trigger();
    return 0;
}

void SenderQueueManager::GetAvailableItems(vector<SenderQueueItem*>& items, int32_t itemsCntLimit) {
    {
        lock_guard<mutex> lock(mQueueMux);
        if (mQueues.empty()) {
            return;
        }
        if (itemsCntLimit == -1) {
            for (auto iter = mQueues.begin(); iter != mQueues.end(); ++iter) {
                iter->second.GetAvailableItems(items, -1);
            }
        } else {
            int cntLimitPerQueue
                = std::max((int)(mDefaultQueueParam.GetCapacity() * 0.3), (int)(itemsCntLimit / mQueues.size()));
            // must check index before moving iterator
            mSenderQueueBeginIndex = mSenderQueueBeginIndex % mQueues.size();
            // here we set sender queue begin index, let the sender order be different each time
            auto beginIter = mQueues.begin();
            std::advance(beginIter, mSenderQueueBeginIndex++);

            for (auto iter = beginIter; iter != mQueues.end(); ++iter) {
                iter->second.GetAvailableItems(items, cntLimitPerQueue);
            }
            for (auto iter = mQueues.begin(); iter != beginIter; ++iter) {
                iter->second.GetAvailableItems(items, cntLimitPerQueue);
            }
        }
    }
    ExactlyOnceQueueManager::GetInstance()->GetAvailableSenderQueueItems(items, itemsCntLimit);
}

bool SenderQueueManager::RemoveItem(QueueKey key, SenderQueueItem* item) {
    {
        lock_guard<mutex> lock(mQueueMux);
        auto iter = mQueues.find(key);
        if (iter != mQueues.end()) {
            return iter->second.Remove(item);
        }
    }
    return ExactlyOnceQueueManager::GetInstance()->RemoveSenderQueueItem(key, item);
}

void SenderQueueManager::DecreaseConcurrencyLimiterInSendingCnt(QueueKey key) {
    lock_guard<mutex> lock(mQueueMux);
    auto iter = mQueues.find(key);
    if (iter != mQueues.end()) {
        iter->second.DecreaseSendingCnt();
    }
}

bool SenderQueueManager::IsAllQueueEmpty() const {
    {
        lock_guard<mutex> lock(mQueueMux);
        for (const auto& q : mQueues) {
            if (!q.second.Empty()) {
                return false;
            }
        }
    }
    return ExactlyOnceQueueManager::GetInstance()->IsAllSenderQueueEmpty();
}

void SenderQueueManager::ClearUnusedQueues() {
    auto const curTime = time(nullptr);
    lock_guard<mutex> lock(mGCMux);
    auto iter = mQueueDeletionTimeMap.begin();
    while (iter != mQueueDeletionTimeMap.end()) {
        if (!(curTime >= iter->second && curTime - iter->second >= INT32_FLAG(sender_queue_gc_threshold_sec))) {
            ++iter;
            continue;
        }
        {
            lock_guard<mutex> lock(mQueueMux);
            auto itr = mQueues.find(iter->first);
            if (itr == mQueues.end()) {
                // should not happen
                continue;
            }
            if (!itr->second.Empty()) {
                ++iter;
                continue;
            }
            mQueues.erase(itr);
        }
        QueueKeyManager::GetInstance()->RemoveKey(iter->first);
        iter = mQueueDeletionTimeMap.erase(iter);
    }
}

bool SenderQueueManager::IsValidToPush(QueueKey key) const {
    lock_guard<mutex> lock(mQueueMux);
    auto iter = mQueues.find(key);
    if (iter != mQueues.end()) {
        return iter->second.IsValidToPush();
    }
    // no need to check exactly once queue, since the caller does not support exactly once
    // should not happen
    return false;
}

bool SenderQueueManager::Wait(uint64_t ms) {
    // TODO: use semaphore instead
    unique_lock<mutex> lock(mStateMux);
    mCond.wait_for(lock, chrono::milliseconds(ms), [this] { return mValidToPop; });
    if (mValidToPop) {
        mValidToPop = false;
        return true;
    }
    return false;
}

void SenderQueueManager::Trigger() {
    {
        lock_guard<mutex> lock(mStateMux);
        mValidToPop = true;
    }
    mCond.notify_one();
}

void SenderQueueManager::SetPipelineForItems(QueueKey key, const std::shared_ptr<CollectionPipeline>& p) {
    lock_guard<mutex> lock(mQueueMux);
    auto iter = mQueues.find(key);
    if (iter != mQueues.end()) {
        iter->second.SetPipelineForItems(p);
    } else {
        ExactlyOnceQueueManager::GetInstance()->SetPipelineForSenderItems(key, p);
    }
}

#ifdef APSARA_UNIT_TEST_MAIN
void SenderQueueManager::Clear() {
    lock_guard<mutex> lock(mQueueMux);
    mQueues.clear();
    mQueueDeletionTimeMap.clear();
}

bool SenderQueueManager::IsQueueMarkedDeleted(QueueKey key) {
    lock_guard<mutex> lock(mGCMux);
    return mQueueDeletionTimeMap.find(key) != mQueueDeletionTimeMap.end();
}
#endif

} // namespace logtail
