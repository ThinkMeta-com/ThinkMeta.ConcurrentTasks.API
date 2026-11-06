#include "pch.h"
#include "TestHelpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFileIoRequest;
import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;
import ThinkMeta.ConcurrentTasks.Platform.Error;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    // How the tests open their files: the default reads an existing file that others may read too;
    // the shared ones let others write meanwhile (the shrinking test cuts a file while it is read).
    constexpr Platform::FileOptions ReadExisting{};
    constexpr Platform::FileOptions ReadSharedWithWriters{.shareWrite = true};
    constexpr Platform::FileOptions OpenReadWrite{.access = Platform::FileAccess::ReadWrite, .shareWrite = true};
    constexpr Platform::FileOptions CreateReadWrite{.access     = Platform::FileAccess::ReadWrite,
                                                    .creation   = Platform::FileCreation::CreateAlways,
                                                    .shareWrite = true};
    constexpr Platform::FileOptions CreateWrite{.access = Platform::FileAccess::Write, .creation = Platform::FileCreation::CreateAlways, .shareWrite = true};
    constexpr Platform::FileOptions CreateWriteAlone{.access    = Platform::FileAccess::Write,
                                                     .creation  = Platform::FileCreation::CreateAlways,
                                                     .shareRead = false};

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

    // A sink that shrinks the file being read when the given block is requested, i.e. after the
    // reader took the size and before it reads that block.
    class ShrinkingWriteSequentialMemory : public VectorWriteSequentialMemory
    {
    public:
        ShrinkingWriteSequentialMemory(const std::filesystem::path& path, std::size_t shrinkAtBlock, std::uint64_t newSize)
            : path_(path), shrinkAtBlock_(shrinkAtBlock), newSize_(newSize)
        {
        }

        std::byte* RequestWriteMemory(std::size_t size) noexcept override
        {
            if (requests_++ == shrinkAtBlock_)
                shrunk_ = ResizeTestFile(path_, newSize_);
            return VectorWriteSequentialMemory::RequestWriteMemory(size);
        }

        bool Shrunk() const noexcept { return shrunk_; }

    private:
        std::filesystem::path path_;
        std::size_t   shrinkAtBlock_;
        std::uint64_t newSize_;
        std::size_t   requests_ = 0;
        bool          shrunk_   = false;
    };

    // A file of the given size with byte i = i * 13 + 5, written synchronously.
    static void WritePatternFile(const std::filesystem::path& path, std::size_t size)
    {
        std::vector<std::byte> data(size);
        for (std::size_t i = 0; i < size; ++i)
            data[i] = static_cast<std::byte>(i * 13 + 5);
        Assert::IsTrue(WriteTestFile(path, data.data(), size), L"Failed to write pattern file");
    }

    // Runs body as a task on a scheduler with the given number of threads and waits for it on the
    // test thread. The task outlives the scheduler, and so its OnTaskExited. The body only records
    // what it saw; the test thread asserts, since a failed assertion must not leave a task.
    template <typename Body>
    static bool RunAsTask(DWORD numberOfThreads, std::chrono::milliseconds timeout, Body&& body)
    {
        std::atomic<bool> done{false};
        ActionTask        task([&] {
            body();
            done.store(true, std::memory_order_release);
        });

        {
            ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = numberOfThreads});
            scheduler.Start();
            scheduler.AddTask(&task, TaskPriority::Normal);
            WaitFor([&done] { return done.load(std::memory_order_acquire); }, timeout);
            scheduler.Stop();
        }
        return done.load(std::memory_order_acquire);
    }

    TEST_CLASS(AsyncIoTests)
    {
    public:
        TEST_METHOD(TestAsyncFileWriteAndRead)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto tempFilePath = TempFilePath("CT_AsyncIo");
            const std::string writeData = "Async I/O Test Payload for ConcurrentTasks!";
            const std::uint32_t writeSize = static_cast<std::uint32_t>(writeData.size());

            Platform::ErrorCode createError = 0, writeError = 0, readError = 0;
            std::uint32_t bytesWritten = 0, bytesRead = 0;
            bool payloadMatches = false;

            const bool finished = RunAsTask(2, std::chrono::seconds(10), [&] {
                AsyncIo::AsyncFile file;

                createError = file.Create(tempFilePath.c_str(), CreateReadWrite);
                if (Platform::Error::Failed(createError))
                    return;

                // Write Async
                AsyncIo::WaitableAsyncFileIoRequest writeRequest;
                writeRequest.SetOffset(0);
                writeError = file.WriteAsync(writeData.data(), writeSize, writeRequest);
                if (!Platform::Error::Succeeded(writeError) && !Platform::Error::IsPending(writeError))
                    return;
                writeRequest.Wait();
                bytesWritten = writeRequest.GetTransferredBytes();

                // Read Async
                std::vector<char> readBuffer(writeSize, 0);
                AsyncIo::WaitableAsyncFileIoRequest readRequest;
                readRequest.SetOffset(0);
                readError = file.ReadAsync(readBuffer.data(), writeSize, readRequest);
                if (!Platform::Error::Succeeded(readError) && !Platform::Error::IsPending(readError))
                    return;
                readRequest.Wait();
                bytesRead = readRequest.GetTransferredBytes();

                std::string readData(readBuffer.begin(), readBuffer.end());
                payloadMatches = readData == writeData;

                file.Close();
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(tempFilePath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile write and read test timed out!");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"Failed to create async temp file");
            Assert::IsTrue(Platform::Error::Succeeded(writeError) || Platform::Error::IsPending(writeError), L"WriteAsync failed");
            Assert::AreEqual(writeSize, bytesWritten, L"Bytes written match expected size");
            Assert::IsTrue(Platform::Error::Succeeded(readError) || Platform::Error::IsPending(readError), L"ReadAsync failed");
            Assert::AreEqual(writeSize, bytesRead, L"Bytes read match expected size");
            Assert::IsTrue(payloadMatches, L"Read payload matches written payload");
        }

        TEST_METHOD(TestAsyncFileConcurrentOffsetOperations)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            constexpr std::size_t NumSegments = 16;
            constexpr std::size_t SegmentSize = 64 * 1024 * 1024; // 64 MB per segment = 1 GB total scattered offset operations

            // The tasks outlive the scheduler, and so its OnTaskExited.
            Synchronization::TaskGroup taskGroup;
            std::vector<std::unique_ptr<ActionTask>> tasks;
            tasks.reserve(NumSegments);
            std::atomic<std::size_t> completedSegments{0};

            SchedulerInfo schedulerInfo = {.numberOfThreads = 4};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto tempFilePath = TempFilePath("CT_AsyncIo");

            // Prepare test file space for 1 GB
            {
                AsyncIo::AsyncFile initFile;
                initFile.Create(tempFilePath.c_str(), CreateWrite);
                std::vector<char> initChunk(SegmentSize, 0);
                for (std::size_t i = 0; i < NumSegments; ++i) {
                    std::uint32_t written = 0;
                    initFile.Write(initChunk.data(), static_cast<DWORD>(SegmentSize), written, static_cast<AsyncIo::AsyncFile::FileSize>(i * SegmentSize));
                }
                initFile.Close();
            }

            AsyncIo::AsyncFile sharedFile;
            auto createErr = sharedFile.Create(tempFilePath.c_str(), OpenReadWrite);
            Assert::IsTrue(Platform::Error::Succeeded(createErr), L"Failed to open shared file for async offset ops");

            for (std::size_t i = 0; i < NumSegments; ++i) {
                std::uint64_t offset = i * SegmentSize;
                char fillChar = static_cast<char>('A' + i);

                tasks.push_back(std::make_unique<ActionTask>([&sharedFile, offset, fillChar, &completedSegments]() {
                    std::vector<char> writeBuf(SegmentSize, fillChar);
                    AsyncIo::WaitableAsyncFileIoRequest writeReq;
                    writeReq.SetOffset(offset);

                    auto err = sharedFile.WriteAsync(writeBuf.data(), static_cast<DWORD>(SegmentSize), writeReq);
                    if (Platform::Error::Succeeded(err) || Platform::Error::IsPending(err)) {
                        writeReq.Wait();
                    }

                    std::vector<char> readBuf(SegmentSize, 0);
                    AsyncIo::WaitableAsyncFileIoRequest readReq;
                    readReq.SetOffset(offset);

                    err = sharedFile.ReadAsync(readBuf.data(), static_cast<DWORD>(SegmentSize), readReq);
                    if (Platform::Error::Succeeded(err) || Platform::Error::IsPending(err)) {
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

            sharedFile.Close();
            scheduler.Stop();
            AsyncIo::DisableAsyncIo();
            DeleteTestFile(tempFilePath);
            ConvertTaskToThread();

            Assert::AreEqual(NumSegments, completedSegments.load(std::memory_order_acquire), L"All 16 concurrent 64MB scattered offset segments completed successfully");
        }

        TEST_METHOD(TestAsyncFileSyncWrappersAndGetSize)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto tempFilePath = TempFilePath("CT_AsyncIo");
            const std::string writeData = "Synchronous wrapper payload!";
            const std::uint32_t writeSize = static_cast<std::uint32_t>(writeData.size());

            Platform::ErrorCode createError = 0, writeError = 0, sizeError = 0, readError = 0;
            std::uint32_t bytesWritten = 0, bytesRead = 0;
            AsyncIo::AsyncFile::FileSize size = 0;
            bool payloadMatches = false;

            const bool finished = RunAsTask(2, std::chrono::seconds(10), [&] {
                AsyncIo::AsyncFile file;

                createError = file.Create(tempFilePath.c_str(), CreateReadWrite);
                if (Platform::Error::Failed(createError))
                    return;

                // Position-based synchronous Write wrapper (internally uses _s request).
                writeError = file.Write(writeData.data(), writeSize, bytesWritten, 0);
                if (Platform::Error::Failed(writeError))
                    return;

                // GetSize must reflect what was written.
                sizeError = file.GetSize(size);
                if (Platform::Error::Failed(sizeError))
                    return;

                // Position-based synchronous Read wrapper.
                std::vector<char> readBuffer(writeSize, 0);
                readError = file.Read(readBuffer.data(), writeSize, bytesRead, 0);
                if (Platform::Error::Failed(readError))
                    return;

                std::string readData(readBuffer.begin(), readBuffer.end());
                payloadMatches = readData == writeData;

                file.Close();
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(tempFilePath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile sync wrappers test timed out!");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"Failed to create async temp file");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"Sync Write failed");
            Assert::AreEqual(writeSize, bytesWritten, L"Sync Write transferred byte count mismatch");
            Assert::IsTrue(Platform::Error::Succeeded(sizeError), L"GetSize failed");
            Assert::AreEqual(static_cast<AsyncIo::AsyncFile::FileSize>(writeSize), size, L"GetSize does not match written size");
            Assert::IsTrue(Platform::Error::Succeeded(readError), L"Sync Read failed");
            Assert::AreEqual(writeSize, bytesRead, L"Sync Read transferred byte count mismatch");
            Assert::IsTrue(payloadMatches, L"Sync read payload mismatch");
        }

        TEST_METHOD(TestAsyncFileSingleWaiterAndRequestReuse)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto tempFilePath = TempFilePath("CT_AsyncIo");
            constexpr std::uint32_t BlockSize = 32;
            constexpr std::size_t NumBlocks = 4;

            // the values of the first block that failed, or of the last one
            Platform::ErrorCode createError = 0, writeStartError = 0, writeError = 0, readStartError = 0, readError = 0;
            std::uint32_t writtenBytes = 0, readBytes = 0;
            bool blocksMatch = false;

            const bool finished = RunAsTask(2, std::chrono::seconds(10), [&] {
                AsyncIo::AsyncFile file;

                createError = file.Create(tempFilePath.c_str(), CreateReadWrite);
                if (Platform::Error::Failed(createError))
                    return;

                // Reuse a single single-waiter request object across multiple writes.
                AsyncIo::WaitableAsyncFileIoRequest_s writeRequest;
                for (std::size_t i = 0; i < NumBlocks; ++i) {
                    std::vector<char> block(BlockSize, static_cast<char>('0' + i));

                    writeRequest.Reset();
                    writeRequest.SetOffset(i * BlockSize);
                    writeStartError = file.WriteAsync(block.data(), BlockSize, writeRequest);
                    if (!Platform::Error::Succeeded(writeStartError) && !Platform::Error::IsPending(writeStartError))
                        return;

                    writeRequest.Wait();
                    writeError   = writeRequest.GetError();
                    writtenBytes = writeRequest.GetTransferredBytes();
                    if (Platform::Error::Failed(writeError) || writtenBytes != BlockSize)
                        return;
                }

                // Reuse a single single-waiter request object across multiple reads.
                AsyncIo::WaitableAsyncFileIoRequest_s readRequest;
                for (std::size_t i = 0; i < NumBlocks; ++i) {
                    std::vector<char> block(BlockSize, 0);

                    readRequest.Reset();
                    readRequest.SetOffset(i * BlockSize);
                    readStartError = file.ReadAsync(block.data(), BlockSize, readRequest);
                    if (!Platform::Error::Succeeded(readStartError) && !Platform::Error::IsPending(readStartError))
                        return;

                    readRequest.Wait();
                    readError = readRequest.GetError();
                    readBytes = readRequest.GetTransferredBytes();
                    if (Platform::Error::Failed(readError) || readBytes != BlockSize)
                        return;

                    const char expected = static_cast<char>('0' + i);
                    if (!std::all_of(block.begin(), block.end(), [expected](char c) { return c == expected; }))
                        return;
                }
                blocksMatch = true;

                file.Close();
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(tempFilePath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile single-waiter/request-reuse test timed out!");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"Failed to create async temp file");
            Assert::IsTrue(Platform::Error::Succeeded(writeStartError) || Platform::Error::IsPending(writeStartError), L"WriteAsync (reused request) failed");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"Reused write request reported an error");
            Assert::AreEqual(BlockSize, writtenBytes, L"Reused write request byte count mismatch");
            Assert::IsTrue(Platform::Error::Succeeded(readStartError) || Platform::Error::IsPending(readStartError), L"ReadAsync (reused request) failed");
            Assert::IsTrue(Platform::Error::Succeeded(readError), L"Reused read request reported an error");
            Assert::AreEqual(BlockSize, readBytes, L"Reused read request byte count mismatch");
            Assert::IsTrue(blocksMatch, L"Reused read block content mismatch");
        }

        TEST_METHOD(TestAsyncFileEofAndErrorPaths)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto tempFilePath = TempFilePath("CT_AsyncIo");
            const auto missingPath  = TempFilePath("CT_AsyncIo"); // never created
            const std::string writeData = "small";
            const std::uint32_t writeSize = static_cast<std::uint32_t>(writeData.size());

            Platform::ErrorCode missingError = 0, createError = 0, writeError = 0;
            bool missingValid = true;
            std::uint32_t eofBytes = 0, pastEofBytes = 0;

            const bool finished = RunAsTask(2, std::chrono::seconds(10), [&] {
                // Opening a non-existent file with OPEN_EXISTING must fail.
                {
                    AsyncIo::AsyncFile missingFile;
                    missingError = missingFile.Create(missingPath.c_str(), ReadExisting);
                    missingValid = missingFile.IsValid();
                }

                AsyncIo::AsyncFile file;
                createError = file.Create(tempFilePath.c_str(), CreateReadWrite);
                if (Platform::Error::Failed(createError))
                    return;

                std::uint32_t bytesWritten = 0;
                writeError = file.Write(writeData.data(), writeSize, bytesWritten, 0);
                if (Platform::Error::Failed(writeError))
                    return;

                // Reading beyond EOF: request more bytes than the file contains.
                constexpr DWORD ReadSize = 512;
                std::vector<char> readBuffer(ReadSize, 0);
                AsyncIo::WaitableAsyncFileIoRequest readRequest;
                readRequest.SetOffset(0);
                auto err = file.ReadAsync(readBuffer.data(), ReadSize, readRequest);

                if (Platform::Error::Succeeded(err) || Platform::Error::IsPending(err)) {
                    readRequest.Wait();
                    // A read that hits EOF returns only the available bytes (or a handled EOF error).
                    eofBytes = readRequest.GetTransferredBytes();
                }

                // Reading entirely past EOF should transfer zero bytes.
                AsyncIo::WaitableAsyncFileIoRequest pastEofRequest;
                pastEofRequest.SetOffset(4096);
                std::vector<char> pastBuffer(ReadSize, 0);
                err = file.ReadAsync(pastBuffer.data(), ReadSize, pastEofRequest);
                if (Platform::Error::Succeeded(err) || Platform::Error::IsPending(err)) {
                    pastEofRequest.Wait();
                    pastEofBytes = pastEofRequest.GetTransferredBytes();
                }

                file.Close();
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(tempFilePath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile EOF/error paths test timed out!");
            Assert::IsTrue(Platform::Error::Failed(missingError), L"Opening a non-existent file should fail");
            Assert::IsFalse(missingValid, L"Missing file handle should be invalid");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"Failed to create async temp file");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"Sync Write failed");
            Assert::IsTrue(eofBytes <= writeSize, L"EOF read must not report more than what exists");
            Assert::AreEqual(std::uint32_t{0}, pastEofBytes, L"Read entirely past EOF must transfer zero bytes");
        }

        TEST_METHOD(TestAsyncFileReadFileBulk)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto tempFilePath = TempFilePath("CT_AsyncIo");

            // 1 GiB payload (1,073,741,824 bytes) to benchmark NVMe throughput.
            constexpr std::size_t TotalPayloadSize = 1024ULL * 1024ULL * 1024ULL; // 1 GB
            constexpr std::size_t ChunkSize        = 64 * 1024 * 1024;             // 64 MB chunk
            constexpr std::size_t NumChunks        = TotalPayloadSize / ChunkSize;

            Platform::ErrorCode createError = 0, writeError = 0, readError = 0;
            std::uint32_t chunkWritten = 0;
            bool endOfStream = false;
            std::size_t writtenSize = 0, bufferSize = 0;
            bool contentMatches = false;

            const bool finished = RunAsTask(4, std::chrono::seconds(120), [&] {
                // Write 1 GB in 64 MB chunks using the async file API.
                {
                    AsyncIo::AsyncFile writeFile;
                    createError = writeFile.Create(tempFilePath.c_str(), CreateWrite);
                    if (Platform::Error::Failed(createError))
                        return;

                    std::vector<char> chunkBuffer(ChunkSize);
                    for (std::size_t c = 0; c < NumChunks; ++c) {
                        const std::size_t offset = c * ChunkSize;
                        for (std::size_t i = 0; i < ChunkSize; ++i) {
                            chunkBuffer[i] = static_cast<char>((offset + i) * 31 + 7);
                        }

                        chunkWritten = 0;
                        writeError = writeFile.Write(chunkBuffer.data(), static_cast<DWORD>(ChunkSize), chunkWritten, static_cast<AsyncIo::AsyncFile::FileSize>(offset));
                        if (Platform::Error::Failed(writeError) || chunkWritten != static_cast<DWORD>(ChunkSize))
                            return;
                    }

                    writeFile.Close();
                }

                // Read the whole 1 GB file back through the static bulk ReadFile pipeline.
                VectorWriteSequentialMemory sink;
                readError = AsyncIo::AsyncFile::ReadFile(tempFilePath.c_str(), ReadSharedWithWriters, &sink);
                endOfStream = sink.IsEndOfStream();
                writtenSize = sink.GetWrittenSize();

                const auto& buffer = sink.GetBuffer();
                bufferSize = buffer.size();
                if (Platform::Error::Failed(readError) || bufferSize != TotalPayloadSize)
                    return;

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
                contentMatches = match;
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(tempFilePath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"1 GB AsyncFile bulk ReadFile test timed out!");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"Failed to create file for 1 GB bulk write");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"Chunk write failed during 1 GB write");
            Assert::AreEqual(static_cast<std::uint32_t>(ChunkSize), chunkWritten, L"Chunk write byte count mismatch");
            Assert::IsTrue(Platform::Error::Succeeded(readError), L"Bulk ReadFile 1 GB failed");
            Assert::IsTrue(endOfStream, L"Bulk ReadFile 1 GB did not mark end of stream");
            Assert::AreEqual(TotalPayloadSize, writtenSize, L"Bulk ReadFile 1 GB size mismatch");
            Assert::AreEqual(TotalPayloadSize, bufferSize, L"Read buffer size must equal 1 GB");
            Assert::IsTrue(contentMatches, L"1 GB ReadFile payload content mismatch");
        }
        TEST_METHOD(TestAsyncFileBulkOptionsAndOffset)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto sourcePath = TempFilePath("CT_AsyncIo");
            const auto copyPath = TempFilePath("CT_AsyncIo");
            constexpr std::size_t FileSize = 3 * 1024 * 1024 + 123; // not a multiple of any block size
            constexpr std::size_t Offset   = 1000;
            WritePatternFile(sourcePath, FileSize);

            Platform::ErrorCode partError = 0, wholeError = 0, writeError = 0, copyError = 0;
            bool partEndOfStream = false, partMatches = false, copyMatches = false;
            std::size_t partSize = 0, wholeSize = 0, releasedSize = 0;

            const bool finished = RunAsTask(2, std::chrono::seconds(30), [&] {
                // read from an offset with a small shape: 64 KB x 3
                VectorWriteSequentialMemory part;
                partError = AsyncIo::AsyncFile::ReadFile(sourcePath.c_str(), ReadExisting, &part, AsyncIo::BulkIoOptions{64 * 1024, 3, Offset});
                partEndOfStream = part.IsEndOfStream();
                partSize = part.GetWrittenSize();
                if (Platform::Error::Failed(partError) || partSize != FileSize - Offset)
                    return;
                bool match = true;
                for (std::size_t i = 0; i < FileSize - Offset && match; ++i)
                    match = part.GetBuffer()[i] == static_cast<std::byte>((i + Offset) * 13 + 5);
                partMatches = match;

                // read whole with the default shape, write a copy with 128 KB x 2, read the copy back
                VectorWriteSequentialMemory whole;
                wholeError = AsyncIo::AsyncFile::ReadFile(sourcePath.c_str(), ReadExisting, &whole);
                wholeSize = whole.GetWrittenSize();
                if (Platform::Error::Failed(wholeError) || wholeSize != FileSize)
                    return;

                VectorReadSequentialMemory source(whole.GetBuffer());
                writeError = AsyncIo::AsyncFile::WriteFile(copyPath.c_str(), CreateWriteAlone, &source, AsyncIo::BulkIoOptions{128 * 1024, 2});
                releasedSize = source.GetReleasedSize();
                if (Platform::Error::Failed(writeError))
                    return;

                VectorWriteSequentialMemory copy;
                copyError = AsyncIo::AsyncFile::ReadFile(copyPath.c_str(), ReadExisting, &copy);
                copyMatches = copy.GetBuffer() == whole.GetBuffer();
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(sourcePath);
            DeleteTestFile(copyPath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile bulk options test timed out or failed");
            Assert::IsTrue(Platform::Error::Succeeded(partError), L"ReadFile with offset failed");
            Assert::IsTrue(partEndOfStream, L"ReadFile with offset did not mark end of stream");
            Assert::AreEqual(FileSize - Offset, partSize, L"ReadFile with offset size mismatch");
            Assert::IsTrue(partMatches, L"ReadFile with offset content mismatch");
            Assert::IsTrue(Platform::Error::Succeeded(wholeError) && wholeSize == FileSize, L"ReadFile with the default shape failed");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"WriteFile with options failed");
            Assert::AreEqual(FileSize, releasedSize, L"WriteFile did not release every block");
            Assert::IsTrue(Platform::Error::Succeeded(copyError), L"ReadFile of the copy failed");
            Assert::IsTrue(copyMatches, L"WriteFile copy differs from the source");
        }

        TEST_METHOD(TestAsyncFileReadFileOutOfMemoryEndsCleanly)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto path = TempFilePath("CT_AsyncIo");
            constexpr std::size_t FileSize = 4 * 1024 * 1024;
            WritePatternFile(path, FileSize);

            // the memory fails on the fourth block while up to three reads are in flight; ReadFile
            // must wait for them, report the error and end the stream
            FailingWriteSequentialMemory sink(3);
            Platform::ErrorCode err = 0;

            const bool finished = RunAsTask(2, std::chrono::seconds(30), [&] {
                err = AsyncIo::AsyncFile::ReadFile(path.c_str(), ReadExisting, &sink, AsyncIo::BulkIoOptions{256 * 1024, 4});
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(path);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile ReadFile out-of-memory test timed out or failed");
            Assert::AreEqual(static_cast<unsigned long>(Platform::Error::NotEnoughMemory), static_cast<unsigned long>(err), L"ReadFile did not report the failed block");
            Assert::IsTrue(sink.IsEndOfStream(), L"ReadFile did not end the stream after the failure");
            Assert::AreEqual(static_cast<std::size_t>(3 * 256 * 1024), sink.GetRequestedSize(), L"ReadFile requested blocks after the failure");
            Assert::IsTrue(sink.GetReleasedSize() <= sink.GetRequestedSize(), L"ReadFile released more than it read");
        }

        TEST_METHOD(TestAsyncFileReadFileShrunkFileReportsEof)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto path = TempFilePath("CT_AsyncIo");
            static constexpr std::size_t BlockSize = 256 * 1024;
            static constexpr std::size_t FileSize = 2 * BlockSize;
            WritePatternFile(path, FileSize);

            // one read at a time; the file loses half of its last block before that block is read,
            // so the read comes back short: ReadFile must report it, not publish a short stream as complete
            ShrinkingWriteSequentialMemory sink(path, 1, FileSize - BlockSize / 2);
            Platform::ErrorCode err = 0;

            const bool finished = RunAsTask(2, std::chrono::seconds(30), [&] {
                err = AsyncIo::AsyncFile::ReadFile(path.c_str(), ReadSharedWithWriters, &sink, AsyncIo::BulkIoOptions{BlockSize, 1});
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(path);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"AsyncFile ReadFile shrunk-file test timed out or failed");
            Assert::IsTrue(sink.Shrunk(), L"The test could not shrink the file");
            Assert::AreEqual(static_cast<unsigned long>(Platform::Error::EndOfFile), static_cast<unsigned long>(err), L"ReadFile did not report the short read");
            Assert::IsTrue(sink.IsEndOfStream(), L"ReadFile did not end the stream after the short read");
            Assert::AreEqual(BlockSize, sink.GetWrittenSize(), L"ReadFile published the short block");
        }

        TEST_METHOD(TestAsyncFileReadFileIntoPipeMissingAndEmptyFile)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();

            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

            const auto emptyPath   = TempFilePath("CT_AsyncIo");
            const auto missingPath = TempFilePath("CT_AsyncIo"); // never created
            Assert::IsTrue(WriteTestFile(emptyPath, nullptr, 0), L"Failed to create the empty file");

            // A consumer of the pipe must see an empty stream, not wait forever, when the file is
            // missing (ReadFile fails) or empty (ReadFile succeeds with 0 bytes). Both cases run at
            // once; all objects, the tasks too, live until the scheduler has stopped.
            struct Case {
                const std::filesystem::path*  path = nullptr;
                Memory::SpscVirtualMemoryPipe pipe;
                std::atomic<int>              done{0};
                std::atomic<unsigned long>    readError{Platform::Error::Success};
                std::atomic<std::size_t>      consumedSize{1};
            };
            std::array<Case, 2> cases;
            cases[0].path = &missingPath;
            cases[1].path = &emptyPath;

            auto produce = [](Case& c) {
                c.readError.store(AsyncIo::AsyncFile::ReadFile(c.path->c_str(), ReadExisting, &c.pipe));
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

            SchedulerInfo schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            scheduler.AddTask(&consumer0, TaskPriority::Normal);
            scheduler.AddTask(&producer0, TaskPriority::Normal);
            scheduler.AddTask(&consumer1, TaskPriority::Normal);
            scheduler.AddTask(&producer1, TaskPriority::Normal);

            const bool finished = WaitFor([&] { return cases[0].done.load(std::memory_order_acquire) == 2 && cases[1].done.load(std::memory_order_acquire) == 2; },
                                          std::chrono::seconds(30));

            scheduler.Stop();
            AsyncIo::DisableAsyncIo();
            DeleteTestFile(emptyPath);
            ConvertTaskToThread();

            Assert::IsTrue(finished, L"ReadFile into a pipe hung on a missing or empty file");
            for (auto& c : cases)
                Assert::AreEqual(static_cast<std::size_t>(0), c.consumedSize.load(), L"the consumer did not see an empty stream");
            Assert::AreEqual(static_cast<unsigned long>(Platform::Error::FileNotFound), cases[0].readError.load(), L"ReadFile did not report the missing file");
            Assert::AreEqual(static_cast<unsigned long>(Platform::Error::Success), cases[1].readError.load(), L"ReadFile of an empty file failed");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
