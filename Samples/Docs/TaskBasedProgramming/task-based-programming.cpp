// Chapter "Task-based programming": the same work - read a file, hash it with MD5 and SHA-1 - first
// as one procedural loop, then as tasks that share memory: with the synchronization written out in
// the tasks, then hidden in the memory object, whose users know only GetMemory and ReleaseMemory. Every
// version checks its hashes against the loop's, the task versions with the tasks added in every
// order. Windows only (BCrypt).
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Schedulers.LocalScheduler;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Schedulers::LocalScheduler;
using TC::Synchronization::Semaphore_s;
using TC::Tasks::Task;
// [/names]

// [helpers]
// A file, read front to back.
class File
{
public:
    File() noexcept = default;
    File(const File&)            = delete;
    File& operator=(const File&) = delete;
    ~File() noexcept { Close(); }

    bool Open(const wchar_t* path) noexcept
    {
        handle_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }

    // Fills buffer as far as the file goes; 0 at its end.
    std::size_t Read(std::span<std::byte> buffer) noexcept
    {
        DWORD read = 0;
        return ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ? read : 0;
    }

    void Close() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// A hash over data that comes in pieces: MD5, SHA-1 or SHA-256 from BCrypt.
class Hash
{
public:
    explicit Hash(BCRYPT_ALG_HANDLE algorithm) noexcept { BCryptCreateHash(algorithm, &hash_, nullptr, 0, nullptr, 0, 0); }
    Hash(const Hash&)            = delete;
    Hash& operator=(const Hash&) = delete;
    ~Hash() noexcept { BCryptDestroyHash(hash_); }

    void Update(std::span<const std::byte> data) noexcept
    {
        BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())), static_cast<ULONG>(data.size()), 0);
    }

    // The digest as lowercase hex.
    std::string Finish() noexcept
    {
        DWORD length = 0;
        DWORD bytes  = 0;
        BCryptGetProperty(hash_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &bytes, 0);
        std::array<unsigned char, 64> digest{};
        BCryptFinishHash(hash_, digest.data(), length, 0);

        std::string hex;
        for (DWORD i = 0; i < length; ++i) {
            char two[3];
            std::snprintf(two, sizeof(two), "%02x", digest[i]);
            hex += two;
        }
        return hex;
    }

private:
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

struct Hashes {
    std::string md5;
    std::string sha1;
};

constexpr std::size_t BlockSize = 64 * 1024;
// [/helpers]

// 1. The procedural original.
namespace Procedural {
    // [procedural]
    Hashes HashFile(const wchar_t* path)
    {
        std::vector<std::byte> memory(BlockSize);
        File                   file;
        file.Open(path);
        Hash md5(BCRYPT_MD5_ALG_HANDLE);
        Hash sha1(BCRYPT_SHA1_ALG_HANDLE);

        for (;;) {
            const std::size_t size = file.Read(memory);
            if (size == 0)
                break;
            md5.Update(std::span(memory).first(size));
            sha1.Update(std::span(memory).first(size));
        }

        file.Close();
        return {md5.Finish(), sha1.Finish()};
    }
    // [/procedural]
} // namespace Procedural

// 4. Tasks with shared memory; the synchronization is written out in the tasks.
namespace Explicit {
    // [explicit-shared]
    // What the three tasks share: one block of memory and, for each hash, a signal that the block is
    // filled and one that the hash is done with it.
    struct Shared {
        std::vector<std::byte> memory = std::vector<std::byte>(BlockSize);
        std::size_t            size   = 0; // bytes in memory; 0: the file has ended

        Semaphore_s md5Filled{0};
        Semaphore_s sha1Filled{0};
        Semaphore_s md5Free{1};
        Semaphore_s sha1Free{1};
    };
    // [/explicit-shared]

    // [explicit-tasks]
    void Read(Shared& shared, const wchar_t* path)
    {
        File file;
        file.Open(path);
        for (;;) {
            shared.md5Free.Wait(); // both hashes are done with the block
            shared.sha1Free.Wait();
            shared.size = file.Read(shared.memory);
            shared.md5Filled.Release(); // both may hash it
            shared.sha1Filled.Release();
            if (shared.size == 0)
                break;
        }
        file.Close();
    }

    void Md5(Shared& shared, std::string& result)
    {
        Hash md5(BCRYPT_MD5_ALG_HANDLE);
        for (;;) {
            shared.md5Filled.Wait();
            if (shared.size == 0)
                break;
            md5.Update(std::span(shared.memory).first(shared.size));
            shared.md5Free.Release();
        }
        result = md5.Finish();
    }

