// Guide "Task graphs": a three-stage pipeline - a serial reader, a parallel
// transform, a serial writer that writes in order - over a ring of slots bounded by
// a semaphore, with the serial stages at high priority. Uses files in %TEMP% that
// it deletes; checks its own results outside the regions that the guide shows.
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::Windows::Error;

// [sizes]
constexpr DWORD    BlockSize = 1 << 20; // 1 MB per block
constexpr unsigned Slots     = 8; // blocks in flight between reader and writer

// The work of the parallel stage, for one byte at a file position.
std::uint8_t Transform(std::uint8_t value, std::uint64_t position) noexcept;
// [/sizes]

std::uint8_t Transform(std::uint8_t value, std::uint64_t position) noexcept
{
    std::uint32_t x = value ^ static_cast<std::uint32_t>(position);
    for (int round = 0; round < 8; ++round)
        x = x * 2654435761u + 0x9E3779B9u;
    return static_cast<std::uint8_t>(x >> 24);
}

// [slot]
// One slot of the ring: a block on its way from the reader through a transform task
// to the writer.
struct Slot;

class TransformTask : public TC::Tasks::Task
{
public:
    Slot* slot = nullptr;
    void  Run() noexcept override;
};

struct Slot {
    std::vector<std::uint8_t> input  = std::vector<std::uint8_t>(BlockSize);
    std::vector<std::uint8_t> output = std::vector<std::uint8_t>(BlockSize);
    std::uint64_t             offset = 0;
    DWORD                     size   = 0; // 0: the end of the input
    TransformTask             transform;
    TC::Synchronization::TaskGroup_s
        transformed; // the writer waits here for this slot's block
};

void TransformTask::Run() noexcept
{
    for (DWORD i = 0; i < slot->size; ++i)
        slot->output[i] = Transform(slot->input[i], slot->offset + i);
}
// [/slot]

// [pipeline]
class Pipeline
{
public:
    Pipeline(const wchar_t* inputPath, const wchar_t* outputPath) noexcept
        : inputPath_(inputPath),
          outputPath_(outputPath)
    {
        for (Slot& slot : slots_)
            slot.transform.slot = &slot;
    }

    // Serial stage 1: read block after block into free slots and fork a transform
    // for each. An empty slot marks the end of the input for the writer.
    void Read() noexcept
    {
        AsyncFile  file;
        const bool open = Error::Succeeded(
            file.Create(inputPath_, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN));
        readFailed_ = !open;
        for (unsigned block = 0;; ++block) {
            freeSlots_
                .Wait(); // at most Slots blocks in flight: bounds memory and tasks
            Slot& slot  = slots_[block % Slots];
            slot.offset = std::uint64_t{BlockSize} * block;
            slot.size   = 0;
            if (open) {
                const auto error = file.Read(slot.input.data(), BlockSize,
                                             slot.size, slot.offset);
                if (Error::Failed(error) && error != Error::EndOfFile)
                    readFailed_ = true; // slot.size is 0: the pipeline ends here
            }
            if (slot.size == 0) {
                readySlots_.Release(); // the end mark
                return;
            }
            slot.transformed.AddTask(&slot.transform);
            slot.transform.Schedule(
                TC::TaskPriority::Normal); // Default would inherit High
            readySlots_.Release();         // the writer may wait for this block now
        }
    }

    // Serial stage 3: write the blocks in order, each as soon as its transform has
    // exited.
    void Write() noexcept
    {
        AsyncFile file;
        writeFailed_ = Error::Failed(file.Create(
            outputPath_, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL));
        for (unsigned block = 0;; ++block) {
            readySlots_.Wait(); // the reader has counted the transform in, or ended
            Slot& slot = slots_[block % Slots];
            if (slot.size == 0)
                return;              // the end mark
            slot.transformed.Wait(); // the join of this block's transform
            DWORD written = 0;
            if (!writeFailed_
                && (Error::Failed(file.Write(slot.output.data(), slot.size, written,
                                             slot.offset))
                    || written != slot.size))
                writeFailed_ = true;
            freeSlots_.Release(); // the slot can take the next block
        }
    }

