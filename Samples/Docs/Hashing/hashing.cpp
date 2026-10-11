// Hashing: one task reads a file with overlapped I/O into a SpscVirtualMemoryPipe,
// a second task hashes the bytes (SHA-256 with BCrypt) while they arrive - neither
// side copies them.
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Windows::Error;

using Sha256 = std::array<std::byte, 32>;

// SHA-256 over memory blocks; BCRYPT_SHA256_ALG_HANDLE needs no algorithm provider
// to be opened.
class Hasher
{
public:
    Hasher() noexcept
    {
        ok_ = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash_,
                                              nullptr, 0, nullptr, 0, 0));
    }
    ~Hasher() noexcept
    {
        if (ok_)
            BCryptDestroyHash(hash_);
    }
    Hasher(const Hasher&)            = delete;
    Hasher& operator=(const Hasher&) = delete;

    void Add(const std::byte* data, std::size_t size) noexcept
    {
        ok_ = ok_
            && BCRYPT_SUCCESS(BCryptHashData(
                hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data)),
                static_cast<ULONG>(size), 0));
    }

    bool Finish(Sha256& result) noexcept
    {
        return ok_
            && BCRYPT_SUCCESS(
                   BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(result.data()),
                                    static_cast<ULONG>(result.size()), 0));
    }

private:
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool               ok_   = false;
};

// [consumer]
// Takes the stream block by block until the end. A block stays valid until it is
// released; releasing it lets the pipe decommit the memory behind it.
bool HashStream(TC::Memory::IReadSequentialMemory& stream, Sha256& result) noexcept
{
    constexpr std::size_t BlockSize = 256 * 1024;
    Hasher                hasher;
    for (;;) {
        std::size_t size
            = BlockSize; // in: wanted; out: what the block has (less at the end)
        const std::byte* data
            = stream.RequestReadMemory(size); // suspends until there is enough data
        if (data == nullptr)
            break; // end of the stream
        hasher.Add(data, size);
        stream.ReleaseReadMemory(size);
    }
    return hasher.Finish(result);
}
// [/consumer]

int main()
{
    // A file of 64 MB of pseudo-random bytes, written with plain C++ streams.
    const std::filesystem::path path
        = std::filesystem::temp_directory_path() / L"ConcurrentTasks-hashing.bin";
    std::vector<std::byte> content(64 * 1024 * 1024);
    std::uint64_t          x = 0x9E3779B97F4A7C15;
    for (std::byte& b : content) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        b = static_cast<std::byte>(x >> 56);
    }
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(content.data()),
                   static_cast<std::streamsize>(content.size()));
        if (!file)
            return 1;
    }

    TC::ConvertThreadToTask();
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    TC::Windows::ErrorCode readError = Error::Success;
    bool                   hashed    = false;
    Sha256                 hash{};
    {
        // [pipe]
        // One producer task, one consumer task, one pipe between them. The pipe,
        // the tasks and the group live until the scheduler has stopped.
        TC::Memory::SpscVirtualMemoryPipe pipe;

        auto readTask = TC::Tasks::Task::Create([&] {
            // Keeps several overlapped reads in flight and hands every finished
            // block to the pipe in file order; ends the stream also after an error.
            readError = TC::AsyncIo::AsyncFile::ReadFile(
                path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, &pipe);
        });
        auto hashTask
            = TC::Tasks::Task::Create([&] { hashed = HashStream(pipe, hash); });

        TC::Synchronization::TaskGroup_s group;
        group.AddTask(&readTask);
        group.AddTask(&hashTask);
        scheduler->AddTask(&readTask);
        scheduler->AddTask(&hashTask);
        group.Wait();
        // [/pipe]

        scheduler->Stop();
    }
    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();

    // The same hash over the bytes in memory, to check the streamed one.
    Sha256 expected{};
    Hasher direct;
    direct.Add(content.data(), content.size());
    const bool ok = readError == Error::Success && hashed && direct.Finish(expected)
        && hash == expected;

    std::printf("SHA-256 of %zu bytes: ", content.size());
    for (std::byte b : hash)
        std::printf("%02x", std::to_integer<unsigned>(b));
    std::printf(" (%s)\n", ok ? "ok" : "MISMATCH");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return ok ? 0 : 1;
}
