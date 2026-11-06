#include "pch.h"
#include "TestHelpers.h"

#if !defined(_WIN32)
#    include <sys/stat.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;
import ThinkMeta.ConcurrentTasks.Platform.File;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// AsyncFile through the platform-neutral file options, the same on Windows (I/O completion port)
// and Linux (io_uring): single reads and writes, the end of the file, and the bulk transfers
// unbuffered with a last block that is no whole sector. The task records what it saw; the test
// thread asserts, since a failed assertion must not leave a task.

namespace ThinkMeta::ConcurrentTasks::Tests {
    namespace {
        constexpr std::size_t SectorAlign = AsyncIo::BulkIoOptions::SectorAlign;

        // sector-aligned storage, as unbuffered transfers need it
        class AlignedBuffer
        {
        public:
            explicit AlignedBuffer(std::size_t size) :
                size_(size), data_(static_cast<std::byte*>(::operator new[](size, std::align_val_t{SectorAlign})))
            {
            }

            ~AlignedBuffer() { ::operator delete[](data_, std::align_val_t{SectorAlign}); }

            AlignedBuffer(const AlignedBuffer&)            = delete;
            AlignedBuffer& operator=(const AlignedBuffer&) = delete;

            std::byte*  Data() const noexcept { return data_; }
            std::size_t Size() const noexcept { return size_; }

        private:
            std::size_t size_;
            std::byte*  data_;
        };

        constexpr std::byte PatternAt(std::size_t i) noexcept { return static_cast<std::byte>(i * 13 + 5); }

        // Hands out an aligned buffer in blocks for AsyncFile::WriteFile.
        class AlignedReadMemory : public Memory::IReadSequentialMemory
        {
        public:
            AlignedReadMemory(const AlignedBuffer& data, std::size_t size) noexcept : data_(data), size_(size) {}

            std::size_t GetSize() const noexcept override { return size_; }

            const std::byte* RequestReadMemory(std::size_t& size) noexcept override
            {
                const std::size_t left = size_ - handedOut_;
                if (left == 0) {
                    size = 0;
                    return nullptr;
                }
                if (size > left)
                    size = left;
                const std::byte* at = data_.Data() + handedOut_;
                handedOut_ += size;
                return at;
            }

            void ReleaseReadMemory(std::size_t size) noexcept override { released_ += size; }

            std::size_t Released() const noexcept { return released_; }

        private:
            const AlignedBuffer& data_;
            std::size_t          size_;
            std::size_t          handedOut_ = 0;
            std::size_t          released_  = 0;
        };

        // Receives AsyncFile::ReadFile into an aligned buffer with room for a rounded-up last block.
        class AlignedWriteMemory : public Memory::IWriteSequentialMemory
        {
        public:
            explicit AlignedWriteMemory(AlignedBuffer& buffer) noexcept : buffer_(buffer) {}

            void Initialize(std::size_t initialSize) noexcept override { initialSize_ = initialSize; }

            std::byte* RequestWriteMemory(std::size_t size) noexcept override
            {
                if (requested_ + size > buffer_.Size())
                    return nullptr;
                std::byte* at = buffer_.Data() + requested_;
                requested_ += size;
                return at;
            }

            void ReleaseWriteMemory(std::size_t size) noexcept override { released_ += size; }

            void SetEndOfStream() noexcept override { endOfStream_ = true; }

            std::size_t InitialSize() const noexcept { return initialSize_; }
            std::size_t Released() const noexcept { return released_; }
            bool        EndOfStream() const noexcept { return endOfStream_; }

        private:
            AlignedBuffer& buffer_;
            std::size_t    initialSize_ = 0;
            std::size_t    requested_   = 0;
            std::size_t    released_    = 0;
            bool           endOfStream_ = false;
        };

        // runs body as a task and waits for it on the test thread
        template <typename Body>
        bool RunAsTask(Body&& body)
        {
            std::atomic<bool> done{false};
            ActionTask        task([&] {
                body();
                done.store(true, std::memory_order_release);
            });

            {
                // the task outlives the scheduler, and so its OnTaskExited
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();
                scheduler.AddTask(&task);
                WaitFor([&done] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(30));
                scheduler.Stop();
            }
            return done.load(std::memory_order_acquire);
        }
    } // namespace

    TEST_CLASS(AsyncFileTests)
    {
    public:
        TEST_METHOD(TestReadWriteAndEndOfFile)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");
            const auto path = TempFilePath("ctAsyncFile");

            using ByteCount = AsyncIo::AsyncFile::ByteCount;
            constexpr ByteCount BlockSize = 100;

            Platform::ErrorCode createError = 0, writeError = 0, readError = 0, tailError = 0, endError = 0;
            ByteCount           written = 0, read = 0, tailRead = 0, endRead = 1;
            bool                contentMatches = false;

            const bool finished = RunAsTask([&] {
                AsyncIo::AsyncFile file;
                createError = file.Create(path.c_str(), {.access = Platform::FileAccess::ReadWrite, .creation = Platform::FileCreation::CreateAlways});
                if (Platform::Error::Failed(createError))
                    return;

                std::byte data[3 * BlockSize];
                for (std::size_t i = 0; i < sizeof(data); ++i)
                    data[i] = PatternAt(i);
                for (std::size_t block = 0; block < 3 && Platform::Error::Succeeded(writeError); ++block) {
                    ByteCount bytes = 0;
                    writeError = file.Write(data + block * BlockSize, BlockSize, bytes, block * BlockSize);
                    written += bytes;
                }

                std::byte back[3 * BlockSize] = {};
                readError      = file.Read(back, sizeof(back), read, 0);
                contentMatches = read == sizeof(back);
                for (std::size_t i = 0; i < read && contentMatches; ++i)
                    contentMatches = back[i] == data[i];

                // a read that starts inside the file and runs past its end is short
                tailError = file.Read(back, BlockSize, tailRead, 3 * BlockSize - 10);
                // one that starts at the end gets the end of the file
                endError = file.Read(back, BlockSize, endRead, 3 * BlockSize);

                file.Close();
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(path);

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"Create failed");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"Write failed");
            Assert::AreEqual(3 * BlockSize, written, L"Write byte count");
            Assert::IsTrue(Platform::Error::Succeeded(readError), L"Read failed");
            Assert::IsTrue(contentMatches, L"Read content differs from what was written");
            Assert::IsTrue(Platform::Error::Succeeded(tailError), L"a read over the end of the file failed");
            Assert::AreEqual(ByteCount{10}, tailRead, L"a read over the end of the file must be short");
            Assert::IsTrue(endError == Platform::Error::EndOfFile, L"a read at the end of the file must report EndOfFile");
            Assert::AreEqual(ByteCount{0}, endRead, L"a failed read reports 0 bytes");
        }

