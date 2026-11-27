#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

// The CPU Queue: pictures and stream events from the decoder, in order, for
// the presentation thread. The decoder only waits while the queue is full, so
// a picture that takes it longer than usual no longer reaches the display
// late: the presentation thread shows the queued ones meanwhile.
//
// Clear (flush, stop) starts a new generation: queued items are dropped, a
// decoder waiting for room returns at once, and an item taken before the
// clear is recognised as stale.
template <class Item> class InboundQueue
{
    struct Entry
    {
        Item item;
        uint64_t generation;
    };

    mutable std::mutex lock;
    std::condition_variable changed;
    std::deque<Entry> items;
    uint64_t generation = 0;
    bool busy = false, closed = false;

  public:
    // Waits while `limit` items are queued. False when a clear or close came
    // first; the item is then dropped.
    bool Push(Item item, int limit)
    {
        std::unique_lock guard(lock);
        const auto mine = generation;
        changed.wait(guard,
                     [&] { return generation != mine || closed || items.size() < size_t((std::max)(limit, 1)); });
        if (generation != mine || closed)
            return false;
        items.push_back({std::move(item), mine});
        changed.notify_all();
        return true;
    }

    // Waits for the next item; false once closed. The item counts as queued
    // until Done.
    bool Pop(Item& item, uint64_t& itemGeneration)
    {
        std::unique_lock guard(lock);
        changed.wait(guard, [&] { return closed || !items.empty(); });
        if (closed)
            return false;
        item = std::move(items.front().item);
        itemGeneration = items.front().generation;
        items.pop_front();
        busy = true;
        changed.notify_all();
        return true;
    }

    // The next item if one is queued, without waiting.
    bool TryPop(Item& item, uint64_t& itemGeneration)
    {
        std::lock_guard guard(lock);
        if (closed || items.empty())
            return false;
        item = std::move(items.front().item);
        itemGeneration = items.front().generation;
        items.pop_front();
        busy = true;
        changed.notify_all();
        return true;
    }

    void Done()
    {
        std::lock_guard guard(lock);
        busy = false;
        changed.notify_all();
    }

    bool Current(uint64_t itemGeneration) const
    {
        std::lock_guard guard(lock);
        return itemGeneration == generation;
    }

    // Drops the queued items; they are released outside the lock.
    void Clear()
    {
        std::deque<Entry> dropped;
        {
            std::lock_guard guard(lock);
            ++generation;
            dropped.swap(items);
            changed.notify_all();
        }
    }

    void Close()
    {
        std::deque<Entry> dropped;
        {
            std::lock_guard guard(lock);
            closed = true;
            dropped.swap(items);
            changed.notify_all();
        }
    }

    // Waits until every queued item has been handled, or a clear or close.
    bool WaitDrained(std::chrono::milliseconds timeout)
    {
        std::unique_lock guard(lock);
        const auto mine = generation;
        return changed.wait_for(
            guard, timeout, [&] { return generation != mine || closed || (items.empty() && !busy); });
    }

    // Queued items, without the one being handled.
    int Size() const
    {
        std::lock_guard guard(lock);
        return int(items.size());
    }
};
