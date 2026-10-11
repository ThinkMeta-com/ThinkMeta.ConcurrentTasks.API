#include "pch.h"
#include "TestHelpers.h"
#include <thread>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    // Only switches to the other task once a shared "buffer level" reaches a threshold -
    // models the "don't swap reader<->writer until the buffer is full enough" scenario.
    class BufferLevelSwitchPolicy : public ITaskSwitchPolicy
    {
    public:
        BufferLevelSwitchPolicy(std::atomic<int>& bufferLevel, int threshold) noexcept : bufferLevel_(bufferLevel), threshold_(threshold) {}

        bool ShouldSwitch([[maybe_unused]] ITask* currentTask) noexcept override
        {
            return bufferLevel_.load(std::memory_order_acquire) >= threshold_;
        }

    private:
        std::atomic<int>& bufferLevel_;
        int                threshold_;
    };

    class AlwaysSwitchPolicy : public ITaskSwitchPolicy
    {
    public:
        bool ShouldSwitch([[maybe_unused]] ITask* currentTask) noexcept override { return true; }
    };

    TEST_CLASS(TaskSwapSchedulerTests)
    {
    public:
        TEST_METHOD(TestPolicyKeepsRunningCurrentTaskUntilThreshold)
        {
            ConvertThreadToTask();

            constexpr int ReaderIterations = 10;
            constexpr int Threshold        = 5;

            std::atomic<int>  bufferLevel{0};
            std::atomic<bool> readerDone{false};
            std::atomic<bool> writerStarted{false};
            std::atomic<int>  bufferLevelWhenWriterStarted{-1};
            std::atomic<bool> driverRunning{true};

            BufferLevelSwitchPolicy       policy(bufferLevel, Threshold);
            ScopedScheduler scheduler(SchedulerInfo{.taskSwitchPolicy = &policy}, SchedulerKind::TaskSwap);

            Synchronization::AutoResetEvent_s readerGate(false);

            auto readerTask = Tasks::Task::Create([&]() {
                for (int i = 0; i < ReaderIterations; ++i) {
                    bufferLevel.fetch_add(1, std::memory_order_release);
                    // simulate blocking async I/O: suspend here, resumed later from another
                    // thread (the "driver" below stands in for an AsyncIoHandler completion).
                    readerGate.Wait();
                }
                readerDone.store(true, std::memory_order_release);
            });

            auto writerTask = Tasks::Task::Create([&]() {
                bufferLevelWhenWriterStarted.store(bufferLevel.load(std::memory_order_acquire), std::memory_order_release);
                writerStarted.store(true, std::memory_order_release);
            });

            scheduler.Start();
            scheduler.AddTask(&readerTask, TaskPriority::Normal);
            scheduler.AddTask(&writerTask, TaskPriority::Normal);

            std::thread driver([&]() {
                while (driverRunning.load(std::memory_order_acquire)) {
                    readerGate.Set();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });

            const bool finished = WaitFor(
                [&]() { return readerDone.load(std::memory_order_acquire) && writerStarted.load(std::memory_order_acquire); },
                std::chrono::seconds(10));

            driverRunning.store(false, std::memory_order_release);
            driver.join();

            scheduler.Stop();
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"Reader and writer should both complete without timing out");
            Assert::IsTrue(writerStarted.load(), L"Writer should have started");
            Assert::IsTrue(bufferLevelWhenWriterStarted.load() >= Threshold, L"Writer must not start before the buffer threshold is reached");
        }

        TEST_METHOD(TestAlwaysSwitchPolicyLetsBothTasksComplete)
        {
            ConvertThreadToTask();

            constexpr int IterationsPerTask = 5;

            AlwaysSwitchPolicy            policy;
            ScopedScheduler scheduler(SchedulerInfo{.taskSwitchPolicy = &policy}, SchedulerKind::TaskSwap);

            Synchronization::AutoResetEvent_s gateA(false);
            Synchronization::AutoResetEvent_s gateB(false);

            std::atomic<int>  countA{0};
            std::atomic<int>  countB{0};
            std::atomic<bool> driverRunning{true};

            auto taskA = Tasks::Task::Create([&]() {
                // count after the wait: a count of IterationsPerTask then means the task waits no more,
                // so the gates may be destroyed without a waiter
                for (int i = 0; i < IterationsPerTask; ++i) {
                    gateA.Wait();
                    countA.fetch_add(1, std::memory_order_release);
                }
            });

            auto taskB = Tasks::Task::Create([&]() {
                for (int i = 0; i < IterationsPerTask; ++i) {
                    gateB.Wait();
                    countB.fetch_add(1, std::memory_order_release);
                }
            });

            scheduler.Start();
            scheduler.AddTask(&taskA, TaskPriority::Normal);
            scheduler.AddTask(&taskB, TaskPriority::Normal);

            std::thread driver([&]() {
                while (driverRunning.load(std::memory_order_acquire)) {
                    gateA.Set();
                    gateB.Set();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });

            const bool finished = WaitFor(
                [&]() { return countA.load(std::memory_order_acquire) == IterationsPerTask && countB.load(std::memory_order_acquire) == IterationsPerTask; },
                std::chrono::seconds(10));

            driverRunning.store(false, std::memory_order_release);
            driver.join();

            scheduler.Stop();
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"Both tasks should complete without timing out");
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Tests
