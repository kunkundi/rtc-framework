#pragma once

#include <mutex>
#include <condition_variable>

class semaphore {
public:
	semaphore(long count = 0) :count(count)
	{
	}

	void wait() {
		std::unique_lock<std::mutex>lock(mx);
		cond.wait(lock, [&]() {return count > 0; });
		--count;
	}
	void signal() {
		std::unique_lock<std::mutex>lock(mx);
		++count;
		cond.notify_one();
	}

private:
	std::mutex mx;
	std::condition_variable cond;
	long count;
};