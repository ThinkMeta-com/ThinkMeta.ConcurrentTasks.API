#include "pch.h"
#include "TestHelpers.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.AsyncIo.StreamedFileReader;
import ThinkMeta.ConcurrentTasks.AsyncIo.OrderedFileWriter;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    namespace {

        std::wstring TempPath(const wchar_t* prefix)
        {
            wchar_t dir[MAX_PATH];
            wchar_t file[MAX_PATH];
            ::GetTempPathW(MAX_PATH, dir);
            ::GetTempFileNameW(dir, prefix, 0, file);
            return file;
        }

        std::uint8_t PatternByte(std::uint64_t i) noexcept { return static_cast<std::uint8_t>((i * 2654435761ull) >> 13); }

        void WritePatternFile(const std::wstring& path, std::uint64_t size)
        {
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(h != INVALID_HANDLE_VALUE, L"cannot create the test file");
            std::vector<std::uint8_t> chunk(1 << 20);
            for (std::uint64_t pos = 0; pos < size;) {
                const auto n = static_cast<DWORD>(std::min<std::uint64_t>(chunk.size(), size - pos));
                for (DWORD k = 0; k < n; ++k)
                    chunk[k] = PatternByte(pos + k);
                DWORD written = 0;
                Assert::IsTrue(::WriteFile(h, chunk.data(), n, &written, nullptr) && written == n);
                pos += n;
            }
            ::CloseHandle(h);
        }

        std::vector<std::uint8_t> ReadWholeFile(const std::wstring& path)
        {
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(h != INVALID_HANDLE_VALUE, L"cannot open the output file");
            LARGE_INTEGER size;
            ::GetFileSizeEx(h, &size);
            std::vector<std::uint8_t> data(static_cast<std::size_t>(size.QuadPart));
            DWORD                     read = 0;
            if (!data.empty())
                Assert::IsTrue(::ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size());
            ::CloseHandle(h);
            return data;
        }

        class Rng
        {
        public:
            explicit Rng(std::uint64_t seed) noexcept : state_(seed * 0x9E3779B97F4A7C15ull + 1) {}
            std::uint64_t Next() noexcept
            {
                state_ ^= state_ >> 12;
                state_ ^= state_ << 25;
                state_ ^= state_ >> 27;
                return state_ * 0x2545F4914F6CDD1Dull;
            }
            std::uint32_t Below(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(Next() % n); }

        private:
            std::uint64_t state_;
        };

        /// <summary>Scheduler, async I/O and the calling thread as a task, for one test.</summary>
        class IoScope
        {
        public:
            explicit IoScope(DWORD threads)
            {
                ConvertThreadToTask();
                scheduler_ = std::make_unique<ScopedScheduler>(SchedulerInfo{.numberOfThreads = threads});
                scheduler_->Start();
                Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");
            }
            ~IoScope()
            {
                AsyncIo::DisableAsyncIo();
                scheduler_->Stop();
                scheduler_.reset();
                ConvertTaskToThread();
            }
            ScopedScheduler& Scheduler() noexcept { return *scheduler_; }

        private:
            std::unique_ptr<ScopedScheduler> scheduler_;
        };

        /// <summary>Streams a pattern file, checks every byte and the read-ahead bound; returns an error text or "".</summary>
        std::wstring RunReader(DWORD threads, bool unbuffered, std::uint64_t size)
        {
            const std::wstring path = TempPath(L"ctR");
            WritePatternFile(path, size);
            std::wstring failure;
            {
                IoScope                              io(threads);
                AsyncIo::StreamedFileReader          reader;
                AsyncIo::StreamedFileReaderSettings  settings;
                settings.segmentSize   = 65536;
                settings.readAhead     = 512 * 1024;
                settings.readsInFlight = 3;
                settings.unbuffered    = unbuffered;
                if (Windows::Error::Failed(reader.Open(path.c_str(), settings, 2)))
                    return L"Open failed";

                std::atomic<bool> done{false};
                std::atomic<int>  mismatch{0};
                auto              consumer = Tasks::Task::Create([&] {
                    constexpr std::uint64_t Lag   = 300 * 1024; // user 1 releases this far behind user 0
                    constexpr std::uint64_t Chunk = 7777;
                    const std::byte*        data  = reader.Data();
                    for (std::uint64_t pos = 0; pos < size;) {
                        const std::uint64_t end = std::min(pos + Chunk, size);
                        if (reader.Wait(end) < end) {
                            mismatch.store(-1);
                            break;
                        }
                        for (std::uint64_t i = pos; i < end; ++i)
                            if (static_cast<std::uint8_t>(data[i]) != PatternByte(i)) {
                                mismatch.store(1);
                                break;
                            }
                        reader.Release(0, pos);
                        reader.Release(1, pos > Lag ? pos - Lag : 0);
                        pos = end;
                    }
                    reader.Release(0, ~std::uint64_t{0});
                    reader.Release(1, ~std::uint64_t{0});
                    done.store(true, std::memory_order_release);
                });

                Synchronization::TaskGroup group;
                group.AddTask(&reader);
                io.Scheduler().AddTask(&reader, TaskPriority::Normal);
                io.Scheduler().AddTask(&consumer, TaskPriority::Normal);
                if (!WaitFor([&] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(30)))
                    failure = L"consumer timed out";
                reader.Stop();
                group.Wait();
                if (mismatch.load() == 1)
                    failure = L"byte mismatch";
                else if (mismatch.load() == -1)
                    failure = L"Wait returned too early";
                else if (Windows::Error::Failed(reader.ReadError()))
                    failure = L"read error";
                else if (reader.PeakWindow() > settings.readAhead + 2 * settings.segmentSize)
                    failure = L"read-ahead window exceeded: " + std::to_wstring(reader.PeakWindow());
            }
            ::DeleteFileW(path.c_str());
            return failure;
        }

        /// <summary>
        /// Builds a file from random direct writes, deferred copies, batch ends and patches (also of bytes already on
        /// their way to the file) and compares it with the same bytes built in memory; returns an error text or "".
        /// </summary>
        std::wstring RunWriter(DWORD threads, std::uint32_t copyTasks, bool unbuffered, std::uint64_t seed)
        {
            const std::wstring path = TempPath(L"ctW");
            std::wstring       failure;

            std::vector<std::uint8_t> source(3 << 20);
            for (std::size_t i = 0; i < source.size(); ++i)
                source[i] = PatternByte(i + seed * 7919);
            std::vector<std::uint8_t> expected;
            expected.reserve(16 << 20);
            {
                IoScope                            io(threads);
                AsyncIo::OrderedFileWriter         writer;
                AsyncIo::OrderedFileWriterSettings settings;
                settings.segmentSize    = 65536;
                settings.writesInFlight = 3;
                settings.batchRing      = 4;
                settings.jobRing        = 64;
                settings.unbuffered     = unbuffered;

                std::vector<std::unique_ptr<AsyncIo::OrderedFileWriter::CopyTask>> copies;
                for (std::uint32_t i = 0; i < copyTasks; ++i)
                    copies.push_back(std::make_unique<AsyncIo::OrderedFileWriter::CopyTask>(writer));
                Synchronization::TaskGroup group;
                group.AddTask(&writer.Writer());
                for (auto& c : copies)
                    group.AddTask(c.get());
                io.Scheduler().AddTask(&writer.Writer(), TaskPriority::Normal);
                for (auto& c : copies)
                    io.Scheduler().AddTask(c.get(), TaskPriority::Normal);

                std::atomic<bool> done{false};
                std::atomic<int>  problem{0};
                auto              producer = Tasks::Task::Create([&] {
                    if (Windows::Error::Failed(writer.Open(path.c_str(), 32 << 20, settings))) {
                        problem.store(1);
                        writer.Close();
                        done.store(true, std::memory_order_release);
                        return;
                    }
                    Rng                                                   rng(seed);
                    std::vector<std::pair<std::uint64_t, std::uint64_t>> direct; // ranges written directly (patchable)
                    const std::size_t                                    target = (6 << 20) + rng.Below(1 << 20);
                    while (expected.size() < target) {
                        const std::uint32_t op = rng.Below(100);
                        if (op < 30) {
                            const std::uint32_t n = 1 + rng.Below(48);
                            std::uint8_t        head[48];
                            for (std::uint32_t k = 0; k < n; ++k)
                                head[k] = static_cast<std::uint8_t>(rng.Next());
                            direct.emplace_back(expected.size(), n);
                            writer.Write(head, n);
                            expected.insert(expected.end(), head, head + n);
                        } else if (op < 85) {
                            const std::uint32_t n   = 1 + rng.Below(40000);
                            const std::uint32_t off = rng.Below(static_cast<std::uint32_t>(source.size() - n));
                            writer.WriteDeferred(source.data() + off, n);
                            expected.insert(expected.end(), source.begin() + off, source.begin() + off + n);
                        } else if (op < 95) {
                            writer.EndBatch();
                            const std::byte* low = writer.LowestPendingSource();
                            const auto*      s   = reinterpret_cast<const std::byte*>(source.data());
                            if (low != nullptr && (low < s || low >= s + source.size()))
                                problem.store(2);
                        } else if (!direct.empty()) {
                            // patch a directly written range, early (often written to the file already) or late
                            const auto&         r = direct[rng.Below(2) == 0 ? rng.Below(static_cast<std::uint32_t>(std::min<std::size_t>(direct.size(), 8)))
                                                                             : rng.Below(static_cast<std::uint32_t>(direct.size()))];
                            const std::uint64_t n = 1 + rng.Below(static_cast<std::uint32_t>(r.second));
                            std::uint8_t        patch[48];
                            for (std::uint64_t k = 0; k < n; ++k)
                                patch[k] = static_cast<std::uint8_t>(rng.Next());
                            writer.WriteAt(r.first, patch, static_cast<std::size_t>(n));
                            std::memcpy(expected.data() + r.first, patch, static_cast<std::size_t>(n));
                        }
                    }
                    if (!writer.Close())
                        problem.store(3);
                    done.store(true, std::memory_order_release);
                });
                io.Scheduler().AddTask(&producer, TaskPriority::Normal);
                if (!WaitFor([&] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(60)))
                    failure = L"producer timed out";
                writer.StopCopyTasks(copyTasks);
                group.Wait();
                if (problem.load() == 1)
                    failure = L"Open failed";
                else if (problem.load() == 2)
                    failure = L"LowestPendingSource outside the source";
                else if (problem.load() == 3)
                    failure = L"Close failed";
                else if (writer.Stats().bytes != expected.size())
                    failure = L"size differs";
            }
            if (failure.empty()) {
                const auto actual = ReadWholeFile(path);
                if (actual.size() != expected.size())
                    failure = L"file size " + std::to_wstring(actual.size()) + L" instead of " + std::to_wstring(expected.size());
                else if (actual != expected) {
                    const auto at = std::mismatch(actual.begin(), actual.end(), expected.begin()).first - actual.begin();
                    failure       = L"content differs at " + std::to_wstring(at);
                }
            }
            ::DeleteFileW(path.c_str());
            return failure;
        }

    } // namespace

    TEST_CLASS (StreamedFileIoTests) {
    public:
        TEST_METHOD (ReaderStreamsEveryByteWithinTheWindow)
        {
            for (DWORD threads : GetTestThreadCounts())
                for (bool unbuffered : {false, true}) {
                    const auto failure = RunReader(threads, unbuffered, (6 << 20) + 12345);
                    Assert::IsTrue(failure.empty(), (failure + L" (threads " + std::to_wstring(threads) + L", unbuffered " + std::to_wstring(unbuffered) + L")").c_str());
                }
        }

        TEST_METHOD (ReaderSmallerThanOneSegment)
        {
            Assert::IsTrue(RunReader(2, false, 1000).empty());
            Assert::IsTrue(RunReader(2, true, 1000).empty());
        }

        TEST_METHOD (ReaderStopReleasesWaiter)
        {
            const std::wstring path = TempPath(L"ctR");
            WritePatternFile(path, 3 << 20);
            {
                IoScope                     io(2);
                AsyncIo::StreamedFileReader reader;
                Assert::IsTrue(Windows::Error::Succeeded(reader.Open(path.c_str())));
                reader.Stop();
                std::atomic<std::uint64_t> got{~std::uint64_t{0}};
                auto                       waiter = Tasks::Task::Create([&] { got.store(reader.Wait(reader.Size())); });
                Synchronization::TaskGroup group;
                group.AddTask(&reader);
                io.Scheduler().AddTask(&reader, TaskPriority::Normal);
                io.Scheduler().AddTask(&waiter, TaskPriority::Normal);
                Assert::IsTrue(WaitFor([&] { return got.load() != ~std::uint64_t{0}; }, std::chrono::seconds(10)), L"waiter hangs after Stop");
                group.Wait();
                Assert::IsTrue(got.load() < reader.Size());
            }
            ::DeleteFileW(path.c_str());
        }

        TEST_METHOD (WriterBuildsTheExactFile)
        {
            std::uint64_t seed = 1;
            for (DWORD threads : GetTestThreadCounts())
                for (std::uint32_t copies : {1u, 4u})
                    for (bool unbuffered : {false, true}) {
                        const auto failure = RunWriter(threads, copies, unbuffered, seed++);
                        Assert::IsTrue(failure.empty(), (failure + L" (threads " + std::to_wstring(threads) + L", copy tasks " + std::to_wstring(copies) +
                                                         L", unbuffered " + std::to_wstring(unbuffered) + L")")
                                                            .c_str());
                    }
        }

        TEST_METHOD (WriterCloseWithoutOpenEndsTheWriterTask)
        {
            IoScope                    io(2);
            AsyncIo::OrderedFileWriter writer;
            Synchronization::TaskGroup group;
            group.AddTask(&writer.Writer());
            io.Scheduler().AddTask(&writer.Writer(), TaskPriority::Normal);
            std::atomic<bool> closed{false};
            auto              closer = Tasks::Task::Create([&] {
                writer.Close();
                closed.store(true);
            });
            io.Scheduler().AddTask(&closer, TaskPriority::Normal);
            Assert::IsTrue(WaitFor([&] { return closed.load(); }, std::chrono::seconds(10)), L"Close without Open hangs");
            group.Wait();
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Tests
