// Guide "Task graphs": a three-stage pipeline - a serial reader, a parallel
// transform, a serial writer that writes in order - over a ring of slots bounded by
// a semaphore, with the serial stages at high priority. Uses files in the temp
// directory that it deletes; checks its own results outside the regions that the
// guide shows.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Platform.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::Platform::Error;
using ByteCount = AsyncFile::ByteCount;
// [/names]

// [sizes]
constexpr ByteCount BlockSize = 1 << 20; // 1 MB per block
constexpr unsigned  Slots     = 8; // blocks in flight between reader and writer

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
    ByteCount                 size   = 0; // 0: the end of the input
    TransformTask             transform;
    // the writer waits here for this slot's block
    TC::Synchronization::TaskGroup_s transformed;
};

void TransformTask::Run() noexcept
{
    for (ByteCount i = 0; i < slot->size; ++i)
        slot->output[i] = Transform(slot->input[i], slot->offset + i);
}
// [/slot]

// [read]
class Pipeline
{
public:
    Pipeline(TC::Platform::FileName inputPath,
             TC::Platform::FileName outputPath) noexcept
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
        AsyncFile file;
        // the default options: an existing file, for reading
        const bool open = Error::Succeeded(
            file.Create(inputPath_, TC::Platform::FileOptions{}));
        readFailed_ = !open;
        for (unsigned block = 0;; ++block) {
            // at most Slots blocks in flight: bounds memory and tasks
            freeSlots_.Wait();
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
            // Default would inherit High
            slot.transform.Schedule(TC::TaskPriority::Normal);
            readySlots_.Release(); // the writer may wait for this block now
        }
    }
    // [/read]

    // [write]
    // Serial stage 3: write the blocks in order, each as soon as its transform has
    // exited.
    void Write() noexcept
    {
        AsyncFile                           file;
        constexpr TC::Platform::FileOptions create{
            .access   = TC::Platform::FileAccess::Write,
            .creation = TC::Platform::FileCreation::CreateAlways};
        writeFailed_ = Error::Failed(file.Create(outputPath_, create));
        for (unsigned block = 0;; ++block) {
            readySlots_.Wait(); // the reader has counted the transform in, or ended
            Slot& slot = slots_[block % Slots];
            if (slot.size == 0)
                return;              // the end mark
            slot.transformed.Wait(); // the join of this block's transform
            ByteCount written = 0;
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
    TC::Platform::FileName inputPath_;
    TC::Platform::FileName outputPath_;
    Slot                   slots_[Slots];
    // Free slots and forked blocks; each has one waiter: the reader, the writer.
    TC::Synchronization::Semaphore_s freeSlots_{Slots};
    TC::Synchronization::Semaphore_s readySlots_{0};
    bool                             readFailed_  = false;
    bool                             writeFailed_ = false;
    // [/write]
};

// Test driver: an input file of 47.5 blocks, so that the last block is a short one.
constexpr std::uint64_t InputSize = std::uint64_t{BlockSize} * 95 / 2;

static std::uint8_t InputByte(std::uint64_t position) noexcept
{
    return static_cast<std::uint8_t>(position * 11 + position / 251);
}

static std::size_t BlockAt(std::uint64_t offset) noexcept
{
    return static_cast<std::size_t>(
        InputSize - offset < BlockSize ? InputSize - offset : BlockSize);
}

static bool MakeInput(const std::filesystem::path& path)
{
    std::ofstream             file(path, std::ios::binary | std::ios::trunc);
    std::vector<std::uint8_t> block(BlockSize);
    for (std::uint64_t offset = 0; offset < InputSize && file;
         offset += BlockSize) {
        const std::size_t size = BlockAt(offset);
        for (std::size_t i = 0; i < size; ++i)
            block[i] = InputByte(offset + i);
        file.write(reinterpret_cast<const char*>(block.data()),
                   static_cast<std::streamsize>(size));
    }
    file.close();
    return !file.fail();
}

static bool CheckOutput(const std::filesystem::path& path)
{
    std::error_code error;
    if (std::filesystem::file_size(path, error) != InputSize || error)
        return false;
    std::ifstream             file(path, std::ios::binary);
    std::vector<std::uint8_t> block(BlockSize);
    bool                      ok = file.good();
    for (std::uint64_t offset = 0; offset < InputSize && ok; offset += BlockSize) {
        const std::size_t wanted = BlockAt(offset);
        ok = static_cast<bool>(file.read(reinterpret_cast<char*>(block.data()),
                                         static_cast<std::streamsize>(wanted)));
        for (std::size_t i = 0; i < wanted && ok; ++i)
            ok = block[i] == Transform(InputByte(offset + i), offset + i);
    }
    return ok;
}

int main()
{
    const std::filesystem::path temp   = std::filesystem::temp_directory_path();
    const std::filesystem::path input  = temp / "ct-guide-graph-in.bin";
    const std::filesystem::path output = temp / "ct-guide-graph-out.bin";
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
    // large: keep it off the stack
    static Pipeline pipeline(input.c_str(), output.c_str());
    auto reader = TC::Tasks::Task::Create([]() noexcept { pipeline.Read(); });
    auto writer = TC::Tasks::Task::Create([]() noexcept { pipeline.Write(); });

    TC::Synchronization::TaskGroup_s stages;
    stages.AddTask(&reader);
    stages.AddTask(&writer);
    // the serial stages are the critical path
    scheduler->AddTask(&reader, TC::TaskPriority::High);
    scheduler->AddTask(&writer, TC::TaskPriority::High);
    stages.Wait(); // the writer waits for every transform, so all have run
    // [/run]

    scheduler->Stop();
    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();

    const bool ok = !pipeline.Failed() && CheckOutput(output);
    std::printf("output:  %s\n", ok ? "in order and correct" : "wrong");
    std::error_code ignored;
    std::filesystem::remove(input, ignored);
    std::filesystem::remove(output, ignored);
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
