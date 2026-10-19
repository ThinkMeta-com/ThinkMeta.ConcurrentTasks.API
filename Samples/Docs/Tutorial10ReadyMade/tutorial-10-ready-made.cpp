// Tutorial chapter 10: ready-made stages - StreamedFileReader streams the input
// into memory and OrderedFileWriter writes the output behind the producer; the
// hand-written reading, slot buffers and blocking writes are gone.
// [names]
#include "tutorial-common.h"

#include <atomic>
#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.OrderedFileWriter;
import ThinkMeta.ConcurrentTasks.AsyncIo.StreamedFileReader;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::OrderedFileWriter;
using TC::AsyncIo::StreamedFileReader;
using TC::Windows::Error;
using namespace Tutorial;
// [/names]

// Set by Ctrl+C, as in chapter 9.
std::atomic<bool> stopRequested{false};

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

// [slot]
// A slot no longer holds a buffer: the encode task reads the block where the
// stream has put it.
struct Slot {
    std::uint64_t                    end = 0; // 0: the end of the input
    EncodeTask                       task;
    TC::Synchronization::TaskGroup_s encoded;
};
// [/slot]

class Packer;

// Runs Packer::Fork as a task.
class ForkTask : public TC::Tasks::Task
{
public:
    explicit ForkTask(Packer& packer) noexcept : packer_(packer) {}

    void Run() noexcept override;

private:
    Packer& packer_;
};

class Packer
{
public:
    // [open]
    bool Pack(TC::ITaskScheduler& scheduler, const Arguments& arguments,
              std::size_t slotCount)
    {
        blockSize_ = arguments.blockSize;
        slotCount_ = slotCount;

        // The input: streamed in segments of one block, at most one ring ahead
        // of the oldest block still in use.
        TC::AsyncIo::StreamedFileReaderSettings readSettings;
        readSettings.segmentSize = static_cast<std::uint32_t>(blockSize_);
        readSettings.readAhead   = slotCount * blockSize_;
        if (Error::Failed(input_.Open(arguments.input, readSettings)))
            return false;

        // The output: the encoded size is not known in advance, so reserve
        // address space for the worst case of the encoder (twice the input).
        const std::uint64_t blocks = input_.Size() / blockSize_ + 1;
        const std::uint64_t bound  = 2 * input_.Size() + 16 * blocks + 16;
        const bool opened = Error::Succeeded(output_.Open(arguments.output, bound));

        // The stages run as tasks at High; the group tells when all have ended.
        TC::Tasks::Task* stages[] = {&input_, &output_.Writer(), &fork_};
        for (TC::Tasks::Task* stage : stages) {
            stages_.AddTask(stage);
            scheduler.AddTask(stage, TC::TaskPriority::High);
        }
        scheduler_ = &scheduler;
        slots_     = std::make_unique<Slot[]>(slotCount);
        if (!opened)
            stopRequested = true; // let the stages end, then report the error
        freeSlots_.Release(slotCount);
        // [/open]

        // [write]
        // main is the producer of the output: it appends the blocks in order.
        std::uint32_t crc = 0;
        for (std::size_t block = 0;; ++block) {
            readySlots_.Wait();
            Slot& slot = slots_[block % slotCount];
            if (slot.end == 0)
                break;
            slot.encoded.Wait();
            const EncodedBlock& encoded = slot.task.encoded;
            const auto          header  = BlockHeader(encoded);
            output_.Write(header.data(), sizeof(header)); // copied at once
            output_.Write(encoded.data.data(), encoded.data.size());
            output_.EndBatch(); // the writer task may write up to here
            crc = CombineCrc32(crc, slot.task.crc, BlockBytes(encoded));
            if (output_.Failed())
                stopRequested = true;
            input_.Release(0, slot.end); // the stream may drop the block
            freeSlots_.Release();
        }
        output_.Write(&crc, sizeof(crc));
        const bool closed = output_.Close(); // waits for the last write
        input_.Stop();                       // ends the stream after a stop
        input_.Release(0, ~std::uint64_t{0});
        output_.StopCopyTasks(0); // no copy tasks: every Write copies at once
        stages_.Wait();           // the stream, the writer and the fork task
        return closed && Error::Succeeded(input_.ReadError()) && !stopRequested;
        // [/write]
    }

    // [fork]
    // Takes block after block from the stream and forks its encoding. Wait
    // suspends this task until the stream has loaded the block.
    void Fork() noexcept
    {
        const std::uint64_t size = input_.Size();
        for (std::size_t block = 0;; ++block) {
            freeSlots_.Wait();
            Slot&               slot  = slots_[block % slotCount_];
            const std::uint64_t begin = std::uint64_t{blockSize_} * block;
            const std::uint64_t end
                = std::min<std::uint64_t>(begin + blockSize_, size);
            slot.end = 0;
            if (begin < size && !stopRequested && input_.Wait(end) >= end)
                slot.end = end;
            if (slot.end == 0) { // the end of the file, or a stop
                readySlots_.Release();
                return;
            }
            slot.task.input = std::span(input_.Data() + begin, end - begin);
            slot.encoded.AddTask(&slot.task);
            scheduler_->AddTask(&slot.task, TC::TaskPriority::Normal);
            readySlots_.Release();
        }
    }
    // [/fork]

private:
    // [members]
    TC::ITaskScheduler*              scheduler_ = nullptr;
    std::size_t                      blockSize_ = 0;
    std::size_t                      slotCount_ = 0;
    StreamedFileReader               input_;
    OrderedFileWriter                output_;
    std::unique_ptr<Slot[]>          slots_;
    TC::Synchronization::Semaphore_s freeSlots_;
    TC::Synchronization::Semaphore_s readySlots_;
    ForkTask                         fork_{*this};
    TC::Synchronization::TaskGroup_s stages_;
    // [/members]
};

void ForkTask::Run() noexcept
{
    packer_.Fork();
}

BOOL WINAPI OnConsoleCtrl(DWORD ctrlType) noexcept
{
    if (ctrlType != CTRL_C_EVENT)
        return FALSE;
    stopRequested = true;
    return TRUE;
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

    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
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
        DeleteFileW(arguments.output);
        std::printf("cannot pack the file\n");
        return 1;
    }
    return CheckOutput(arguments) ? 0 : 1;
}
