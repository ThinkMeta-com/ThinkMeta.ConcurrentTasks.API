// Tutorial chapter 4: sizing the work - the number of scheduler threads and the
// capacity of the task queues are chosen for the work instead of taken as given.
// [names]
#include "tutorial-common.h"

#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

namespace TC = ThinkMeta::ConcurrentTasks;
using namespace Tutorial;
// [/names]

// Encodes one block, as in chapter 3.
class EncodeTask : public TC::Tasks::WaitableTask
{
public:
    void Run() noexcept override { EncodeBlock(input, encoded); }

    std::span<const std::byte> input;
    EncodedBlock               encoded;
};

// Forks one task per block and writes the blocks in order, as in chapter 3.
class Packer
{
public:
    bool Pack(TC::ITaskScheduler& scheduler, const Arguments& arguments)
    {
        File input  = File::OpenRead(arguments.input);
        File output = File::Create(arguments.output);
        if (!input.IsOpen() || !output.IsOpen() || !input.ReadAll(data_))
            return false;

        const std::size_t blockSize = arguments.blockSize;
        const std::size_t count     = (data_.size() + blockSize - 1) / blockSize;
        tasks_                      = std::make_unique<EncodeTask[]>(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t offset = i * blockSize;
            tasks_[i].input          = std::span(data_).subspan(
                offset, std::min(blockSize, data_.size() - offset));
            scheduler.AddTask(&tasks_[i]);
        }

        std::uint32_t crc = 0;
        bool          ok  = true;
        for (std::size_t i = 0; i < count; ++i) {
            tasks_[i].Wait();
            crc = WriteBlock(output, tasks_[i].encoded, crc, ok);
        }
        return WriteFooter(output, crc) && ok;
    }

private:
    std::vector<std::byte>        data_;
    std::unique_ptr<EncodeTask[]> tasks_;
};

// [main]
int wmain(int argc, wchar_t* argv[])
{
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments))
        return arguments.exitCode;

    // Every block becomes a task, and all of them are runnable at once: the
    // task queues must hold that many.
    const std::uint64_t fileSize = File::OpenRead(arguments.input).Size();
    const std::uint64_t blocks
        = (fileSize + arguments.blockSize - 1) / arguments.blockSize;
    if (blocks > TC::MaximumTaskQueueCapacity) {
        std::printf("%llu blocks are too many; choose larger blocks\n",
                    static_cast<unsigned long long>(blocks));
        return 1;
    }

    TC::ConvertThreadToTask();

    TC::SchedulerInfo info{};
    info.numberOfThreads   = arguments.threads; // 0: one per logical processor
    info.taskQueueCapacity = std::max(static_cast<std::uint32_t>(blocks),
                                      TC::DefaultTaskQueueCapacity);
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    Packer          packer; // lives until after Stop
    const Stopwatch total;
    const bool      packed = packer.Pack(*scheduler, arguments);
    std::printf(
        "time: %.2f s (%llu blocks of %zu KB, %u threads)\n", total.Seconds(),
        static_cast<unsigned long long>(blocks), arguments.blockSize / 1024,
        arguments.threads != 0 ? arguments.threads
                               : GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();

    if (!packed) {
        std::printf("cannot pack the file\n");
        return 1;
    }
    return CheckOutput(arguments) ? 0 : 1;
}
// [/main]
