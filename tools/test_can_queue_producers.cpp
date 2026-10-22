#include "isobus/utility/thread_synchronization.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    constexpr unsigned producers = 4, perProducer = 50000;
    LockFreeQueue<unsigned> queue(128);
    std::atomic<unsigned> ready{0}, finished{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (unsigned p = 0; p < producers; ++p)
        threads.emplace_back([&, p] {
            ++ready;
            while (!go) std::this_thread::yield();
            for (unsigned n = 0; n < perProducer; ++n)
                while (!queue.push(p * perProducer + n)) std::this_thread::yield();
            ++finished;
        });
    while (ready != producers) std::this_thread::yield();
    go = true;
    std::vector<bool> seen(producers * perProducer, false);
    unsigned count = 0, duplicates = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
        unsigned value;
        if (queue.peek(value))
        {
            queue.pop();
            if (value >= seen.size() || seen[value]) ++duplicates;
            else seen[value] = true;
            ++count;
        }
        else if (finished == producers) break;
        else std::this_thread::yield();
    }
    for (auto &thread : threads) thread.join();
    std::cout << "received=" << count << " expected=" << seen.size()
              << " duplicate_or_corrupt=" << duplicates << '\n';
    return (count == seen.size() && duplicates == 0) ? 0 : 1;
}
