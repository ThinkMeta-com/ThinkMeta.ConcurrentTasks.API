// Async file copy: a task copies a file with asynchronous reads and writes through
// the core (an I/O completion port on Windows, io_uring on Linux) - the read of the
// next block runs while the current block is being written.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Platform.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::AsyncIo::WaitableAsyncFileIoRequest_s;
using TC::Platform::Error;
using TC::Platform::ErrorCode;
using ByteCount = AsyncFile::ByteCount;

// not a multiple of the block size
constexpr std::uint64_t FileSize  = 8 * 1024 * 1024 + 123;
constexpr ByteCount     BlockSize = 1024 * 1024;

// How the files are opened: written from scratch, or an existing file read.
constexpr TC::Platform::FileOptions ToWrite{
    .access   = TC::Platform::FileAccess::Write,
    .creation = TC::Platform::FileCreation::CreateAlways};
constexpr TC::Platform::FileOptions
    ToRead{}; // the default: an existing file, for reading

std::byte Pattern(std::uint64_t offset) noexcept
{
    return static_cast<std::byte>((offset * 7 + offset / 4096) & 0xFF);
}

// [create]
// Writes the source file block by block. Write suspends the calling task until the
// write has completed; every call names its file offset, since the handle has no
// file pointer.
ErrorCode CreateSource(TC::Platform::FileName path) noexcept
{
    AsyncFile              file;
    ErrorCode              error = file.Create(path, ToWrite);
    std::vector<std::byte> block(BlockSize);
    for (std::uint64_t offset = 0; Error::Succeeded(error) && offset < FileSize;
         offset += BlockSize) {
        const auto bytes = static_cast<ByteCount>(
            std::min<std::uint64_t>(BlockSize, FileSize - offset));
        for (ByteCount i = 0; i < bytes; ++i)
            block[i] = Pattern(offset + i);
        ByteCount written = 0;
        error             = file.Write(block.data(), bytes, written, offset);
        if (Error::Succeeded(error) && written != bytes)
            error = Error::WriteFault;
    }
    return error;
}
// [/create]

// [copy]
// Two buffers: while the block in one buffer is being written, the next block is
// read into the other one.
ErrorCode Copy(TC::Platform::FileName sourcePath,
               TC::Platform::FileName targetPath) noexcept
{
    AsyncFile source;
    AsyncFile target;
    ErrorCode error = source.Create(sourcePath, ToRead);
    if (Error::Succeeded(error))
        error = target.Create(targetPath, ToWrite);
    AsyncFile::FileSize size = 0;
    if (Error::Succeeded(error))
        error = source.GetSize(size);
    if (Error::Failed(error))
        return error;

    std::vector<std::byte> buffers[2]
        = {std::vector<std::byte>(BlockSize), std::vector<std::byte>(BlockSize)};
    WaitableAsyncFileIoRequest_s writes[2];
    bool writing[2] = {false, false}; // started and not yet waited for

    for (std::uint64_t offset = 0, block = 0; offset < size;
         offset += BlockSize, ++block) {
        const std::size_t i     = block % 2;
        const auto        bytes = static_cast<ByteCount>(
            std::min<std::uint64_t>(BlockSize, size - offset));

        // The buffer is free once the write from it has completed.
        if (writing[i]) {
            writes[i].Wait();
            writing[i] = false;
            error      = writes[i].GetError();
            if (Error::Failed(error))
                break;
        }

        // Suspends this task until the block is read; the write from the other
        // buffer goes on meanwhile.
        ByteCount read = 0;
        error          = source.Read(buffers[i].data(), bytes, read, offset);
        if (Error::Succeeded(error) && read != bytes)
            error = Error::EndOfFile;
        if (Error::Failed(error))
            break;

        writes[i].Reset(); // also clears the offset
        writes[i].SetOffset(offset);
        const ErrorCode started
            = target.WriteAsync(buffers[i].data(), bytes, writes[i]);
        if (Error::Failed(started) && !Error::IsPending(started)) {
            error = started; // not started: never wait for this request
            break;
        }
        writing[i] = true;
    }

    // No write may be pending when its buffer, its request or its file goes away -
    // also after an error.
    for (std::size_t i = 0; i < 2; ++i) {
        if (writing[i]) {
            writes[i].Wait();
            if (Error::Succeeded(error))
                error = writes[i].GetError();
        }
    }
    return error;
}
// [/copy]

// Reads both files with Read (suspending, explicit offsets) and compares them with
// the pattern.
bool Verify(TC::Platform::FileName path) noexcept
{
    AsyncFile           file;
    AsyncFile::FileSize size = 0;
    if (Error::Failed(file.Create(path, ToRead))
        || Error::Failed(file.GetSize(size)) || size != FileSize)
        return false;
    std::vector<std::byte> block(BlockSize);
    for (std::uint64_t offset = 0; offset < size; offset += BlockSize) {
        const auto bytes = static_cast<ByteCount>(
            std::min<std::uint64_t>(BlockSize, size - offset));
        ByteCount read = 0;
        if (Error::Failed(file.Read(block.data(), bytes, read, offset))
            || read != bytes)
            return false;
        for (ByteCount i = 0; i < bytes; ++i) {
            if (block[i] != Pattern(offset + i))
                return false;
        }
    }
    return true;
}

// [task]
// The file operations suspend their caller, so they run in a task. WaitableTask_s
// lets main wait for it.
class CopyTask : public TC::Tasks::WaitableTask_s
{
public:
    CopyTask(TC::Platform::FileName source, TC::Platform::FileName target) noexcept
        : source_(source),
          target_(target)
    {}

    void Run() noexcept override
    {
        error = CreateSource(source_);
        if (Error::Succeeded(error))
            error = Copy(source_, target_);
        verified = Error::Succeeded(error) && Verify(source_) && Verify(target_);
    }

    ErrorCode error    = Error::Success;
    bool      verified = false;

private:
    TC::Platform::FileName source_;
    TC::Platform::FileName target_;
};
// [/task]

int main()
{
    const std::filesystem::path source = std::filesystem::temp_directory_path()
        / "ConcurrentTasks-copy-source.bin";
    const std::filesystem::path target = std::filesystem::temp_directory_path()
        / "ConcurrentTasks-copy-target.bin";

    // [run]
    TC::ConvertThreadToTask();
    // Before the first AsyncFile is created: on Windows it associates its handle
    // with the core's completion port.
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    CopyTask task(source.c_str(), target.c_str());
    scheduler->AddTask(&task);
    task.Wait();
    scheduler->Stop();
    scheduler->Destroy();

    // No operation is pending any more: every file is closed.
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();
    // [/run]

    std::printf("Copied %llu bytes: %s, %s\n",
                static_cast<unsigned long long>(FileSize),
                task.error == Error::Success ? "no error" : "FAILED",
                task.verified ? "verified" : "NOT VERIFIED");
    std::error_code ignored;
    std::filesystem::remove(source, ignored);
    std::filesystem::remove(target, ignored);
    return task.error == Error::Success && task.verified ? 0 : 1;
}
