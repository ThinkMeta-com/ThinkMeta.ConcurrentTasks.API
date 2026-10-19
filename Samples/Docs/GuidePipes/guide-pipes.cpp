// Guide "Pipes": SpscVirtualMemoryPipe between a producer and a consumer, written
// by hand on one side and by AsyncFile::WriteFile / AsyncFile::ReadFile on the
// other. Uses a file in %TEMP% that it deletes; checks its own results outside the
// regions that the guide shows.
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Memory::IReadSequentialMemory;
using TC::Memory::IWriteSequentialMemory;
using TC::Windows::Error;
// [/names]

// [sample]
// One measurement; the stream is a sequence of them.
struct Sample {
    std::uint64_t time;
    double        value;
};
// [/sample]

// Stands for the data source of the producer: the sample with the given number.
static Sample TakeSample(std::size_t index) noexcept
{
    return Sample{index * 1000, static_cast<double>(index % 1000)};
}

// [producer]
// Writes count samples into the stream, block by block, straight into the stream's
// memory.
void WriteSamples(IWriteSequentialMemory& out, std::size_t count) noexcept
{
    // announce the size first (also for an empty stream)
    out.Initialize(count * sizeof(Sample));
    constexpr std::size_t PerBlock = 4096; // samples per block
    for (std::size_t first = 0; first < count; first += PerBlock) {
        const std::size_t n = std::min(PerBlock, count - first);
        // the next bytes of the stream
        std::byte* data = out.RequestWriteMemory(n * sizeof(Sample));
        if (data == nullptr)
            break;
        for (std::size_t i = 0; i < n; ++i) {
            const Sample sample = TakeSample(first + i);
            std::memcpy(data + i * sizeof(Sample), &sample, sizeof(Sample));
        }
        out.ReleaseWriteMemory(n * sizeof(Sample)); // publish them to the consumer
    }
    out.SetEndOfStream(); // always, also after an error
}
// [/producer]

// [consumer]
// Reads the samples block by block, in place, and adds up their values. Returns
// false if the stream ended early.
bool SumSamples(IReadSequentialMemory& in, double& sum) noexcept
{
    // waits until the producer has called Initialize
    const std::size_t total = in.GetSize();
    std::size_t       read  = 0;
    sum                     = 0;
    for (;;) {
        std::size_t size = 4096 * sizeof(Sample); // wanted; less only at the end
        const std::byte* data = in.RequestReadMemory(size);
        if (data == nullptr)
            break; // end of the stream
        for (std::size_t offset = 0; offset < size; offset += sizeof(Sample)) {
            Sample sample;
            std::memcpy(&sample, data + offset, sizeof(Sample));
            sum += sample.value;
        }
        read += size;
        in.ReleaseReadMemory(size); // the memory may be reused now
    }
    return read == total;
}
// [/consumer]

int main()
{
    wchar_t     path[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, path);
    if (length == 0 || length + 32 > MAX_PATH)
        return 1;
    lstrcatW(path, L"ct-guide-pipes.bin");

    TC::ConvertThreadToTask();
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();
    bool                             ok = true;
    TC::Synchronization::TaskGroup_s helpers;

    // [to-file]
    // A producer task fills the pipe; main writes it to a file while it fills.
    constexpr std::size_t             count = 2'000'000; // 32 MB of samples
    TC::Memory::SpscVirtualMemoryPipe toFile;
    auto                              producer = TC::Tasks::Task::Create(
        [&toFile]() noexcept { WriteSamples(toFile, count); });
    helpers.AddTask(&producer);
    scheduler->AddTask(&producer);

    const auto written = TC::AsyncIo::AsyncFile::WriteFile(
        path, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, &toFile);
    helpers.Wait();
    // [/to-file]
    std::printf("pipe to file:  %s\n", Error::Succeeded(written) ? "ok" : "failed");
    ok = ok && Error::Succeeded(written);

    // [from-file]
    // main reads the file into a second pipe; a consumer task adds up the samples
    // as they arrive.
    TC::Memory::SpscVirtualMemoryPipe fromFile;
    double                            sum      = 0;
    bool                              complete = false;
    auto                              consumer
        = TC::Tasks::Task::Create([&fromFile, &sum, &complete]() noexcept {
              complete = SumSamples(fromFile, sum);
          });
    helpers.AddTask(&consumer);
    scheduler->AddTask(&consumer);

    TC::AsyncIo::BulkIoOptions options; // 5 reads of 1 MB in flight by default
    const auto                 read = TC::AsyncIo::AsyncFile::ReadFile(
        path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, &fromFile, options);
    helpers.Wait();
    // [/from-file]

    double expected = 0;
    for (std::size_t i = 0; i < count; ++i)
        expected += TakeSample(i).value;
    const bool same = complete && sum == expected;
    std::printf("file to pipe:  %s, all samples read back: %s\n",
                Error::Succeeded(read) ? "ok" : "failed", same ? "yes" : "no");
    ok = ok && Error::Succeeded(read) && same;

    scheduler->Stop();
    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();
    DeleteFileW(path);
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
