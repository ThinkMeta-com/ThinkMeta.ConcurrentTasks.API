// Tutorial chapter 3: one task per block - the blocks are encoded in parallel,
// main writes them in order as they become ready.
// [names]
#include "tutorial-common.h"

#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

namespace TC = ThinkMeta::ConcurrentTasks;
using namespace Tutorial;
// [/names]

// [task]
// Encodes one block. Blocks are independent, so these tasks can run in any
// order and on any thread.
class EncodeTask : public TC::Tasks::WaitableTask
{
public:
    void Run() noexcept override { EncodeBlock(input, encoded); }

    std::span<const std::byte> input;
    EncodedBlock               encoded;
};
// [/task]

// [packer]
// Holds the file and the tasks. It must outlive every task it started, so main
// destroys it only after the scheduler has stopped.
class Packer
{
public:
    bool Pack(TC::ITaskScheduler& scheduler, const Arguments& arguments)
    {
        File input  = File::OpenRead(arguments.input);
        File output = File::Create(arguments.output);
        if (!input.IsOpen() || !output.IsOpen() || !input.ReadAll(data_))
            return false;

        // Fork: one task per block.
        const std::size_t blockSize = arguments.blockSize;
        const std::size_t count     = (data_.size() + blockSize - 1) / blockSize;
        tasks_ = std::make_unique<EncodeTask[]>(count); // fixed addresses
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t offset = i * blockSize;
            tasks_[i].input          = std::span(data_).subspan(
                offset, std::min(blockSize, data_.size() - offset));
            scheduler.AddTask(&tasks_[i]);
        }

        // Join in order: block i is written as soon as it is ready, while the
        // blocks after it are still being encoded.
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
// [/packer]

// [main]
int wmain(int argc, wchar_t* argv[])
{
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments))
        return arguments.exitCode;

    TC::ConvertThreadToTask();

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    Packer          packer; // lives until after Stop
    const Stopwatch total;
    const bool      packed = packer.Pack(*scheduler, arguments);
    std::printf("time: %.2f s\n", total.Seconds());

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
