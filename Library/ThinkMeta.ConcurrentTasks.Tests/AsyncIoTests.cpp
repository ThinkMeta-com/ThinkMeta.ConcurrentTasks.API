#include "pch.h"
#include "TestHelpers.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFileIoRequest;
import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    static std::wstring GetTempFilePath()
    {
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];

        ::GetTempPathW(MAX_PATH, tempPath);
        ::GetTempFileNameW(tempPath, L"CT_AsyncIo", 0, tempFile);

        return std::wstring(tempFile);
    }

    // Minimal contiguous sink for AsyncFile::ReadFile bulk reads. It pre-allocates on
    // Initialize and hands out distinct sub-spans of the single buffer per write request.
    class VectorWriteSequentialMemory : public Memory::IWriteSequentialMemory
    {
    public:
        void Initialize(std::size_t initialSize) noexcept override
        {
            buffer_.assign(initialSize, std::byte{0});
            reservePos_ = 0;
            releasedSize_ = 0;
            endOfStream_ = false;
        }

        std::byte* RequestWriteMemory(std::size_t size) noexcept override
        {
            // Hand out the next distinct region so pipelined reads never overlap and the
            // pre-allocated buffer is never reallocated (which would invalidate in-flight pointers).
            std::byte* ptr = buffer_.data() + reservePos_;
            reservePos_ += size;
            return ptr;
        }

        void ReleaseWriteMemory(std::size_t size) noexcept override { releasedSize_ += size; }

        void SetEndOfStream() noexcept override { endOfStream_ = true; }

        const std::vector<std::byte>& GetBuffer() const noexcept { return buffer_; }
        std::size_t GetWrittenSize() const noexcept { return releasedSize_; }
        bool IsEndOfStream() const noexcept { return endOfStream_; }

    private:
        std::vector<std::byte> buffer_;
        std::size_t            reservePos_ = 0;
        std::size_t            releasedSize_ = 0;
        bool                   endOfStream_ = false;
    };

    // Hands out a byte vector in blocks for AsyncFile::WriteFile; several blocks may be out at once.
    class VectorReadSequentialMemory : public Memory::IReadSequentialMemory
    {
    public:
        explicit VectorReadSequentialMemory(const std::vector<std::byte>& data) : data_(data) {}

        std::size_t GetSize() const noexcept override { return data_.size(); }

        const std::byte* RequestReadMemory(std::size_t& size) noexcept override
        {
            const std::size_t left = data_.size() - handedOut_;
            if (left == 0) {
                size = 0;
                return nullptr;
            }
            if (size > left)
                size = left;
            const std::byte* at = data_.data() + handedOut_;
            handedOut_ += size;
            return at;
        }

        void ReleaseReadMemory(std::size_t size) noexcept override { releasedSize_ += size; }

        std::size_t GetReleasedSize() const noexcept { return releasedSize_; }

    private:
        const std::vector<std::byte>& data_;
        std::size_t                   handedOut_ = 0;
        std::size_t                   releasedSize_ = 0;
    };

    // A sink that runs out of memory after a number of blocks: RequestWriteMemory returns nullptr.
    class FailingWriteSequentialMemory : public Memory::IWriteSequentialMemory
    {
    public:
        explicit FailingWriteSequentialMemory(std::size_t blocksBeforeFailure) : blocksLeft_(blocksBeforeFailure) {}

        void Initialize(std::size_t initialSize) noexcept override { buffer_.assign(initialSize, std::byte{0}); }

        std::byte* RequestWriteMemory(std::size_t size) noexcept override
        {
            if (blocksLeft_ == 0)
                return nullptr;
            --blocksLeft_;
            std::byte* ptr = buffer_.data() + requestedSize_;
            requestedSize_ += size;
            return ptr;
        }

        void ReleaseWriteMemory(std::size_t size) noexcept override { releasedSize_ += size; }
        void SetEndOfStream() noexcept override { endOfStream_ = true; }

        std::size_t GetRequestedSize() const noexcept { return requestedSize_; }
        std::size_t GetReleasedSize() const noexcept { return releasedSize_; }
        bool IsEndOfStream() const noexcept { return endOfStream_; }

    private:
        std::vector<std::byte> buffer_;
        std::size_t            blocksLeft_;
        std::size_t            requestedSize_ = 0;
        std::size_t            releasedSize_ = 0;
        bool                   endOfStream_ = false;
    };

    // A file of the given size with byte i = i * 13 + 5, written synchronously.
    static void WritePatternFile(const std::wstring& path, std::size_t size)
    {
        std::vector<std::byte> data(size);
        for (std::size_t i = 0; i < size; ++i)
            data[i] = static_cast<std::byte>(i * 13 + 5);
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::IsTrue(file != INVALID_HANDLE_VALUE, L"Failed to create pattern file");
        DWORD written = 0;
        const BOOL ok = ::WriteFile(file, data.data(), static_cast<DWORD>(size), &written, nullptr);
        ::CloseHandle(file);
        Assert::IsTrue(ok && written == size, L"Failed to write pattern file");
    }

    TEST_CLASS(AsyncIoTests)
    {
    public:
        TEST_METHOD(TestAsyncFileWriteAndRead)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring tempFilePath = GetTempFilePath();
            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&tempFilePath, &testPassed]() {
                AsyncIo::AsyncFile file;

                auto err = file.Create(
                    const_cast<wchar_t*>(tempFilePath.c_str()),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL
                );

                Assert::IsTrue(Windows::Error::Succeeded(err), L"Failed to create async temp file");

                const std::string writeData = "Async I/O Test Payload for ConcurrentTasks!";
                const DWORD writeSize = static_cast<DWORD>(writeData.size());

                // Write Async
                AsyncIo::WaitableAsyncFileIoRequest writeRequest;
                writeRequest.SetOffset(0);
                err = file.WriteAsync(writeData.data(), writeSize, writeRequest);

                if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                    writeRequest.Wait();
                    Assert::AreEqual(writeSize, writeRequest.GetTransferredBytes(), L"Bytes written match expected size");
                } else {
                    Assert::Fail(L"WriteAsync failed");
                }

                // Read Async
                std::vector<char> readBuffer(writeSize, 0);
                AsyncIo::WaitableAsyncFileIoRequest readRequest;
                readRequest.SetOffset(0);
                err = file.ReadAsync(readBuffer.data(), writeSize, readRequest);

                if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                    readRequest.Wait();
                    Assert::AreEqual(writeSize, readRequest.GetTransferredBytes(), L"Bytes read match expected size");
                } else {
                    Assert::Fail(L"ReadAsync failed");
                }

                std::string readData(readBuffer.begin(), readBuffer.end());
                Assert::AreEqual(writeData, readData, L"Read payload matches written payload");

                file.Close();
                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(10));
            Assert::IsTrue(finished, L"AsyncFile write and read test timed out!");

            AsyncIo::DisableAsyncIo();

            ::DeleteFileW(tempFilePath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileConcurrentOffsetOperations)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 4};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring tempFilePath = GetTempFilePath();
            constexpr std::size_t NumSegments = 16;
            constexpr std::size_t SegmentSize = 64 * 1024 * 1024; // 64 MB per segment = 1 GB total scattered offset operations

            // Prepare test file space for 1 GB
            {
                AsyncIo::AsyncFile initFile;
                initFile.Create(const_cast<wchar_t*>(tempFilePath.c_str()), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
                std::vector<char> initChunk(SegmentSize, 0);
                for (std::size_t i = 0; i < NumSegments; ++i) {
                    DWORD written = 0;
                    initFile.Write(initChunk.data(), static_cast<DWORD>(SegmentSize), written, static_cast<AsyncIo::AsyncFile::FileSize>(i * SegmentSize));
                }
                initFile.Close();
            }

            AsyncIo::AsyncFile sharedFile;
            auto createErr = sharedFile.Create(
                const_cast<wchar_t*>(tempFilePath.c_str()),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL
            );
            Assert::IsTrue(Windows::Error::Succeeded(createErr), L"Failed to open shared file for async offset ops");

            Synchronization::TaskGroup taskGroup;
            std::vector<std::unique_ptr<ActionTask>> tasks;
            tasks.reserve(NumSegments);
            std::atomic<std::size_t> completedSegments{0};

            for (std::size_t i = 0; i < NumSegments; ++i) {
                std::uint64_t offset = i * SegmentSize;
                char fillChar = static_cast<char>('A' + i);

                tasks.push_back(std::make_unique<ActionTask>([&sharedFile, offset, fillChar, SegmentSize, &completedSegments]() {
                    std::vector<char> writeBuf(SegmentSize, fillChar);
                    AsyncIo::WaitableAsyncFileIoRequest writeReq;
                    writeReq.SetOffset(offset);

                    auto err = sharedFile.WriteAsync(writeBuf.data(), static_cast<DWORD>(SegmentSize), writeReq);
                    if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                        writeReq.Wait();
                    }

                    std::vector<char> readBuf(SegmentSize, 0);
                    AsyncIo::WaitableAsyncFileIoRequest readReq;
                    readReq.SetOffset(offset);

                    err = sharedFile.ReadAsync(readBuf.data(), static_cast<DWORD>(SegmentSize), readReq);
                    if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                        readReq.Wait();
                    }

                    bool match = true;
                    for (std::size_t step = 0; step < SegmentSize; step += 4096) {
                        if (readBuf[step] != fillChar) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        completedSegments.fetch_add(1, std::memory_order_release);
                    }
                }));

                taskGroup.AddTask(tasks.back().get());
                scheduler.AddTask(tasks.back().get(), TaskPriority::Normal);
            }

            taskGroup.Wait();

            Assert::AreEqual(NumSegments, completedSegments.load(std::memory_order_acquire), L"All 16 concurrent 64MB scattered offset segments completed successfully");

            sharedFile.Close();
            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(tempFilePath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileSyncWrappersAndGetSize)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring tempFilePath = GetTempFilePath();
            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&tempFilePath, &testPassed]() {
                AsyncIo::AsyncFile file;

                auto err = file.Create(
                    const_cast<wchar_t*>(tempFilePath.c_str()),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL
                );
                Assert::IsTrue(Windows::Error::Succeeded(err), L"Failed to create async temp file");

                const std::string writeData = "Synchronous wrapper payload!";
                const DWORD writeSize = static_cast<DWORD>(writeData.size());

                // Position-based synchronous Write wrapper (internally uses _s request).
                DWORD bytesWritten = 0;
                err = file.Write(writeData.data(), writeSize, bytesWritten, 0);
                Assert::IsTrue(Windows::Error::Succeeded(err), L"Sync Write failed");
                Assert::AreEqual(writeSize, bytesWritten, L"Sync Write transferred byte count mismatch");

                // GetSize must reflect what was written.
                AsyncIo::AsyncFile::FileSize size = 0;
                err = file.GetSize(size);
                Assert::IsTrue(Windows::Error::Succeeded(err), L"GetSize failed");
                Assert::AreEqual(static_cast<AsyncIo::AsyncFile::FileSize>(writeSize), size, L"GetSize does not match written size");

                // Position-based synchronous Read wrapper.
                std::vector<char> readBuffer(writeSize, 0);
                DWORD bytesRead = 0;
                err = file.Read(readBuffer.data(), writeSize, bytesRead, 0);
                Assert::IsTrue(Windows::Error::Succeeded(err), L"Sync Read failed");
                Assert::AreEqual(writeSize, bytesRead, L"Sync Read transferred byte count mismatch");

                std::string readData(readBuffer.begin(), readBuffer.end());
                Assert::AreEqual(writeData, readData, L"Sync read payload mismatch");

                file.Close();
                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(10));
            Assert::IsTrue(finished, L"AsyncFile sync wrappers test timed out!");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(tempFilePath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileSingleWaiterAndRequestReuse)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring tempFilePath = GetTempFilePath();
            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&tempFilePath, &testPassed]() {
                AsyncIo::AsyncFile file;

                auto err = file.Create(
                    const_cast<wchar_t*>(tempFilePath.c_str()),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL
                );
                Assert::IsTrue(Windows::Error::Succeeded(err), L"Failed to create async temp file");

                constexpr DWORD BlockSize = 32;
                constexpr std::size_t NumBlocks = 4;

                // Reuse a single single-waiter request object across multiple writes.
                AsyncIo::WaitableAsyncFileIoRequest_s writeRequest;
                for (std::size_t i = 0; i < NumBlocks; ++i) {
                    std::vector<char> block(BlockSize, static_cast<char>('0' + i));

                    writeRequest.Reset();
                    writeRequest.SetOffset(i * BlockSize);
                    err = file.WriteAsync(block.data(), BlockSize, writeRequest);

                    if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                        writeRequest.Wait();
                        Assert::IsTrue(Windows::Error::Succeeded(writeRequest.GetError()), L"Reused write request reported an error");
                        Assert::AreEqual(BlockSize, writeRequest.GetTransferredBytes(), L"Reused write request byte count mismatch");
                    } else {
                        Assert::Fail(L"WriteAsync (reused request) failed");
                    }
                }

                // Reuse a single single-waiter request object across multiple reads.
                AsyncIo::WaitableAsyncFileIoRequest_s readRequest;
                for (std::size_t i = 0; i < NumBlocks; ++i) {
                    std::vector<char> block(BlockSize, 0);

                    readRequest.Reset();
                    readRequest.SetOffset(i * BlockSize);
                    err = file.ReadAsync(block.data(), BlockSize, readRequest);

                    if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                        readRequest.Wait();
                        Assert::IsTrue(Windows::Error::Succeeded(readRequest.GetError()), L"Reused read request reported an error");
                        Assert::AreEqual(BlockSize, readRequest.GetTransferredBytes(), L"Reused read request byte count mismatch");
                    } else {
                        Assert::Fail(L"ReadAsync (reused request) failed");
                    }

                    const char expected = static_cast<char>('0' + i);
                    bool match = std::all_of(block.begin(), block.end(), [expected](char c) { return c == expected; });
                    Assert::IsTrue(match, L"Reused read block content mismatch");
                }

                file.Close();
                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(10));
            Assert::IsTrue(finished, L"AsyncFile single-waiter/request-reuse test timed out!");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(tempFilePath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileEofAndErrorPaths)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring tempFilePath = GetTempFilePath();
            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&tempFilePath, &testPassed]() {
                // Opening a non-existent file with OPEN_EXISTING must fail.
                {
                    AsyncIo::AsyncFile missingFile;
                    const std::wstring missingPath = tempFilePath + L".__no_such_file__";
                    auto err = missingFile.Create(
                        const_cast<wchar_t*>(missingPath.c_str()),
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL
                    );
                    Assert::IsTrue(Windows::Error::Failed(err), L"Opening a non-existent file should fail");
                    Assert::IsFalse(missingFile.IsValid(), L"Missing file handle should be invalid");
                }

                AsyncIo::AsyncFile file;
                auto err = file.Create(
                    const_cast<wchar_t*>(tempFilePath.c_str()),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL
                );
                Assert::IsTrue(Windows::Error::Succeeded(err), L"Failed to create async temp file");

                const std::string writeData = "small";
                const DWORD writeSize = static_cast<DWORD>(writeData.size());
                DWORD bytesWritten = 0;
                err = file.Write(writeData.data(), writeSize, bytesWritten, 0);
                Assert::IsTrue(Windows::Error::Succeeded(err), L"Sync Write failed");

                // Reading beyond EOF: request more bytes than the file contains.
                constexpr DWORD ReadSize = 512;
                std::vector<char> readBuffer(ReadSize, 0);
                AsyncIo::WaitableAsyncFileIoRequest readRequest;
                readRequest.SetOffset(0);
                err = file.ReadAsync(readBuffer.data(), ReadSize, readRequest);

                if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                    readRequest.Wait();
                    // A read that hits EOF returns only the available bytes (or a handled EOF error).
                    Assert::IsTrue(readRequest.GetTransferredBytes() <= writeSize, L"EOF read must not report more than what exists");
                }

                // Reading entirely past EOF should transfer zero bytes.
                AsyncIo::WaitableAsyncFileIoRequest pastEofRequest;
                pastEofRequest.SetOffset(4096);
                std::vector<char> pastBuffer(ReadSize, 0);
                err = file.ReadAsync(pastBuffer.data(), ReadSize, pastEofRequest);
                if (Windows::Error::Succeeded(err) || err == ERROR_IO_PENDING) {
                    pastEofRequest.Wait();
                    Assert::AreEqual(static_cast<DWORD>(0), pastEofRequest.GetTransferredBytes(), L"Read entirely past EOF must transfer zero bytes");
                }

                file.Close();
                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(10));
            Assert::IsTrue(finished, L"AsyncFile EOF/error paths test timed out!");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(tempFilePath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileReadFileBulk)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 4};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring tempFilePath = GetTempFilePath();

            // 1 GiB payload (1,073,741,824 bytes) to benchmark NVMe throughput.
            constexpr std::size_t TotalPayloadSize = 1024ULL * 1024ULL * 1024ULL; // 1 GB
            constexpr std::size_t ChunkSize        = 64 * 1024 * 1024;             // 64 MB chunk
            constexpr std::size_t NumChunks        = TotalPayloadSize / ChunkSize;

            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&tempFilePath, &testPassed, TotalPayloadSize, ChunkSize, NumChunks]() {
                // Write 1 GB in 64 MB chunks using the async file API.
                {
                    AsyncIo::AsyncFile writeFile;
                    auto err = writeFile.Create(
                        const_cast<wchar_t*>(tempFilePath.c_str()),
                        GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL
                    );
                    Assert::IsTrue(Windows::Error::Succeeded(err), L"Failed to create file for 1 GB bulk write");

                    std::vector<char> chunkBuffer(ChunkSize);
                    for (std::size_t c = 0; c < NumChunks; ++c) {
                        const std::size_t offset = c * ChunkSize;
                        for (std::size_t i = 0; i < ChunkSize; ++i) {
                            chunkBuffer[i] = static_cast<char>((offset + i) * 31 + 7);
                        }

                        DWORD written = 0;
                        err = writeFile.Write(chunkBuffer.data(), static_cast<DWORD>(ChunkSize), written, static_cast<AsyncIo::AsyncFile::FileSize>(offset));
                        Assert::IsTrue(Windows::Error::Succeeded(err), L"Chunk write failed during 1 GB write");
                        Assert::AreEqual(static_cast<DWORD>(ChunkSize), written, L"Chunk write byte count mismatch");
                    }

                    writeFile.Close();
                }

                // Read the whole 1 GB file back through the static bulk ReadFile pipeline.
                VectorWriteSequentialMemory sink;
                auto err = AsyncIo::AsyncFile::ReadFile(
                    const_cast<wchar_t*>(tempFilePath.c_str()),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    &sink
                );

                Assert::IsTrue(Windows::Error::Succeeded(err), L"Bulk ReadFile 1 GB failed");
                Assert::IsTrue(sink.IsEndOfStream(), L"Bulk ReadFile 1 GB did not mark end of stream");
                Assert::AreEqual(TotalPayloadSize, sink.GetWrittenSize(), L"Bulk ReadFile 1 GB size mismatch");

                const auto& buffer = sink.GetBuffer();
                Assert::AreEqual(TotalPayloadSize, buffer.size(), L"Read buffer size must equal 1 GB");

                // Sample-check contents across chunks to verify integrity
                bool match = true;
                for (std::size_t c = 0; c < NumChunks; ++c) {
                    const std::size_t offset = c * ChunkSize;
                    for (std::size_t step = 0; step < ChunkSize; step += 4096) {
                        const std::size_t idx = offset + step;
                        if (static_cast<char>(buffer[idx]) != static_cast<char>(idx * 31 + 7)) {
                            match = false;
                            break;
                        }
                    }
                    if (!match) break;
                }
                Assert::IsTrue(match, L"1 GB ReadFile payload content mismatch");

                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(120));
            Assert::IsTrue(finished, L"1 GB AsyncFile bulk ReadFile test timed out!");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(tempFilePath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }
        TEST_METHOD(TestAsyncFileBulkOptionsAndOffset)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring sourcePath = GetTempFilePath();
            const std::wstring copyPath   = GetTempFilePath();
            constexpr std::size_t FileSize = 3 * 1024 * 1024 + 123; // not a multiple of any block size
            constexpr std::size_t Offset   = 1000;
            WritePatternFile(sourcePath, FileSize);

            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&sourcePath, &copyPath, &testPassed, FileSize, Offset]() {
                // read from an offset with a small shape: 64 KB x 3
                VectorWriteSequentialMemory part;
                auto err = AsyncIo::AsyncFile::ReadFile(const_cast<wchar_t*>(sourcePath.c_str()), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                                        FILE_ATTRIBUTE_NORMAL, &part, AsyncIo::BulkIoOptions{64 * 1024, 3, Offset});
                Assert::IsTrue(Windows::Error::Succeeded(err), L"ReadFile with offset failed");
                Assert::IsTrue(part.IsEndOfStream(), L"ReadFile with offset did not mark end of stream");
                Assert::AreEqual(FileSize - Offset, part.GetWrittenSize(), L"ReadFile with offset size mismatch");
                bool match = true;
                for (std::size_t i = 0; i < FileSize - Offset && match; ++i)
                    match = part.GetBuffer()[i] == static_cast<std::byte>((i + Offset) * 13 + 5);
                Assert::IsTrue(match, L"ReadFile with offset content mismatch");

                // read whole with the default shape, write a copy with 128 KB x 2, read the copy back
                VectorWriteSequentialMemory whole;
                err = AsyncIo::AsyncFile::ReadFile(const_cast<wchar_t*>(sourcePath.c_str()), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                                   FILE_ATTRIBUTE_NORMAL, &whole);
                Assert::IsTrue(Windows::Error::Succeeded(err) && whole.GetWrittenSize() == FileSize, L"ReadFile with the default shape failed");

                VectorReadSequentialMemory source(whole.GetBuffer());
                err = AsyncIo::AsyncFile::WriteFile(const_cast<wchar_t*>(copyPath.c_str()), GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                                    &source, AsyncIo::BulkIoOptions{128 * 1024, 2});
                Assert::IsTrue(Windows::Error::Succeeded(err), L"WriteFile with options failed");
                Assert::AreEqual(FileSize, source.GetReleasedSize(), L"WriteFile did not release every block");

                VectorWriteSequentialMemory copy;
                err = AsyncIo::AsyncFile::ReadFile(const_cast<wchar_t*>(copyPath.c_str()), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                                   FILE_ATTRIBUTE_NORMAL, &copy);
                Assert::IsTrue(Windows::Error::Succeeded(err), L"ReadFile of the copy failed");
                Assert::IsTrue(copy.GetBuffer() == whole.GetBuffer(), L"WriteFile copy differs from the source");

                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(30));
            Assert::IsTrue(finished, L"AsyncFile bulk options test timed out or failed");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(sourcePath.c_str());
            ::DeleteFileW(copyPath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileReadFileOutOfMemoryEndsCleanly)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring path = GetTempFilePath();
            constexpr std::size_t FileSize = 4 * 1024 * 1024;
            WritePatternFile(path, FileSize);

            std::atomic<bool> testPassed{false};

            auto ioTask = Tasks::Task::Create([&path, &testPassed]() {
                // the memory fails on the fourth block while up to three reads are in flight; ReadFile
                // must wait for them, report the error and end the stream
                FailingWriteSequentialMemory sink(3);
                const auto err = AsyncIo::AsyncFile::ReadFile(const_cast<wchar_t*>(path.c_str()), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                                              FILE_ATTRIBUTE_NORMAL, &sink, AsyncIo::BulkIoOptions{256 * 1024, 4});
                Assert::AreEqual(static_cast<unsigned long>(ERROR_NOT_ENOUGH_MEMORY), static_cast<unsigned long>(err), L"ReadFile did not report the failed block");
                Assert::IsTrue(sink.IsEndOfStream(), L"ReadFile did not end the stream after the failure");
                Assert::AreEqual(static_cast<std::size_t>(3 * 256 * 1024), sink.GetRequestedSize(), L"ReadFile requested blocks after the failure");
                Assert::IsTrue(sink.GetReleasedSize() <= sink.GetRequestedSize(), L"ReadFile released more than it read");

                testPassed.store(true, std::memory_order_release);
            });

            scheduler.AddTask(&ioTask, TaskPriority::Normal);

            const bool finished = WaitFor([&testPassed] { return testPassed.load(std::memory_order_acquire); }, std::chrono::seconds(30));
            Assert::IsTrue(finished, L"AsyncFile ReadFile out-of-memory test timed out or failed");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(path.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAsyncFileReadFileIntoPipeMissingAndEmptyFile)
        {
            ConvertThreadToTask();

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const std::wstring emptyPath = GetTempFilePath(); // GetTempFileName creates it empty
            const std::wstring missingPath = emptyPath + L".missing";

            // A consumer of the pipe must see an empty stream, not wait forever, when the file is
            // missing (ReadFile fails) or empty (ReadFile succeeds with 0 bytes). Both cases run at
            // once; all objects live until the scheduler has stopped.
            struct Case {
                const std::wstring*           path = nullptr;
                Memory::SpscVirtualMemoryPipe pipe;
                std::atomic<int>              done{0};
                std::atomic<unsigned long>    readError{ERROR_SUCCESS};
                std::atomic<std::size_t>      consumedSize{1};
            };
            std::array<Case, 2> cases;
            cases[0].path = &missingPath;
            cases[1].path = &emptyPath;

            auto produce = [](Case& c) {
                c.readError.store(AsyncIo::AsyncFile::ReadFile(const_cast<wchar_t*>(c.path->c_str()), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                                               FILE_ATTRIBUTE_NORMAL, &c.pipe));
                c.done.fetch_add(1, std::memory_order_release);
            };
            auto consume = [](Case& c) {
                std::size_t total = c.pipe.GetSize();
                for (;;) {
                    std::size_t size = 4096;
                    if (!c.pipe.RequestReadMemory(size))
                        break;
                    total += size;
                    c.pipe.ReleaseReadMemory(size);
                }
                c.consumedSize.store(total);
                c.done.fetch_add(1, std::memory_order_release);
            };
            auto producer0 = Tasks::Task::Create([&] { produce(cases[0]); });
            auto consumer0 = Tasks::Task::Create([&] { consume(cases[0]); });
            auto producer1 = Tasks::Task::Create([&] { produce(cases[1]); });
            auto consumer1 = Tasks::Task::Create([&] { consume(cases[1]); });
            scheduler.AddTask(&consumer0, TaskPriority::Normal);
            scheduler.AddTask(&producer0, TaskPriority::Normal);
            scheduler.AddTask(&consumer1, TaskPriority::Normal);
            scheduler.AddTask(&producer1, TaskPriority::Normal);

            const bool finished = WaitFor([&] { return cases[0].done.load(std::memory_order_acquire) == 2 && cases[1].done.load(std::memory_order_acquire) == 2; },
                                          std::chrono::seconds(30));
            Assert::IsTrue(finished, L"ReadFile into a pipe hung on a missing or empty file");
            for (auto& c : cases)
                Assert::AreEqual(static_cast<std::size_t>(0), c.consumedSize.load(), L"the consumer did not see an empty stream");
            Assert::AreEqual(static_cast<unsigned long>(ERROR_FILE_NOT_FOUND), cases[0].readError.load(), L"ReadFile did not report the missing file");
            Assert::AreEqual(static_cast<unsigned long>(ERROR_SUCCESS), cases[1].readError.load(), L"ReadFile of an empty file failed");

            AsyncIo::DisableAsyncIo();
            ::DeleteFileW(emptyPath.c_str());

            scheduler.Stop();
            ConvertTaskToThread();
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
