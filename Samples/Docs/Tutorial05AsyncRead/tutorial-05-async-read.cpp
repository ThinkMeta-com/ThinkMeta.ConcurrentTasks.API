// Tutorial chapter 5: reading without blocking - a reader task reads the file
// with AsyncFile and starts each block's task as soon as the block is in memory.
// [names]
#include "tutorial-common.h"

#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::Windows::Error;
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

// [reader-task]
class Packer;

// Runs Packer::Read as a task.
class ReaderTask : public TC::Tasks::WaitableTask
{
public:
    explicit ReaderTask(Packer& packer) noexcept : packer_(packer) {}

    void Run() noexcept override;

private:
    Packer& packer_;
};
// [/reader-task]

class Packer
{
public:
    // [pack]
    bool Pack(TC::ITaskScheduler& scheduler, const Arguments& arguments)
    {
        AsyncFile::FileSize size = 0;
        if (Error::Failed(input_.Create(arguments.input, GENERIC_READ,
                                        FILE_SHARE_READ, OPEN_EXISTING,
                                        FILE_FLAG_SEQUENTIAL_SCAN))
            || Error::Failed(input_.GetSize(size)))
            return false;
        File output = File::Create(arguments.output);
        if (!output.IsOpen())
            return false;

        scheduler_ = &scheduler;
        blockSize_ = arguments.blockSize;
        data_.resize(size);
        count_ = (data_.size() + blockSize_ - 1) / blockSize_;
        tasks_ = std::make_unique<EncodeTask[]>(count_);

        // The reader forks the blocks; main only writes. Waiting for a task that
        // the reader has not started yet is fine: the wait ends when it has run.
        scheduler.AddTask(&reader_);
        std::uint32_t crc = 0;
        bool          ok  = true;
        for (std::size_t i = 0; i < count_; ++i) {
            tasks_[i].Wait();
            crc = WriteBlock(output, tasks_[i].encoded, crc, ok);
        }
        reader_.Wait();
        input_.Close(); // no read is pending any more
        return WriteFooter(output, crc) && ok && readOk_;
    }
    // [/pack]

    // [read]
    // Runs as a task: reads block after block and forks its encoding at once.
    // Read suspends the task, not the thread, while the disk works.
    void Read() noexcept
    {
        for (std::size_t i = 0; i < count_; ++i) {
            const std::size_t offset = i * blockSize_;
            const auto        wanted
                = static_cast<DWORD>(std::min(blockSize_, data_.size() - offset));
            DWORD      read  = 0; // DWORD: Read's out parameter
            const auto error = input_.Read(&data_[offset], wanted, read, offset);
            if (Error::Failed(error) || read != wanted) {
                // Main waits for every block, so it still gets one: an empty
                // one. Chapter 9 handles errors properly.
                readOk_ = false;
                read    = 0;
            }
            tasks_[i].input = std::span(data_).subspan(offset, read);
            scheduler_->AddTask(&tasks_[i]);
        }
    }
    // [/read]

private:
    TC::ITaskScheduler*           scheduler_ = nullptr;
    AsyncFile                     input_;
    std::size_t                   blockSize_ = 0;
    std::size_t                   count_     = 0;
    std::vector<std::byte>        data_;
    std::unique_ptr<EncodeTask[]> tasks_;
    ReaderTask                    reader_{*this};
    bool                          readOk_ = true;
};

void ReaderTask::Run() noexcept
{
    packer_.Read();
}

// [main]
int wmain(int argc, wchar_t* argv[])
{
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments))
        return arguments.exitCode;

    const std::uint64_t fileSize = File::OpenRead(arguments.input).Size();
    const std::uint64_t blocks
        = (fileSize + arguments.blockSize - 1) / arguments.blockSize;
    if (blocks > TC::MaximumTaskQueueCapacity) {
        std::printf("%llu blocks are too many; choose larger blocks\n",
                    static_cast<unsigned long long>(blocks));
        return 1;
    }

    TC::ConvertThreadToTask();
    // AsyncFile completes its reads on the core's I/O thread.
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;

    TC::SchedulerInfo info{};
    info.numberOfThreads   = arguments.threads;
    info.taskQueueCapacity = std::max(static_cast<std::uint32_t>(blocks),
                                      TC::DefaultTaskQueueCapacity);
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool packed = false;
    {
        // Lives until after Stop, and its file is gone before DisableAsyncIo.
        Packer          packer;
        const Stopwatch total;
        packed = packer.Pack(*scheduler, arguments);
        std::printf("time: %.2f s\n", total.Seconds());
        scheduler->Stop();
    }

    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();

    if (!packed) {
        std::printf("cannot pack the file\n");
        return 1;
    }
    return CheckOutput(arguments) ? 0 : 1;
}
// [/main]
