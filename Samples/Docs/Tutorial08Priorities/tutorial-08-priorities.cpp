// Tutorial chapter 8: the critical path - the reader runs at high priority, so
// that it gets a thread as soon as its read completes instead of queuing behind
// the encode tasks.
// [names]
#include "tutorial-common.h"

#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::Windows::Error;
using namespace Tutorial;
// [/names]

// Encodes one block and computes its CRC, as in chapter 7.
class EncodeTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        EncodeBlock(input, encoded);
        crc = BlockCrc(encoded);
    }

    std::span<const std::byte> input;
    EncodedBlock               encoded;
    std::uint32_t              crc = 0;
};

struct Slot {
    std::vector<std::byte>           buffer;
    std::size_t                      size = 0;
    EncodeTask                       task;
    TC::Synchronization::TaskGroup_s encoded;
};

class Packer;

class ReaderTask : public TC::Tasks::WaitableTask
{
public:
    explicit ReaderTask(Packer& packer) noexcept : packer_(packer) {}

    void Run() noexcept override;

private:
    Packer& packer_;
};

class Packer
{
public:
    bool Pack(TC::ITaskScheduler& scheduler, const Arguments& arguments,
              std::size_t slotCount)
    {
        if (Error::Failed(input_.Create(arguments.input, GENERIC_READ,
                                        FILE_SHARE_READ, OPEN_EXISTING,
                                        FILE_FLAG_SEQUENTIAL_SCAN)))
            return false;
        File output = File::Create(arguments.output);
        if (!output.IsOpen())
            return false;

        scheduler_ = &scheduler;
        slotCount_ = slotCount;
        slots_     = std::make_unique<Slot[]>(slotCount);
        for (std::size_t i = 0; i < slotCount; ++i)
            slots_[i].buffer.resize(arguments.blockSize);
        freeSlots_.Release(slotCount);

        // [start-reader]
        // The reader is the serial stage that feeds everything else: High.
        scheduler.AddTask(&reader_, TC::TaskPriority::High);
        // [/start-reader]
        std::uint32_t crc = 0;
        bool          ok  = true;
        for (std::size_t block = 0;; ++block) {
            readySlots_.Wait();
            Slot& slot = slots_[block % slotCount];
            if (slot.size == 0)
                break;
            slot.encoded.Wait();
            const EncodedBlock& encoded = slot.task.encoded;
            ok                          = ok && WriteBlockData(output, encoded);
            crc = CombineCrc32(crc, slot.task.crc, BlockBytes(encoded));
            freeSlots_.Release();
        }
        reader_.Wait();
        input_.Close();
        return WriteFooter(output, crc) && ok && readOk_;
    }

    // [read]
    void Read() noexcept
    {
        for (std::size_t block = 0;; ++block) {
            freeSlots_.Wait();
            Slot&               slot   = slots_[block % slotCount_];
            const std::uint64_t offset = std::uint64_t{slot.buffer.size()} * block;
            DWORD               read   = 0; // DWORD: Read's out parameter
            const auto          error
                = input_.Read(slot.buffer.data(),
                              static_cast<DWORD>(slot.buffer.size()), read, offset);
            if (Error::Failed(error) && error != Error::EndOfFile)
                readOk_ = false;
            slot.size = read;
            if (slot.size == 0) {
                readySlots_.Release();
                return;
            }
            slot.task.input = std::span(slot.buffer).first(slot.size);
            slot.encoded.AddTask(&slot.task);
            // Default would inherit the reader's High: the encoding is Normal.
            scheduler_->AddTask(&slot.task, TC::TaskPriority::Normal);
            readySlots_.Release();
        }
    }
    // [/read]

private:
    TC::ITaskScheduler*              scheduler_ = nullptr;
    AsyncFile                        input_;
    std::size_t                      slotCount_ = 0;
    std::unique_ptr<Slot[]>          slots_;
    TC::Synchronization::Semaphore_s freeSlots_;
    TC::Synchronization::Semaphore_s readySlots_;
    ReaderTask                       reader_{*this};
    bool                             readOk_ = true;
};

void ReaderTask::Run() noexcept
{
    packer_.Read();
}

int wmain(int argc, wchar_t* argv[])
{
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments))
        return arguments.exitCode;

    const std::uint32_t threads   = arguments.threads != 0
        ? arguments.threads
        : GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const std::size_t   slotCount = std::size_t{4} * threads;

    TC::ConvertThreadToTask();
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;

    TC::SchedulerInfo info{};
    info.numberOfThreads = threads;
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool packed = false;
    {
        Packer          packer;
        const Stopwatch total;
        packed = packer.Pack(*scheduler, arguments, slotCount);
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