    bool Failed() const noexcept { return readFailed_ || writeFailed_; }

private:
    const wchar_t*                   inputPath_;
    const wchar_t*                   outputPath_;
    Slot                             slots_[Slots];
    TC::Synchronization::Semaphore_s freeSlots_{
        Slots}; // free slots; one waiter: the reader
    TC::Synchronization::Semaphore_s readySlots_{
        0}; // forked blocks; one waiter: the writer
    bool readFailed_  = false;
    bool writeFailed_ = false;
};
// [/pipeline]

// Test driver: an input file of 47.5 blocks, so that the last block is a short one.
constexpr std::uint64_t InputSize = std::uint64_t{BlockSize} * 95 / 2;

static std::uint8_t InputByte(std::uint64_t position) noexcept
{
    return static_cast<std::uint8_t>(position * 11 + position / 251);
}

static bool MakeInput(const wchar_t* path) noexcept
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::vector<std::uint8_t> block(BlockSize);
    bool                      ok = true;
    for (std::uint64_t offset = 0; offset < InputSize && ok; offset += BlockSize) {
        const DWORD size = static_cast<DWORD>(
            InputSize - offset < BlockSize ? InputSize - offset : BlockSize);
        for (DWORD i = 0; i < size; ++i)
            block[i] = InputByte(offset + i);
        DWORD written = 0;
        ok            = ::WriteFile(file, block.data(), size, &written, nullptr)
            && written == size;
    }
    CloseHandle(file);
    return ok;
}

static bool CheckOutput(const wchar_t* path) noexcept
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool          ok = GetFileSizeEx(file, &size)
        && static_cast<std::uint64_t>(size.QuadPart) == InputSize;
    std::vector<std::uint8_t> block(BlockSize);
    for (std::uint64_t offset = 0; offset < InputSize && ok; offset += BlockSize) {
        const DWORD wanted = static_cast<DWORD>(
            InputSize - offset < BlockSize ? InputSize - offset : BlockSize);
        DWORD read = 0;
        ok         = ::ReadFile(file, block.data(), wanted, &read, nullptr)
            && read == wanted;
        for (DWORD i = 0; i < wanted && ok; ++i)
            ok = block[i] == Transform(InputByte(offset + i), offset + i);
    }
    CloseHandle(file);
    return ok;
}

int main()
{
    wchar_t     input[MAX_PATH];
    wchar_t     output[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, input);
    if (length == 0 || length + 32 > MAX_PATH)
        return 1;
    lstrcpyW(output, input);
    lstrcatW(input, L"ct-guide-graph-in.bin");
    lstrcatW(output, L"ct-guide-graph-out.bin");
    if (!MakeInput(input))
        return 1;

    TC::ConvertThreadToTask();
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    // [run]
    static Pipeline pipeline(input, output); // large: keep it off the stack
    auto reader = TC::Tasks::Task::Create([]() noexcept { pipeline.Read(); });
    auto writer = TC::Tasks::Task::Create([]() noexcept { pipeline.Write(); });

    TC::Synchronization::TaskGroup_s stages;
    stages.AddTask(&reader);
    stages.AddTask(&writer);
    scheduler->AddTask(
        &reader, TC::TaskPriority::High); // the serial stages are the critical path
    scheduler->AddTask(&writer, TC::TaskPriority::High);
    stages.Wait(); // the writer waits for every transform, so all have run
    // [/run]

    scheduler->Stop();
    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();

    const bool ok = !pipeline.Failed() && CheckOutput(output);
    std::printf("output:  %s\n", ok ? "in order and correct" : "wrong");
    DeleteFileW(input);
    DeleteFileW(output);
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
