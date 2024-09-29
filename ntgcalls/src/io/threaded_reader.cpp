//
// Created by Laky64 on 28/09/24.
//

#include <thread>
#include <ntgcalls/io/threaded_reader.hpp>

namespace ntgcalls {
    ThreadedReader::ThreadedReader(BaseSink *sink, const size_t bufferCount): BaseReader(sink) {
        bufferThreads.reserve(bufferCount);
    }

    ThreadedReader::~ThreadedReader() {
        running = false;
        cv.notify_all();
        for (auto& thread : bufferThreads) {
            thread.Finalize();
        }
    }

    void ThreadedReader::open() {
        const size_t bufferCount = bufferThreads.capacity();
        running = true;
        auto frameSize = sink->frameSize();
        auto frameTime = sink->frameTime();
        for (size_t i = 0; i < bufferCount; ++i) {
            bufferThreads.push_back(
                rtc::PlatformThread::SpawnJoinable(
                    [this, i, frameSize, frameTime] {
                        activeBufferCount++;
                        while (running) {
                            std::unique_lock lock(mtx);
                            try {
                                dataCallback(std::move(read(frameSize)));
                            } catch (...) {
                                break;
                            }
                            cv.wait(lock, [this, i] {
                                return !running || activeBuffer == i;
                            });
                            if (!running) break;
                            activeBuffer = (activeBuffer + 1) % bufferThreads.size();
                            lock.unlock();
                            cv.notify_all();
                            std::this_thread::sleep_for(frameTime);
                        }
                        activeBufferCount--;
                        if (activeBufferCount == 0) {
                            (void) eofCallback();
                        }
                    },
                    "ThreadedReader_" + std::to_string(bufferCount),
                    rtc::ThreadAttributes().SetPriority(rtc::ThreadPriority::kRealtime)
                )
            );
        }
    }
} // ntgcalls