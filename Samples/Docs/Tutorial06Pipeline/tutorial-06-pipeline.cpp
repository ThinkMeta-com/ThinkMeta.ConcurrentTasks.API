// Tutorial chapter 6: a bounded ring - blocks travel through a fixed number of
// slots from the reader through an encode task to the writer, so memory and
// tasks no longer grow with the file.
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

// [slot]
// Encodes one block. A plain Task this time: the slot's group tells when it has
// run, and the same object runs again for a later block.
class EncodeTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override { EncodeBlock(input, encoded); }

    std::span<const std::byte> input;
    EncodedBlock               encoded;
};

// One slot of the ring: a block on its way from the reader through its encode
// task to the writer.
struct Slot {
    std::vector<std::byte>           buffer;   // the raw block
    std::size_t                      size = 0; // 0: the end of the input
    EncodeTask                       task;
    TC::Synchronization::TaskGroup_s encoded; // the writer waits here
};
// [/slot]

class Packer;

// Runs Packer::Read as a task, as in chapter 5.
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
    // [pack]
    // The writer: takes the blocks in order, waits for each one's encoding,
    // writes it and hands the slot back to the reader.
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
        freeSlots_.Release(slotCount); // every slot is free at the start

        scheduler.AddTask(&reader_);
        std::uint32_t crc = 0;
        bool          ok  = true;
        for (std::size_t block = 0;; ++block) {
            readySlots_.Wait(); // the reader has forked this block, or ended
            Slot& slot = slots_[block % slotCount];
            if (slot.size == 0)
                break; // the end mark
            slot.encoded.Wait();
            crc = WriteBlock(output, slot.task.encoded, crc, ok);
            freeSlots_.Release(); // the slot can take the next block
        }
        reader_.Wait();
        input_.Close();
        return WriteFooter(output, crc) && ok && readOk_;
    }
    // [/pack]

    // [read]
    // The reader: fills free slots block after block and forks each block's
    // encoding. An empty slot tells the writer that the input has ended.
    void Read() noexcept
    {
        for (std::size_t block = 0;; ++block) {
            freeSlots_.Wait(); // at most slotCount blocks in flight
            Slot&               slot   = slots_[block % slotCount_];
            const std::uint64_t offset = std::uint64_t{slot.buffer.size()} * block;
            DWORD               read   = 0; // DWORD: Read's out parameter
            const auto          error
                = input_.Read(slot.buffer.data(),
                              static_cast<DWORD>(slot.buffer.size()), read, offset);
            if (Error::Failed(error) && error != Error::EndOfFile)
                readOk_ = false; // read is 0: the pipeline ends here
            slot.size = read;
            if (slot.size == 0) {
                readySlots_.Release(); // the end mark
                return;
            }
            slot.task.input = std::span(slot.buffer).first(slot.size);
            slot.encoded.AddTask(&slot.task); // count it in before it can exit
            scheduler_->AddTask(&slot.task);
            readySlots_.Release(); // the writer may wait for this block now
        }
    }
    // [/read]

private:
    // [members]
    TC::ITaskScheduler*     scheduler_ = nullptr;
    AsyncFile               input_;
    std::size_t             slotCount_ = 0;
    std::unique_ptr<Slot[]> slots_;
    // Free slots and forked blocks. Each has exactly one waiting task - the
    // reader, the writer - so the single-waiter variant is enough.
    TC::Synchronization::Semaphore_s freeSlots_;
    TC::Synchronization::Semaphore_s readySlots_;
    ReaderTask                       reader_{*this};
    bool                             readOk_ = true;
    // [/members]
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

    // Enough blocks in flight to keep every thread busy while the reader and
    // the writer catch up. At most this many encode tasks are runnable, so the
    // default queue capacity is enough again.
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
        Packer          packer; // lives until after Stop
        const Stopwatch total;
        packed = packer.Pack(*scheduler, arguments, slotCount);
        std::printf("time: %.2f s (%zu slots of %zu KB)\n", total.Seconds(),
                    slotCount, arguments.blockSize / 1024);
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
