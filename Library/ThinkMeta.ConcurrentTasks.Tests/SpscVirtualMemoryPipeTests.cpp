#include "pch.h"
#include "TestHelpers.h"

#define NOMINMAX
#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pipe's end of stream races with its last block: the producer releases the last bytes and
// sets the end right after, while the consumer, short of its wanted size, looks at both. Every
// byte must still arrive, and the last block must be exactly what is left.

namespace ThinkMeta::ConcurrentTasks::Tests {

    TEST_CLASS(SpscVirtualMemoryPipeTests)
    {
    public:
        TEST_METHOD(TestLastBlockArrivesWithEndOfStream)
        {
            ConvertThreadToTask();
            SchedulerInfo   schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            constexpr int Rounds = 3000;

            std::atomic<int>  done{0};
            std::atomic<bool> failed{false};
            std::wstring      failure;
            std::mutex        failureLock;

            // Round r: the stream is r % 97 + 1 blocks of 1000 bytes plus a tail of r % 13 + 1
            // bytes; the consumer always asks for 4096 bytes, so its last requests find less than
            // wanted and decide on the end of stream.
            const auto fail = [&](std::wstring text) {
                std::lock_guard<std::mutex> lock(failureLock);
                if (!failed.exchange(true))
                    failure = std::move(text);
            };

            for (int round = 0; round < Rounds && !failed.load(); ++round) {
                Memory::SpscVirtualMemoryPipe pipe;
                const std::size_t             blocks = static_cast<std::size_t>(round % 97) + 1;
                const std::size_t             tail   = static_cast<std::size_t>(round % 13) + 1;
                const std::size_t             total  = blocks * 1000 + tail;

                ActionTask producer([&pipe, blocks, tail, total]() {
                    pipe.Initialize(total);
                    std::size_t written = 0;
                    for (std::size_t b = 0; b < blocks; ++b) {
                        auto* memory = pipe.RequestWriteMemory(1000);
                        for (std::size_t i = 0; i < 1000; ++i)
                            memory[i] = static_cast<std::byte>((written + i) & 0xFF);
                        written += 1000;
                        pipe.ReleaseWriteMemory(1000);
                    }
                    auto* memory = pipe.RequestWriteMemory(tail);
                    for (std::size_t i = 0; i < tail; ++i)
                        memory[i] = static_cast<std::byte>((written + i) & 0xFF);
                    // the last block and the end, back to back: the window the consumer races with
                    pipe.ReleaseWriteMemory(tail);
                    pipe.SetEndOfStream();
                });

                std::size_t received = 0;
                bool        corrupt  = false;
                ActionTask  consumer([&pipe, &received, &corrupt]() {
                    const std::size_t size = pipe.GetSize();
                    for (;;) {
                        std::size_t want = 4096;
                        const auto* data = pipe.RequestReadMemory(want);
                        if (data == nullptr || want == 0)
                            break;
                        for (std::size_t i = 0; i < want; ++i) {
                            if (data[i] != static_cast<std::byte>((received + i) & 0xFF))
                                corrupt = true;
                        }
                        received += want;
                        pipe.ReleaseReadMemory(want);
                    }
                    if (received != size)
                        corrupt = true;
                });

                Synchronization::TaskGroup group;
                group.AddTask(&producer);
                group.AddTask(&consumer);
                scheduler.AddTask(&consumer, TaskPriority::Normal);
                scheduler.AddTask(&producer, TaskPriority::Normal);
                group.Wait();

                if (received != total)
                    fail(std::format(L"round {}: {} of {} bytes arrived", round, received, total));
                else if (corrupt)
                    fail(std::format(L"round {}: data corrupt", round));
                done.fetch_add(1);
            }

            scheduler.Stop();
            ConvertTaskToThread();

            Assert::IsFalse(failed.load(), failure.c_str());
            Assert::AreEqual(Rounds, done.load(), L"not every round ran");
        }

        // A file whose size is no multiple of the sector size, written and read back through
        // AsyncFile::WriteFile and ReadFile with FILE_FLAG_NO_BUFFERING: the last block is written
        // padded and the file cut to its size, and the last read is rounded up and ended by the file.
        TEST_METHOD(TestUnbufferedWriteAndReadOfOddSize)
        {
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");
            ConvertThreadToTask();
            SchedulerInfo   schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            constexpr std::size_t Block = 65536;
            constexpr std::size_t Size  = 3 * Block + 12345; // three whole blocks and a tail inside a sector
            const auto            Pattern = [](std::size_t i) { return static_cast<std::byte>(i * 13 + 5); };

            wchar_t tempPath[MAX_PATH];
            wchar_t path[MAX_PATH];
            ::GetTempPathW(MAX_PATH, tempPath);
            ::GetTempFileNameW(tempPath, L"CT_Odd", 0, path);

            AsyncIo::BulkIoOptions options;
            options.blockSize = Block;

            // write: a producer task fills the pipe, WriteFile drains it unbuffered
            Memory::SpscVirtualMemoryPipe outPipe;
            Windows::ErrorCode            writeError = Windows::Error::Success;
            ActionTask                    producer([&outPipe, Pattern]() {
                outPipe.Initialize(Size);
                for (std::size_t written = 0; written < Size;) {
                    const std::size_t n      = Size - written < Block ? Size - written : Block;
                    auto*             memory = outPipe.RequestWriteMemory(n);
                    for (std::size_t i = 0; i < n; ++i)
                        memory[i] = Pattern(written + i);
                    written += n;
                    outPipe.ReleaseWriteMemory(n);
                }
                outPipe.SetEndOfStream();
            });
            ActionTask writer([&outPipe, &writeError, &path, &options]() {
                writeError = AsyncIo::AsyncFile::WriteFile(path, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING, &outPipe, options);
            });
            {
                Synchronization::TaskGroup group;
                group.AddTask(&producer);
                group.AddTask(&writer);
                scheduler.AddTask(&writer, TaskPriority::Normal);
                scheduler.AddTask(&producer, TaskPriority::Normal);
                group.Wait();
            }
            Assert::AreEqual(static_cast<unsigned long>(Windows::Error::Success), static_cast<unsigned long>(writeError), L"WriteFile failed");

            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            Assert::IsTrue(::GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) != 0, L"GetFileAttributesEx failed");
            Assert::AreEqual(static_cast<unsigned long long>(Size), (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow,
                             L"the file does not have its exact size");

            // read back: ReadFile fills a pipe unbuffered, a consumer task checks every byte
            Memory::SpscVirtualMemoryPipe inPipe;
            Windows::ErrorCode            readError = Windows::Error::Success;
            std::size_t                   received  = 0;
            std::size_t                   wrong     = 0;
            ActionTask                    reader([&inPipe, &readError, &path, &options]() {
                readError = AsyncIo::AsyncFile::ReadFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, &inPipe, options);
            });
            ActionTask consumer([&inPipe, &received, &wrong, Pattern]() {
                received = 0;
                if (inPipe.GetSize() != Size)
                    ++wrong;
                for (;;) {
                    std::size_t want = Block;
                    const auto* data = inPipe.RequestReadMemory(want);
                    if (data == nullptr || want == 0)
                        break;
                    for (std::size_t i = 0; i < want; ++i) {
                        if (data[i] != Pattern(received + i))
                            ++wrong;
                    }
                    received += want;
                    inPipe.ReleaseReadMemory(want);
                }
            });
            {
                Synchronization::TaskGroup group;
                group.AddTask(&reader);
                group.AddTask(&consumer);
                scheduler.AddTask(&consumer, TaskPriority::Normal);
                scheduler.AddTask(&reader, TaskPriority::Normal);
                group.Wait();
            }
            ::DeleteFileW(path);

            scheduler.Stop();
            ConvertTaskToThread();
            AsyncIo::DisableAsyncIo();

            Assert::AreEqual(static_cast<unsigned long>(Windows::Error::Success), static_cast<unsigned long>(readError), L"ReadFile failed");
            Assert::AreEqual(Size, received, L"not every byte was read back");
            Assert::AreEqual(static_cast<std::size_t>(0), wrong, L"bytes read back differ from what was written");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