    void Sha1(Shared& shared, std::string& result)
    {
        Hash sha1(BCRYPT_SHA1_ALG_HANDLE);
        for (;;) {
            shared.sha1Filled.Wait();
            if (shared.size == 0)
                break;
            sha1.Update(std::span(shared.memory).first(shared.size));
            shared.sha1Free.Release();
        }
        result = sha1.Finish();
    }
    // [/explicit-tasks]

    // [explicit-run]
    // order: the order in which the three tasks are added, e.g. {2, 0, 1}
    Hashes HashFile(const wchar_t* path, const std::array<int, 3>& order)
    {
        Shared shared;
        Hashes hashes;

        auto  read  = Task::Create([&] { Read(shared, path); });
        auto  md5   = Task::Create([&] { Md5(shared, hashes.md5); });
        auto  sha1  = Task::Create([&] { Sha1(shared, hashes.sha1); });
        Task* tasks[] = {&read, &md5, &sha1};

        LocalScheduler scheduler;
        for (int i : order)
            tasks[i]->Schedule(scheduler);
        scheduler.Run(); // returns once all three have ended
        return hashes;
    }
    // [/explicit-run]
} // namespace Explicit

// What can go wrong: a missing dependency. The tasks of Explicit, but the reader does not wait for
// SHA-1 before it fills the buffer again. Whether that goes wrong depends on the order in which the
// tasks happen to run.
namespace MissingDependency {
    // [missing-dependency]
    void ReadTooEarly(Explicit::Shared& shared, const wchar_t* path)
    {
        File file;
        file.Open(path);
        for (;;) {
            shared.md5Free.Wait(); // wrong: SHA-1 may not be done with the block yet
            shared.size = file.Read(shared.memory);
            shared.md5Filled.Release();
            shared.sha1Filled.Release();
            if (shared.size == 0)
                break;
        }
        file.Close();
    }
    // [/missing-dependency]

    Hashes HashFile(const wchar_t* path, const std::array<int, 3>& order)
    {
        Explicit::Shared shared;
        Hashes           hashes;

        auto  read    = Task::Create([&] { ReadTooEarly(shared, path); });
        auto  md5     = Task::Create([&] { Explicit::Md5(shared, hashes.md5); });
        auto  sha1    = Task::Create([&] { Explicit::Sha1(shared, hashes.sha1); });
        Task* tasks[] = {&read, &md5, &sha1};

        LocalScheduler scheduler;
        for (int i : order)
            tasks[i]->Schedule(scheduler);
        scheduler.Run();
        return hashes;
    }
} // namespace MissingDependency

// 5. The synchronization hidden in the memory object; the tasks know only GetMemory and ReleaseMemory.
namespace Hidden {
    // [shared-memory]
    // Blocks of memory that one writer fills and every reader reads, in the order written. Each task
    // has a port of its own with two calls: GetMemory waits for the next block this task may use;
    // ReleaseMemory hands it on - from the writer to every reader, and back to the writer once every
    // reader has released it. A reader's GetMemory returns an empty block once the writer has released
    // one.
    class SharedMemory
    {
    public:
        // [memory-interface]
        struct Block {
            std::span<std::byte> data;
            std::size_t          size = 0; // bytes in data
        };

        class Port
        {
        public:
            Block GetMemory() noexcept { return owner_->Get(*this); }
            void  ReleaseMemory(const Block& block) noexcept { owner_->Release(*this, block); }

        private:
            friend class SharedMemory;
            SharedMemory* owner_  = nullptr;
            std::size_t   reader_ = 0; // the reader's index; Writer for the writer
            std::size_t   next_   = 0; // the number of the block this port uses next
        };
        // [/memory-interface]

        SharedMemory(std::size_t blocks, std::size_t readers, std::size_t blockSize)
            : blocks_(blocks), readers_(readers), blockSize_(blockSize), memory_(blocks * blockSize), sizes_(blocks),
              ports_(readers + 1), filled_(std::make_unique<Semaphore_s[]>(readers)), free_(std::make_unique<Semaphore_s[]>(readers))
        {
            for (std::size_t r = 0; r < readers; ++r) {
                free_[r].Release(blocks); // every block is free for every reader at first
                ports_[r].reader_ = r;
            }
            ports_[readers].reader_ = Writer;
            for (auto& port : ports_)
                port.owner_ = this;
        }

        Port& WriterPort() noexcept { return ports_[readers_]; }
        Port& ReaderPort(std::size_t reader) noexcept { return ports_[reader]; }

