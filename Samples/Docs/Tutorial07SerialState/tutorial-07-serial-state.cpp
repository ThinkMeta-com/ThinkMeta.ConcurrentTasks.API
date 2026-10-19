// Tutorial chapter 7: state that flows - the CRC over the output depends on
// every block before. -v lock tries to update it from the encode tasks under a
// lock and gets it wrong; the default splits it into a CRC per block, computed
// in parallel, and a cheap combine in order.
// [names]
#include "tutorial-common.h"

#include <cwchar>
#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.CriticalSection;
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

// [shared-crc]
// The first attempt: one CRC for the whole output, shared by all encode tasks
// and guarded by a critical section.
struct SharedCrc {
    TC::Synchronization::CriticalSection lock;
    std::uint32_t                        crc = 0;
};
// [/shared-crc]

// [task]
class EncodeTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        EncodeBlock(input, encoded);
        if (shared != nullptr) {
            // Wrong: continues the CRC in the order in which the blocks finish.
            TC::Synchronization::CriticalSectionLock lock(shared->lock);
            shared->crc = ContinueCrc(shared->crc, encoded);
        } else
            crc = BlockCrc(encoded); // this block's own CRC, in parallel
    }

    std::span<const std::byte> input;
    EncodedBlock               encoded;
    std::uint32_t              crc    = 0;
    SharedCrc*                 shared = nullptr; // set for -v lock
};
// [/task]

// A slot of the ring, as in chapter 6.
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

        const bool useLock = std::wcscmp(arguments.variant, L"lock") == 0;
        scheduler_         = &scheduler;
        slotCount_         = slotCount;
        slots_             = std::make_unique<Slot[]>(slotCount);
        for (std::size_t i = 0; i < slotCount; ++i) {
            slots_[i].buffer.resize(arguments.blockSize);
            slots_[i].task.shared = useLock ? &sharedCrc_ : nullptr;
        }
        freeSlots_.Release(slotCount);

        // [write]
        scheduler.AddTask(&reader_);
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
            // The serial part shrinks to a combine per block.
            crc = CombineCrc32(crc, slot.task.crc, BlockBytes(encoded));
            freeSlots_.Release();
        }
        reader_.Wait();
        input_.Close();
        if (useLock)
            crc = sharedCrc_.crc; // every encode task has exited
        return WriteFooter(output, crc) && ok && readOk_;
        // [/write]
    }

    // The reader of chapter 6.
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
            scheduler_->AddTask(&slot.task);
            readySlots_.Release();
        }
    }

private:
    TC::ITaskScheduler*              scheduler_ = nullptr;
    AsyncFile                        input_;
    std::size_t                      slotCount_ = 0;
    std::unique_ptr<Slot[]>          slots_;
    TC::Synchronization::Semaphore_s freeSlots_;
    TC::Synchronization::Semaphore_s readySlots_;
    SharedCrc                        sharedCrc_;
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