        TEST_METHOD(TestBulkUnbufferedRoundTrip)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");
            const auto path = TempFilePath("ctAsyncFile");

            // not a whole number of sectors: the last block goes through the padded copy, and the
            // file is cut to its exact size
            constexpr std::size_t FileSize = 3 * 64 * 1024 + 1000;
            constexpr std::size_t Rounded  = (FileSize + SectorAlign - 1) / SectorAlign * SectorAlign;
            const AsyncIo::BulkIoOptions bulk{64 * 1024, 2};

            AlignedBuffer source(Rounded);
            for (std::size_t i = 0; i < FileSize; ++i)
                source.Data()[i] = PatternAt(i);
            AlignedBuffer target(Rounded);

            AlignedReadMemory   reader(source, FileSize);
            AlignedWriteMemory  writer(target);
            Platform::ErrorCode writeError = 0, readError = 0, sizeError = 0;
            AsyncIo::AsyncFile::FileSize size = 0;

            const bool finished = RunAsTask([&] {
                writeError = AsyncIo::AsyncFile::WriteFile(
                    path.c_str(), {.access = Platform::FileAccess::Write, .creation = Platform::FileCreation::CreateAlways, .unbuffered = true}, &reader,
                    bulk);

                AsyncIo::AsyncFile file;
                sizeError = file.Create(path.c_str(), {});
                if (Platform::Error::Succeeded(sizeError))
                    sizeError = file.GetSize(size);
                file.Close();

                readError = AsyncIo::AsyncFile::ReadFile(path.c_str(), {.unbuffered = true}, &writer, bulk);
            });

            AsyncIo::DisableAsyncIo();
            DeleteTestFile(path);

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(Platform::Error::Succeeded(writeError), L"WriteFile unbuffered failed");
            Assert::AreEqual(FileSize, reader.Released(), L"WriteFile must release every byte of the data");
            Assert::IsTrue(Platform::Error::Succeeded(sizeError), L"opening or sizing the written file failed");
            Assert::AreEqual(static_cast<unsigned long long>(FileSize), static_cast<unsigned long long>(size), L"the file must be cut to its exact size");
            Assert::IsTrue(Platform::Error::Succeeded(readError), L"ReadFile unbuffered failed");
            Assert::IsTrue(writer.EndOfStream(), L"ReadFile must end the stream");
            Assert::AreEqual(FileSize, writer.InitialSize(), L"ReadFile must announce the file size");
            Assert::AreEqual(FileSize, writer.Released(), L"ReadFile must release every byte of the file");

            bool matches = true;
            for (std::size_t i = 0; i < FileSize && matches; ++i)
                matches = target.Data()[i] == PatternAt(i);
            Assert::IsTrue(matches, L"the file read back differs from what was written");
        }

#if !defined(_WIN32)
        // The timeout path of the waiting forms: a read of an empty FIFO waits for a writer that
        // never comes, so the read is cancelled at its timeout (Linux only: a named pipe on Windows
        // is no file). Opened for reading and writing, the FIFO opens at once.
        TEST_METHOD(TestReadTimesOutOnAnEmptyFifo)
        {
            SkipWithoutAsyncIo();
            const auto path = TempFilePath("ctAsyncFileFifo");
            Assert::AreEqual(0, mkfifo(path.c_str(), 0600), L"mkfifo failed");
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");
            Assert::IsTrue(EnableTimers(), L"EnableTimers failed");

            Platform::ErrorCode createError = 0, readError = 0;
            AsyncIo::AsyncFile::ByteCount read = 1;
            const bool finished = RunAsTask([&] {
                AsyncIo::AsyncFile file;
                createError = file.Create(path.c_str(), {.access = Platform::FileAccess::ReadWrite});
                if (Platform::Error::Failed(createError))
                    return;
                std::byte buffer[16];
                readError = file.Read(buffer, sizeof(buffer), read, 0, Duration::Milliseconds(50));
                file.Close();
            });

            DisableTimers();
            AsyncIo::DisableAsyncIo();
            DeleteTestFile(path);

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(Platform::Error::Succeeded(createError), L"opening the FIFO failed");
            Assert::IsTrue(readError == Platform::Error::TimedOut, L"a read with nothing to read must time out");
            Assert::AreEqual(AsyncIo::AsyncFile::ByteCount{0}, read, L"a timed-out read reports 0 bytes");
        }
#endif
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
