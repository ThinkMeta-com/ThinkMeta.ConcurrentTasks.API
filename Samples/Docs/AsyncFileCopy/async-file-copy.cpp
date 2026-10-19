// Async file copy: a task copies a file with overlapped reads and writes on the
// core's I/O completion port - the read of the next block runs while the current
// block is being written.
#define NOMINMAX
#include <Windows.h>

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
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncFile;
using TC::AsyncIo::WaitableAsyncFileIoRequest_s;
using TC::Windows::Error;
using TC::Windows::ErrorCode;

// not a multiple of the block size
constexpr std::uint64_t FileSize  = 8 * 1024 * 1024 + 123;
constexpr DWORD         BlockSize = 1024 * 1024;

std::byte Pattern(std::uint64_t offset) noexcept
{
    return static_cast<std::byte>((offset * 7 + offset / 4096) & 0xFF);
}

// [create]
// Writes the source file block by block. Write suspends the calling task until the
// write has completed; every call names its file offset, since the handle has no
// file pointer.
ErrorCode CreateSource(const wchar_t* path) noexcept
{
    AsyncFile file;
    ErrorCode error
        = file.Create(path, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    std::vector<std::byte> block(BlockSize);
    for (std::uint64_t offset = 0; Error::Succeeded(error) && offset < FileSize;
         offset += BlockSize) {
        const auto bytes = static_cast<DWORD>(
            std::min<std::uint64_t>(BlockSize, FileSize - offset));
        for (DWORD i = 0; i < bytes; ++i)
            block[i] = Pattern(offset + i);
        DWORD written = 0;
        error         = file.Write(block.data(), bytes, written, offset);
        if (Error::Succeeded(error) && written != bytes)
            error = ERROR_WRITE_FAULT;
    }
    return error;
}
// [/create]

// [copy]
// Two buffers: while the block in one buffer is being written, the next block is
// read into the other one.
ErrorCode Copy(const wchar_t* sourcePath, const wchar_t* targetPath) noexcept
{
    AsyncFile source;
    AsyncFile target;
    ErrorCode error = source.Create(sourcePath, GENERIC_READ, FILE_SHARE_READ,
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
    if (Error::Succeeded(error))
        error = target.Create(targetPath, GENERIC_WRITE, 0, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL);
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
        const std::size_t i = block % 2;
        const auto        bytes
            = static_cast<DWORD>(std::min<std::uint64_t>(BlockSize, size - offset));

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
        DWORD read = 0;
        error      = source.Read(buffers[i].data(), bytes, read, offset);
        if (Error::Succeeded(error) && read != bytes)
            error = ERROR_HANDLE_EOF;
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
bool Verify(const wchar_t* path) noexcept
{
    AsyncFile           file;
    AsyncFile::FileSize size = 0;
    if (Error::Failed(file.Create(path, GENERIC_READ, FILE_SHARE_READ,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL))
        || Error::Failed(file.GetSize(size)) || size != FileSize)
        return false;
    std::vector<std::byte> block(BlockSize);
    for (std::uint64_t offset = 0; offset < size; offset += BlockSize) {
        const auto bytes
            = static_cast<DWORD>(std::min<std::uint64_t>(BlockSize, size - offset));
        DWORD read = 0;
        if (Error::Failed(file.Read(block.data(), bytes, read, offset))
            || read != bytes)
            return false;
        for (DWORD i = 0; i < bytes; ++i) {
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
    CopyTask(const wchar_t* source, const wchar_t* target) noexcept
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
    const wchar_t* source_;
    const wchar_t* target_;
};
// [/task]

int main()
{
    const std::filesystem::path source = std::filesystem::temp_directory_path()
        / L"ConcurrentTasks-copy-source.bin";
    const std::filesystem::path target = std::filesystem::temp_directory_path()
        / L"ConcurrentTasks-copy-target.bin";

    // [run]
    TC::ConvertThreadToTask();
    // Before the first AsyncFile is created: it associates its handle with the
    // core's completion port.
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

    std::printf("Copied %llu bytes: %s, %s\n", FileSize,
                task.error == Error::Success ? "no error" : "FAILED",
                task.verified ? "verified" : "NOT VERIFIED");
    std::error_code ignored;
    std::filesystem::remove(source, ignored);
    std::filesystem::remove(target, ignored);
    return task.error == Error::Success && task.verified ? 0 : 1;
}
