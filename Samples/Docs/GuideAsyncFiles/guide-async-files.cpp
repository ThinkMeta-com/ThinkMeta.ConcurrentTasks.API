// Guide "Asynchronous files": AsyncFile with the waiting Read/Write, several
// ReadAsync requests in flight, error codes, and the whole-file ReadFile into a
// pipe with BulkIoOptions. Works on a file it creates in %TEMP% and deletes; checks
// its own results outside the regions that the guide shows.
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::Windows::Error;

constexpr DWORD BlockSize = 64 * 1024;

// [write-read]
// Writes data at the start of the file and reads it back. Each call suspends the
// calling task until the operation has completed; its thread runs other tasks
// meanwhile.
static bool WriteAndReadBack(AsyncFile& file, const std::vector<std::byte>& data,
                             std::vector<std::byte>& copy) noexcept
{
    DWORD written = 0;
    if (Error::Failed(
            file.Write(data.data(), static_cast<DWORD>(data.size()), written, 0)))
        return false;

    DWORD read = 0;
    if (Error::Failed(
            file.Read(copy.data(), static_cast<DWORD>(copy.size()), read, 0)))
        return false;
    return read == copy.size();
}
// [/write-read]

// [read-timeout]
// The same read, but giving up after five seconds (needs EnableTimers).
static bool ReadWithTimeout(AsyncFile&              file,
                            std::vector<std::byte>& buffer) noexcept
{
    DWORD      read  = 0;
    const auto error = file.Read(buffer.data(), static_cast<DWORD>(buffer.size()),
                                 read, 0, TC::Duration::Seconds(5));
    if (error == ERROR_TIMEOUT)
        std::printf("the disk did not answer in time\n");
    return Error::Succeeded(error);
}
// [/read-timeout]

// [in-flight]
// Reads the first four blocks with four reads in flight at once: start them all,
// then wait.
static bool ReadFourBlocks(AsyncFile& file,
                           std::vector<std::byte> (&buffers)[4]) noexcept
{
    TC::AsyncIo::WaitableAsyncFileIoRequest_s requests[4];

    int started = 0;
    for (; started < 4; ++started) {
        requests[started].SetOffset(std::uint64_t{BlockSize} * started);
        const auto error
            = file.ReadAsync(buffers[started].data(), BlockSize, requests[started]);
        if (Error::Failed(error) && !Error::IsPending(error))
            break; // not started: never wait for this request
    }

    // Every request that was started must complete before it goes out of scope.
    bool ok = started == 4;
    for (int i = 0; i < started; ++i) {
        requests[i].Wait(); // returns at once if the read has completed already
        ok = ok && Error::Succeeded(requests[i].GetError());
    }
    return ok;
}
// [/in-flight]

// [errors]
// Reading at or beyond the end of the file is an error, not an empty read.
static bool IsEndOfFile(AsyncFile& file, std::uint64_t offset) noexcept
{
    std::byte buffer[512];
    DWORD     read = 0;
    return file.Read(buffer, sizeof(buffer), read, offset)
        == Error::EndOfFile; // ERROR_HANDLE_EOF
}
// [/errors]

// [consume]
// Takes the data out of the pipe as it arrives; returns the number of bytes.
static std::uint64_t Consume(TC::Memory::SpscVirtualMemoryPipe& pipe) noexcept
{
    std::uint64_t total = 0;
    for (;;) {
        std::size_t      size = 128 * 1024;
        const std::byte* data
            = pipe.RequestReadMemory(size); // suspends until data or the end
        if (data == nullptr)
            return total;
        total += size; // ... process data[0 .. size) here ...
        pipe.ReleaseReadMemory(size);
    }
}
// [/consume]

// Runs as a task: uses the functions above on a file it creates, and checks the
// results.
class FileTask : public TC::Tasks::WaitableTask_s
{
public:
    explicit FileTask(const wchar_t* path) noexcept : path_(path) {}

    bool Ok() const noexcept { return ok_; }

    void Run() noexcept override { ok_ = Waiting() && InFlight() && ReadWhole(); }

private:
    bool Waiting() noexcept
    {
        // [create]
        AsyncFile file;
        if (Error::Failed(file.Create(path_, GENERIC_READ | GENERIC_WRITE, 0,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL)))
            return false;
        // [/create]

        std::vector<std::byte> data(4 * BlockSize), copy(data.size());
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<std::byte>(i * 7 + i / 4096);

        const bool written = WriteAndReadBack(file, data, copy) && copy == data;
        const bool timed   = ReadWithTimeout(file, copy) && copy == data;
        const bool atEnd   = IsEndOfFile(file, data.size() + 4096);
        std::printf("written and read back: %s\n",
                    written && timed ? "ok" : "failed");
        std::printf("read past the end:     %s\n",
                    atEnd ? "ERROR_HANDLE_EOF" : "unexpected");
        return written && timed && atEnd && Error::Succeeded(file.Close());
    }

    bool InFlight() noexcept
    {
        AsyncFile file;
        if (Error::Failed(file.Create(path_, GENERIC_READ, FILE_SHARE_READ,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL)))
            return false;

        std::vector<std::byte> buffers[4];
        for (auto& buffer : buffers)
            buffer.resize(BlockSize);
        bool ok = ReadFourBlocks(file, buffers);
        for (int b = 0; b < 4 && ok; ++b)
            for (std::size_t i = 0; i < BlockSize; ++i) {
                const std::size_t offset = std::size_t{BlockSize} * b + i;
                ok                       = ok
                    && buffers[b][i]
                        == static_cast<std::byte>(offset * 7 + offset / 4096);
            }
        std::printf("four reads in flight:  %s\n", ok ? "ok" : "failed");
        return ok;
    }

    bool ReadWhole() noexcept
    {
        // [bulk]
        // Read the whole file into a pipe with up to 8 reads of 256 KB in flight,
        // while a second task consumes the data as it arrives.
        TC::Memory::SpscVirtualMemoryPipe pipe;
        std::uint64_t                     bytes    = 0;
        auto                              consumer = TC::Tasks::Task::Create(
            [&pipe, &bytes]() noexcept { bytes = Consume(pipe); });
        helpers_.AddTask(&consumer);
        consumer.Schedule();

        TC::AsyncIo::BulkIoOptions options;
        options.blockSize      = 256 * 1024;
        options.blocksInFlight = 8;
        const auto error       = AsyncFile::ReadFile(
            path_, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, &pipe, options);
        helpers_.Wait(); // the consumer has taken everything
        // [/bulk]

        std::printf("whole file via pipe:   %llu bytes\n",
                    static_cast<unsigned long long>(bytes));
        return Error::Succeeded(error) && bytes == 4 * BlockSize;
    }

    const wchar_t*                   path_;
    bool                             ok_ = false;
    TC::Synchronization::TaskGroup_s helpers_; // outlives the consumer task
};

int main()
{
    wchar_t     path[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, path);
    if (length == 0 || length + 32 > MAX_PATH)
        return 1;
    lstrcatW(path, L"ct-guide-async-files.bin");

    TC::ConvertThreadToTask();
    // [enable]
    if (!TC::AsyncIo::EnableAsyncIo() || !TC::EnableTimers())
        return 1;
    // [/enable]

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    FileTask task(path);
    scheduler->AddTask(&task);
    task.Wait();
    const bool ok = task.Ok();

    scheduler->Stop();
    scheduler->Destroy();
    // [disable]
    TC::DisableTimers();
    TC::AsyncIo::DisableAsyncIo(); // no operation may be pending any more
    // [/disable]
    TC::ConvertTaskToThread();
    DeleteFileW(path);

    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