    private:
        static constexpr std::size_t Writer = ~std::size_t{0};

        Block Get(Port& port) noexcept
        {
            const std::size_t slot = port.next_ % blocks_;
            if (port.reader_ == Writer) {
                // free once every reader has released it: each reader releases in order
                for (std::size_t r = 0; r < readers_; ++r)
                    free_[r].Wait();
                return {Data(slot), 0};
            }
            filled_[port.reader_].Wait();
            return {Data(slot), sizes_[slot]};
        }

        void Release(Port& port, const Block& block) noexcept
        {
            const std::size_t slot = port.next_++ % blocks_;
            if (port.reader_ == Writer) {
                sizes_[slot] = block.size;
                for (std::size_t r = 0; r < readers_; ++r)
                    filled_[r].Release();
            } else
                free_[port.reader_].Release();
        }

        std::span<std::byte> Data(std::size_t slot) noexcept { return std::span(memory_).subspan(slot * blockSize_, blockSize_); }

        std::size_t                    blocks_;
        std::size_t                    readers_;
        std::size_t                    blockSize_;
        std::vector<std::byte>         memory_;
        std::vector<std::size_t>       sizes_;
        std::vector<Port>              ports_;  // the readers', then the writer's
        std::unique_ptr<Semaphore_s[]> filled_; // per reader: blocks it may read
        std::unique_ptr<Semaphore_s[]> free_;   // per reader: blocks it has released
    };
    // [/shared-memory]

    // [memory-tasks]
    void Read(SharedMemory::Port& memory, const wchar_t* path)
    {
        File file;
        file.Open(path);
        for (;;) {
            auto block = memory.GetMemory();
            block.size = file.Read(block.data);
            memory.ReleaseMemory(block);
            if (block.size == 0)
                break;
        }
        file.Close();
    }

    // MD5, SHA-1, SHA-256 alike: the algorithm is a parameter
    void HashBlocks(SharedMemory::Port& memory, BCRYPT_ALG_HANDLE algorithm, std::string& result)
    {
        Hash hash(algorithm);
        for (auto block = memory.GetMemory(); block.size != 0; block = memory.GetMemory()) {
            hash.Update(block.data.first(block.size));
            memory.ReleaseMemory(block);
        }
        result = hash.Finish();
    }
    // [/memory-tasks]

    // [memory-run]
    Hashes HashFile(const wchar_t* path, const std::array<int, 3>& order)
    {
        SharedMemory memory(1, 2, BlockSize); // one block, two readers: as in the loop
        Hashes       hashes;

        auto  read    = Task::Create([&] { Read(memory.WriterPort(), path); });
        auto  md5     = Task::Create([&] { HashBlocks(memory.ReaderPort(0), BCRYPT_MD5_ALG_HANDLE, hashes.md5); });
        auto  sha1    = Task::Create([&] { HashBlocks(memory.ReaderPort(1), BCRYPT_SHA1_ALG_HANDLE, hashes.sha1); });
        Task* tasks[] = {&read, &md5, &sha1};

        LocalScheduler scheduler;
        for (int i : order)
            tasks[i]->Schedule(scheduler);
        scheduler.Run();
        return hashes;
    }
    // [/memory-run]

    // [reuse]
    // One more hash: one more reader and one more task. Read and the other tasks stay as they are.
    Hashes HashFileWithSha256(const wchar_t* path, std::string& sha256Result)
    {
        SharedMemory memory(1, 3, BlockSize);
        Hashes       hashes;

        auto read   = Task::Create([&] { Read(memory.WriterPort(), path); });
        auto md5    = Task::Create([&] { HashBlocks(memory.ReaderPort(0), BCRYPT_MD5_ALG_HANDLE, hashes.md5); });
        auto sha1   = Task::Create([&] { HashBlocks(memory.ReaderPort(1), BCRYPT_SHA1_ALG_HANDLE, hashes.sha1); });
        auto sha256 = Task::Create([&] { HashBlocks(memory.ReaderPort(2), BCRYPT_SHA256_ALG_HANDLE, sha256Result); });

        LocalScheduler scheduler;
        Task*          tasks[] = {&read, &md5, &sha1, &sha256};
        for (Task* task : tasks)
            task->Schedule(scheduler);
        scheduler.Run();
        return hashes;
    }
    // [/reuse]

