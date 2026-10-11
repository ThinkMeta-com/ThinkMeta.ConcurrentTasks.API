//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// StreamedFileReader: a file read ahead with overlapped reads into one reserved address range, so
// consumers address it like a mapped file while memory stays bounded by the read-ahead (as long as the
// consumers progress evenly).
//
// Preliminary API: everything in this file is subject to change.
//
module;

#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.StreamedFileReader;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {

    /// <summary>How a <see cref="StreamedFileReader"/> reads: granule, how far ahead, how many reads at once.</summary>
    /// <remarks>
    /// <see cref="StreamedFileReader::Open"/> corrects out-of-range values instead of failing: <see cref="segmentSize"/> is
    /// raised to at least 64 KB and rounded up to a multiple of 64 KB, <see cref="readAhead"/> is raised to at least one
    /// segment, <see cref="readsInFlight"/> is clamped to 1..<see cref="StreamedFileReader::MaxReadsInFlight"/>.
    /// </remarks>
    export struct StreamedFileReaderSettings {
        /// <summary>Bytes per read; memory is committed and decommitted in the same unit.</summary>
        std::uint64_t segmentSize = std::uint64_t{4} << 20; // read, commit and decommit granule (multiple of 64 KB)

        /// <summary>How far past the lowest released segment the reader reads without being asked.</summary>
        /// <remarks>
        /// A <see cref="StreamedFileReader::Wait"/> beyond it is still served, and the memory up to that
        /// Wait stays committed until every user has released it; so it bounds memory only while all users
        /// stay within the read-ahead of the slowest one.
        /// </remarks>
        std::uint64_t readAhead = std::uint64_t{256} << 20; // loaded ahead of the oldest byte still in use

        /// <summary>Overlapped reads the reader task keeps in flight.</summary>
        std::uint32_t readsInFlight = 4;                        // at most MaxReadsInFlight

        /// <summary>Open the file with <c>FILE_FLAG_NO_BUFFERING</c>.</summary>
        /// <remarks>The last read is rounded up to whole sectors.</remarks>
        bool unbuffered = false; // FILE_FLAG_NO_BUFFERING (whole sectors are read)
    };

    /// <summary>A file streamed into memory, addressed like a mapped file, with bounded memory use.</summary>
    /// <remarks>
    /// <para>
    /// The file is streamed into one reserved address range, so every byte is addressed by its file position: the reader
    /// task (<see cref="Run"/>) commits and reads segment after segment, at most
    /// <see cref="StreamedFileReaderSettings::readAhead"/> past the oldest byte still in use, or as
    /// far as a user waits for. Users report the bytes they are done with (<see cref="Release"/>); whole segments below the lowest
    /// such position of all users are decommitted. Memory stays bounded by the read-ahead, not by the file size,
    /// only while the users progress evenly: committed is everything from the lowest release of all users up to
    /// the read-ahead past it or the farthest Wait, whichever is higher. A fast user's Wait far ahead is
    /// served while a slow user holds the front, and every user slot starts at <c>0</c>, so nothing is
    /// decommitted until each user has released. Usage rule: keep the users within the read-ahead of each
    /// other and let every slot opened release (<c>~0</c> when a user is done), or memory grows up to the file size.
    /// <see cref="Wait"/> and Release may be called from tasks and threads; Release of one user slot from one caller at a time.
    /// </para>
    /// <para>
    /// Usage: <see cref="Open"/>, then schedule the object itself (it is the reader task) once; consumers call
    /// Wait(end) before touching bytes below end and Release when done with them. Needs
    /// <see cref="EnableAsyncIo"/>. The file is read in rising offset order; loaded only grows. Wait suspends
    /// the calling task (an OS thread must have called <see cref="ConvertThreadToTask"/>). Errors: Open returns
    /// a Win32 error code; a read error is kept in <see cref="ReadError"/> and does not stop the stream - Wait
    /// still returns, the affected bytes read as zero, so consumers must check ReadError. Destroy
    /// the object only after the reader task has ended (Run has returned).
    /// </para>
    /// </remarks>
    /// <seealso cref="StreamedFileReaderSettings"/>
    export class StreamedFileReader : public Tasks::Task
    {
        StreamedFileReader(const StreamedFileReader&)            = delete;
        StreamedFileReader& operator=(const StreamedFileReader&) = delete;

    public:
        /// <summary>Upper bound of <see cref="StreamedFileReaderSettings::readsInFlight"/>.</summary>
        static constexpr std::uint32_t MaxReadsInFlight = 16;

        /// <summary>Number of <see cref="Release"/> slots (users).</summary>
        static constexpr std::uint32_t MaxUsers = 8;

        /// <summary>Sector size the last unbuffered read is rounded up to.</summary>
        static constexpr std::uint64_t SectorAlign = 4096;

        /// <summary>Creates a reader with no file.</summary>
        /// <remarks>Call <see cref="Open"/> before scheduling it.</remarks>
        StreamedFileReader() noexcept = default;

        /// <summary>Releases the address range.</summary>
        /// <remarks>The reader task must have ended (no read in flight).</remarks>
        ~StreamedFileReader() noexcept
        {
            if (base_ != nullptr)
                VirtualFree(base_, 0, MEM_RELEASE);
        }

        /// <summary>Opens the file and reserves its address range.</summary>
        /// <param name="path">Path of an existing file; must not be <c>nullptr</c>; need not stay valid after the call.</param>
        /// <param name="settings">
        /// Tuning of the reader; out-of-range values are corrected (see <see cref="StreamedFileReaderSettings"/>).
        /// </param>
        /// <param name="users">How many <see cref="Release"/> slots are in use.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the file is open and its range reserved; otherwise the Win32 error code (see
        /// remarks).
        /// </returns>
        /// <remarks>
        /// Call once, before the reader task is scheduled. Opens <c>OPEN_EXISTING</c>, <c>GENERIC_READ</c>,
        /// <c>FILE_SHARE_READ</c>; needs <see cref="EnableAsyncIo"/>. Does not suspend. users <c>0</c> counts as 1, more than
        /// <see cref="MaxUsers"/> as MaxUsers; slots in use start at <c>0</c>, so nothing is decommitted until each of
        /// them has released. Returns the Win32 error of <c>CreateFileW</c>/association, <c>GetFileSizeEx</c> or
        /// the reservation of the file size (rounded up to a segment, plus one).
        /// </remarks>
        /// <seealso cref="StreamedFileReaderSettings"/>
        Windows::ErrorCode Open(const wchar_t* path, const StreamedFileReaderSettings& settings = {}, std::uint32_t users = 1) noexcept
        {
            settings_ = settings;
            if (settings_.readsInFlight == 0 || settings_.readsInFlight > MaxReadsInFlight)
                settings_.readsInFlight = settings_.readsInFlight == 0 ? 1 : MaxReadsInFlight;
            if (settings_.segmentSize < 65536)
                settings_.segmentSize = 65536;
            settings_.segmentSize = RoundUp(settings_.segmentSize, 65536);
            if (settings_.readAhead < settings_.segmentSize)
                settings_.readAhead = settings_.segmentSize;
            for (std::uint32_t i = 0; i < MaxUsers; ++i)
                lows_[i] = i < (users == 0 ? 1 : users) ? 0 : ~std::uint64_t{0};

            auto error = file_.Create(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                      settings_.unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN);
            if (Windows::Error::Failed(error))
                return error;
            AsyncFile::FileSize size = 0;
            error                    = file_.GetSize(size);
            if (Windows::Error::Failed(error))
                return error;
            size_ = size;
            // one spare segment: the last one is committed whole
            base_ = static_cast<std::byte*>(
                VirtualAlloc(nullptr, static_cast<std::size_t>(RoundUp(size_, settings_.segmentSize) + settings_.segmentSize), MEM_RESERVE, PAGE_READWRITE));
            return base_ != nullptr ? Windows::Error::Success : Windows::Error::GetLastError();
        }

        /// <summary>Byte <c>0</c> of the file.</summary>
        /// <returns>The start of the file's address range; <c>nullptr</c> before a successful <see cref="Open"/>.</returns>
        /// <remarks>A byte is valid between <see cref="Wait"/> covering it and its release (<see cref="Release"/>).</remarks>
        const std::byte* Data() const noexcept { return base_; }

        /// <summary>The file size, fixed at <see cref="Open"/>.</summary>
        /// <returns>The file size in bytes.</returns>
        std::uint64_t Size() const noexcept { return size_; }

        /// <summary>Waits until the bytes before <paramref name="end"/> are loaded.</summary>
        /// <param name="end">File offset in bytes up to which the data is needed; clamped to <see cref="Size"/>.</param>
        /// <returns>
        /// How far the file is loaded: at least end, less only when the reader was stopped before (<see cref="Stop"/>).
        /// </returns>
        /// <remarks>
        /// end is clamped to <see cref="Size"/>. Returns at once if the bytes are loaded; otherwise asks the
        /// reader to read up to end (even beyond the read-ahead) and suspends the calling task
        /// until they are. A Wait beyond the read-ahead commits memory past the bound, kept until all users
        /// have released it (see the class remarks). Any number of tasks may wait at once. Bytes are loaded also when their
        /// read failed (they are zero then): check <see cref="ReadError"/>. The reader task must be scheduled.
        /// </remarks>
        /// <seealso cref="Release"/>
        std::uint64_t Wait(std::uint64_t end) noexcept
        {
            if (end > size_)
                end = size_;
            std::uint64_t loaded = loaded_.load(std::memory_order_acquire);
            if (loaded >= end)
                return loaded;
            const auto    t0 = std::chrono::steady_clock::now();
            std::uint64_t r  = requested_.load(std::memory_order_relaxed);
            while (r < end && !requested_.compare_exchange_weak(r, end, std::memory_order_seq_cst)) {
            }
            readerWake_.Set();
            for (;;) {
                const bool finished = finished_.load(std::memory_order_acquire);
                loaded              = loaded_.load(std::memory_order_acquire);
                if (loaded >= end || finished)
                    break;
                loadedEvent_.Wait();
            }
            loadedEvent_.Set(); // other waiters may need the same news
            waited_.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()),
                              std::memory_order_relaxed);
            return loaded;
        }

        /// <summary>Reports that a user is done with the bytes before a position.</summary>
        /// <param name="user">The user slot.</param>
        /// <param name="below">Bytes before it are no longer used; <c>~0</c>: all of them.</param>
        /// <remarks>
        /// Never suspends. Whole segments below the minimum over all slots (and below what is
        /// loaded) are decommitted, which also lets the reader read further ahead. Bytes below
        /// below must not be touched afterwards by that user. below should not decrease for a slot;
        /// user >= <see cref="MaxUsers"/> is ignored. If another caller is decommitting at the same moment, this
        /// call leaves the work to a later Release. A slot that never releases keeps everything from its
        /// position on committed.
        /// </remarks>
        /// <seealso cref="Wait"/>
        void Release(std::uint32_t user, std::uint64_t below) noexcept
        {
            if (user >= MaxUsers)
                return;
            lows_[user].store(below, std::memory_order_release);
            std::uint64_t low = ~std::uint64_t{0};
            for (const auto& l : lows_) {
                const auto v = l.load(std::memory_order_acquire);
                if (v < low)
                    low = v;
            }
            const auto loaded = loaded_.load(std::memory_order_acquire);
            if (low > loaded)
                low = loaded;
            low = low / settings_.segmentSize * settings_.segmentSize;
            // one caller decommits at a time; a caller that finds the lock taken leaves it to the next release
            if (low <= released_.load(std::memory_order_acquire) || releasing_.exchange(true, std::memory_order_acquire))
                return;
            const auto from = released_.load(std::memory_order_relaxed);
            if (low > from) {
                VirtualFree(base_ + from, static_cast<std::size_t>(low - from), MEM_DECOMMIT);
                released_.store(low, std::memory_order_seq_cst);
                readerWake_.Set();
            }
            releasing_.store(false, std::memory_order_release);
        }

        /// <summary>Stops the reader.</summary>
        /// <remarks>
        /// The reader issues no more reads and ends once the reads in flight are done.
        /// Any task or thread; never suspends. Waiters are then released with what was loaded.
        /// The reader also ends on its own at the end of the file.
        /// </remarks>
        /// <seealso cref="Run"/>
        void Stop() noexcept
        {
            stop_.store(true, std::memory_order_seq_cst);
            readerWake_.Set();
        }

        /// <summary>The first read error (<see cref="Windows::Error::Success"/> if none).</summary>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if every read so far succeeded; otherwise the Win32 error code of the first failed
        /// read.
        /// </returns>
        /// <remarks>After an error the pages read as zero.</remarks>
        Windows::ErrorCode ReadError() const noexcept { return error_.load(std::memory_order_relaxed); }

        /// <summary>Seconds all callers of <see cref="Wait"/> waited together.</summary>
        /// <returns>The summed waiting time in seconds (microsecond resolution).</returns>
        double Waited() const noexcept { return static_cast<double>(waited_.load(std::memory_order_relaxed)) / 1e6; }

        /// <summary>Largest committed range (bytes) while reading.</summary>
        /// <returns>The largest committed range in bytes.</returns>
        std::uint64_t PeakWindow() const noexcept { return peakWindow_; }

        /// <summary>The reader task.</summary>
        /// <remarks>
        /// Schedule it once after <see cref="Open"/>; it ends at the end of the file or after <see cref="Stop"/>.
        /// Suspends while it may not read further (read-ahead used up and nobody waiting) and
        /// while it waits for the oldest read. Reads complete in any order on the I/O thread but
        /// are accounted in issue order, so loaded grows contiguously. A read error (or a short
        /// read: <c>ERROR_HANDLE_EOF</c>) is recorded once (<see cref="ReadError"/>); later segments are committed but not read.
        /// Closes the file before it ends.
        /// </remarks>
        void Run() noexcept override
        {
            const std::uint64_t segment   = settings_.segmentSize;
            std::uint64_t       issuedTo  = 0;
            std::uint64_t       loaded    = 0;
            std::uint64_t       issued    = 0;
            std::uint64_t       completed = 0;
            Windows::ErrorCode  error     = Windows::Error::Success;

            while (loaded < size_) {
                const bool stop = stop_.load(std::memory_order_seq_cst);
                if (!stop) {
                    std::uint64_t allowed = released_.load(std::memory_order_seq_cst) + settings_.readAhead;
                    const auto    asked   = requested_.load(std::memory_order_seq_cst);
                    if (asked > allowed)
                        allowed = asked;
                    while (issued - completed < settings_.readsInFlight && issuedTo < size_ && issuedTo < allowed) {
                        const auto slot = static_cast<std::uint32_t>(issued % settings_.readsInFlight);
                        auto&      req  = requests_[slot];
                        bytes_[slot]    = size_ - issuedTo < segment ? size_ - issuedTo : segment;
                        req.Reset();
                        req.SetOffset(issuedTo);
                        Windows::ErrorCode result = VirtualAlloc(base_ + issuedTo, static_cast<std::size_t>(segment), MEM_COMMIT, PAGE_READWRITE) != nullptr
                                                        ? Windows::Error::Success
                                                        : Windows::Error::GetLastError();
                        // unbuffered: whole sectors (the segment is committed whole; the read ends at the end of the file)
                        const std::uint64_t request = settings_.unbuffered ? RoundUp(bytes_[slot], SectorAlign) : bytes_[slot];
                        if (Windows::Error::Succeeded(result))
                            result = Windows::Error::Succeeded(error) ? file_.ReadAsync(base_ + issuedTo, static_cast<DWORD>(request), req) : error;
                        if (Windows::Error::Failed(result) && !Windows::Error::IsPending(result))
                            req.RequestCompleted(result, 0);
                        issuedTo += bytes_[slot];
                        ++issued;
                        const auto window = issuedTo - released_.load(std::memory_order_relaxed);
                        if (window > peakWindow_)
                            peakWindow_ = window;
                    }
                }
                if (completed == issued) {
                    if (stop)
                        break;
                    readerWake_.Wait();
                    continue;
                }
                const auto slot = static_cast<std::uint32_t>(completed % settings_.readsInFlight);
                auto&      req  = requests_[slot];
                req.Wait();
                if (Windows::Error::Succeeded(error) && (Windows::Error::Failed(req.GetError()) || req.GetTransferredBytes() != bytes_[slot])) {
                    error = Windows::Error::Failed(req.GetError()) ? req.GetError() : static_cast<Windows::ErrorCode>(ERROR_HANDLE_EOF);
                    error_.store(error, std::memory_order_relaxed);
                }
                loaded += bytes_[slot];
                ++completed;
                loaded_.store(loaded, std::memory_order_release);
                loadedEvent_.Set();
            }
            file_.Close();
            // a waiter that asked for more than the reader got to (after Stop) is let go with what there is
            loaded_.store(size_ < loaded ? size_ : loaded, std::memory_order_release);
            finished_.store(true, std::memory_order_release);
            loadedEvent_.Set();
        }

    private:
        static std::uint64_t RoundUp(std::uint64_t n, std::uint64_t granule) noexcept { return (n + granule - 1) / granule * granule; }

        StreamedFileReaderSettings settings_;
        AsyncFile                  file_;
        std::byte*                 base_ = nullptr;
        std::uint64_t              size_ = 0;

        std::atomic<std::uint64_t>      loaded_{0};
        std::atomic<std::uint64_t>      requested_{0};
        std::atomic<std::uint64_t>      released_{0};
        std::atomic<bool>               releasing_{false};
        std::atomic<bool>               stop_{false};
        std::atomic<bool>               finished_{false};
        std::atomic<Windows::ErrorCode> error_{Windows::Error::Success};
        std::atomic<std::uint64_t>      waited_{0};
        std::atomic<std::uint64_t>      lows_[MaxUsers] = {};

        Synchronization::AutoResetEvent readerWake_;
        Synchronization::AutoResetEvent loadedEvent_;

        // reader task
        std::uint64_t                peakWindow_ = 0;
        std::uint64_t                bytes_[MaxReadsInFlight] = {};
        WaitableAsyncFileIoRequest   requests_[MaxReadsInFlight];
    };

} // namespace ThinkMeta::ConcurrentTasks::AsyncIo
