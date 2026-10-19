// Tutorial chapter 9: errors and shutdown - a write error or Ctrl+C stops the
// pipeline without leaving a task waiting or an object destroyed too early.
// -v fail-write simulates a write error at block 10.
// [names]
#include "tutorial-common.h"

#include <atomic>
#include <cwchar>
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

// [stop]
// Set to ask the pipeline to stop: the reader reads no further block. A plain
// flag, because nobody waits for it - the reader only looks at it before each
// read. Global, because Ctrl+C may set it at any time, even when no packer
// exists.
std::atomic<bool> stopRequested{false};
// [/stop]

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

        const bool failWrite = std::wcscmp(arguments.variant, L"fail-write") == 0;
        scheduler_           = &scheduler;
        slotCount_           = slotCount;
        slots_               = std::make_unique<Slot[]>(slotCount);
        for (std::size_t i = 0; i < slotCount; ++i)
            slots_[i].buffer.resize(arguments.blockSize);
        freeSlots_.Release(slotCount);

        // [write]
        scheduler.AddTask(&reader_, TC::TaskPriority::High);
        std::uint32_t crc = 0;
        bool          ok  = true;
        for (std::size_t block = 0;; ++block) {
            readySlots_.Wait();
            Slot& slot = slots_[block % slotCount];
            if (slot.size == 0)
                break; // the end mark: the reader has finished
            // Wait for the encoding even after an error: the task must have
            // exited before its slot is reused or destroyed.
            slot.encoded.Wait();
            if (ok) {
                const EncodedBlock& encoded = slot.task.encoded;
                ok = !(failWrite && block == 10) && WriteBlockData(output, encoded);
                crc = CombineCrc32(crc, slot.task.crc, BlockBytes(encoded));
                if (!ok)
                    stopRequested
                        = true; // no more reading; drain what is in flight
            }
            freeSlots_.Release(); // also after an error: the reader may wait
        }
        reader_.Wait();
        input_.Close();
        const bool stopped = stopRequested;
        return WriteFooter(output, crc) && ok && readOk_ && !stopped;
        // [/write]
    }

    // [read]
    void Read() noexcept
    {
        for (std::size_t block = 0;; ++block) {
            freeSlots_.Wait();
            Slot& slot = slots_[block % slotCount_];
            DWORD read = 0; // DWORD: Read's out parameter
            if (!stopRequested) {
                const std::uint64_t offset
                    = std::uint64_t{slot.buffer.size()} * block;
                const auto error = input_.Read(
                    slot.buffer.data(), static_cast<DWORD>(slot.buffer.size()),
                    read, offset);
                if (Error::Failed(error) && error != Error::EndOfFile)
                    readOk_ = false;
            }
            slot.size = read;
            if (slot.size == 0) { // the end of the file, an error or a stop
                readySlots_.Release();
                return;
            }
            slot.task.input = std::span(slot.buffer).first(slot.size);
            slot.encoded.AddTask(&slot.task);
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

// [ctrl-c]
// Ctrl+C runs this on a thread that Windows creates - no task, no scheduler
// thread. It only sets the flag; the pipeline then shuts down as after an
// error.
BOOL WINAPI OnConsoleCtrl(DWORD ctrlType) noexcept
{
    if (ctrlType != CTRL_C_EVENT)
        return FALSE;
    stopRequested = true;
    return TRUE;
}
// [/ctrl-c]

// [main]
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
        // Pack returns only after the reader and every encode task have
        // finished, so stopping the scheduler now is safe.
        scheduler->Stop();
    }

    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();

    if (!packed) {
        DeleteFileW(arguments.output); // no half-written output
        std::printf("cannot pack the file\n");
        return 1;
    }
    return CheckOutput(arguments) ? 0 : 1;
}
// [/main]