    // [scaling]
    // The hashes on the threads of a default scheduler, the reader on the local one; with more
    // blocks, reading runs ahead of hashing. The tasks are the same.
    Hashes HashFileOnWorkers(const wchar_t* path, TC::ITaskScheduler& workers)
    {
        SharedMemory memory(8, 2, BlockSize);
        Hashes       hashes;

        auto md5  = Task::Create([&] { HashBlocks(memory.ReaderPort(0), BCRYPT_MD5_ALG_HANDLE, hashes.md5); });
        auto sha1 = Task::Create([&] { HashBlocks(memory.ReaderPort(1), BCRYPT_SHA1_ALG_HANDLE, hashes.sha1); });
        auto read = Task::Create([&] {
            TC::Synchronization::TaskGroup_s hashing; // the local scheduler does not see tasks on workers
            hashing.AddTask(&md5);
            md5.Schedule(workers);
            hashing.AddTask(&sha1);
            sha1.Schedule(workers);
            Read(memory.WriterPort(), path);
            hashing.Wait();
        });

        LocalScheduler scheduler;
        read.Schedule(scheduler);
        scheduler.Run();
        return hashes;
    }
    // [/scaling]
} // namespace Hidden

namespace {
    // A test file of a size that is no multiple of the block size, with content that is not all zeros.
    bool CreateTestFile(const wchar_t* path)
    {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        std::vector<std::byte> data(3 * 1024 * 1024 + 12345);
        std::uint32_t          x = 2463534242u;
        for (auto& b : data) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            b = static_cast<std::byte>(x);
        }
        DWORD      written = 0;
        const bool ok      = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
        CloseHandle(file);
        return ok;
    }

    // SHA-256 the plain way, to check the reuse example.
    std::string Sha256Of(const wchar_t* path)
    {
        std::vector<std::byte> memory(BlockSize);
        File                   file;
        file.Open(path);
        Hash sha256(BCRYPT_SHA256_ALG_HANDLE);
        for (std::size_t size; (size = file.Read(memory)) != 0;)
            sha256.Update(std::span(memory).first(size));
        return sha256.Finish();
    }

    bool Same(const Hashes& a, const Hashes& b) noexcept { return a.md5 == b.md5 && a.sha1 == b.sha1; }
} // namespace

int wmain(int argc, wchar_t* argv[])
{
    wchar_t testFile[MAX_PATH] = {};
    const wchar_t* path        = argc > 1 ? argv[1] : nullptr;
    if (!path) {
        wchar_t directory[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"ctb", 0, testFile) || !CreateTestFile(testFile)) {
            std::printf("could not create a test file\n");
            return 1;
        }
        path = testFile;
    }

    // The versions below do not check their file: a path that cannot be read would give the hashes
    // of nothing in all of them, and they would agree.
    if (File probe; !probe.Open(path)) {
        std::printf("cannot open the file\n");
        return 1;
    }

    const Hashes expected = Procedural::HashFile(path);
    std::printf("procedural:  MD5 %s  SHA-1 %s\n", expected.md5.c_str(), expected.sha1.c_str());

    bool ok = true;

    // every order in which the three tasks can be added
    std::array<int, 3> order{0, 1, 2};
    int                orders = 0;
    do {
        ok = ok && Same(Explicit::HashFile(path, order), expected) && Same(Hidden::HashFile(path, order), expected);
        ++orders;
    } while (std::next_permutation(order.begin(), order.end()));
    std::printf("tasks, explicit and hidden synchronization, %d orders: %s\n", orders, ok ? "same hashes" : "MISMATCH");

    // [order-test]
    // The same test finds a missing dependency: the hashes are right in some orders and wrong in
    // others - on one thread, so in every run alike.
    int right = 0;
    do {
        if (Same(MissingDependency::HashFile(path, order), expected))
            ++right;
    } while (std::next_permutation(order.begin(), order.end()));
    std::printf("reader that does not wait for SHA-1: right in %d of %d orders\n", right, orders);
    // [/order-test]
    ok = ok && right < orders; // the mistake must show

    std::string sha256;
    const bool  reuse = Same(Hidden::HashFileWithSha256(path, sha256), expected) && sha256 == Sha256Of(path);
    std::printf("one more hash task (SHA-256 %s): %s\n", sha256.c_str(), reuse ? "same hashes" : "MISMATCH");
    ok = ok && reuse;

    TC::SchedulerInfo info{};
    auto*             workers = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    workers->Start();
    const bool scaled = Same(Hidden::HashFileOnWorkers(path, *workers), expected);
    workers->Stop();
    workers->Destroy();
    std::printf("hashes on worker threads: %s\n", scaled ? "same hashes" : "MISMATCH");
    ok = ok && scaled;

    if (testFile[0])
        DeleteFileW(testFile);
    std::printf("%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
