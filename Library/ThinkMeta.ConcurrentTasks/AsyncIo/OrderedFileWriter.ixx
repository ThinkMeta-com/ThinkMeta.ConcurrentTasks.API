//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// OrderedFileWriter: builds an output file front to back in reserved memory, with deferred copies
// done by copy tasks and the finished front written by a writer task with overlapped writes.
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
#include <cstring>
#include <cwchar>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.OrderedFileWriter;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {

    /// <summary>How an <see cref="OrderedFileWriter"/> writes: granule, writes at once, copy queue sizes.</summary>
    /// <remarks>
    /// <see cref="OrderedFileWriter::Open"/> corrects out-of-range values instead of failing: <see cref="segmentSize"/> is
    /// raised to at least 64 KB and rounded up to a multiple of 64 KB, <see cref="writesInFlight"/> is clamped to
    /// 1..<see cref="OrderedFileWriter::MaxWritesInFlight"/>, <see cref="batchRing"/> and <see cref="jobRing"/> are raised
    /// to at least 2.
    /// </remarks>
    export struct OrderedFileWriterSettings {
        /// <summary>Bytes per write; memory is committed and decommitted in the same unit.</summary>
        std::uint64_t segmentSize = std::uint64_t{4} << 20; // write, commit and decommit granule (multiple of 64 KB)

        /// <summary>Overlapped writes the writer task keeps in flight.</summary>
        std::uint32_t writesInFlight = 4;                      // at most MaxWritesInFlight

        /// <summary>Batches submitted to the copy tasks and not yet retired.</summary>
        /// <remarks>The producer waits (and helps) when the ring is full.</remarks>
        std::uint32_t batchRing = 256; // batches handed to the copy tasks, not yet finished

        /// <summary><see cref="OrderedFileWriter::WriteDeferred"/> copies not yet retired.</summary>
        /// <remarks>A batch is split at half of it; the producer waits when the ring is full.</remarks>
        std::uint32_t jobRing = 65536; // copies in those batches

        /// <summary>Open the file with <c>FILE_FLAG_NO_BUFFERING</c>.</summary>
        /// <remarks>The last write is padded to whole sectors and <see cref="OrderedFileWriter::Close"/> sets the exact size.</remarks>
        bool unbuffered = false; // FILE_FLAG_NO_BUFFERING (the size is set at the end)
    };

    /// <summary>Counters of a finished <see cref="OrderedFileWriter"/>.</summary>
    /// <seealso cref="OrderedFileWriter::Stats"/>
    export struct OrderedFileWriterStats {
        /// <summary>Bytes appended (the final file size).</summary>
        std::uint64_t bytes = 0; // file size

        /// <summary>Number of <see cref="OrderedFileWriter::WriteDeferred"/> calls that were recorded.</summary>
        std::uint64_t deferredCopies = 0;

        /// <summary>Bytes those WriteDeferred calls appended.</summary>
        std::uint64_t deferredBytes = 0;

        /// <summary>Seconds the producer waited for a copy batch to finish.</summary>
        /// <remarks>It waits when a ring is full and in <see cref="OrderedFileWriter::Close"/>.</remarks>
        double producerWaited = 0; // seconds the producer waited for copy slots

        /// <summary>Seconds the writer task waited for writes to complete.</summary>
        double writerWaited = 0; // seconds the writer task waited for writes to complete

        /// <summary>Seconds the writer task spent inside the write calls.</summary>
        double writerIssuing = 0; // seconds in the write calls (buffered writes copy into the cache synchronously)
    };

    /// <summary>An output file built front to back in memory, with the copying and writing done by other tasks.</summary>
    /// <remarks>
    /// <para>
    /// The file is built in one reserved address range. One producer (a task or thread) appends bytes with
    /// <see cref="Write"/> and promises copies with <see cref="WriteDeferred"/>; <see cref="EndBatch"/>
    /// hands the promised copies to the copy tasks (<see cref="CopyTask"/>, any number). The writer task
    /// (<see cref="Writer"/>) writes the front
    /// of the range that is complete, segment by segment, and decommits it. <see cref="WriteAt"/> patches earlier bytes: in memory
    /// while they are not on their way to the file yet, else after the last write. A source of WriteDeferred must stay
    /// valid until <see cref="LowestPendingSource"/> no longer covers it.
    /// </para>
    /// <para>
    /// Ordering: the file receives exactly the bytes in append order, at offset = <see cref="Position"/> at the
    /// time of the append, whatever order the copy tasks finish in. The writer only writes the
    /// prefix in which every deferred copy has finished (in whole segments until <see cref="Close"/>, then the
    /// rest), segment by segment in rising offset order, with up to
    /// <see cref="OrderedFileWriterSettings::writesInFlight"/> writes
    /// overlapping. WriteAt patches land either in memory before the write or, if the bytes have
    /// already been issued, are applied after the last write by Close; either way the final file
    /// holds the patched bytes.
    /// </para>
    /// <para>
    /// Usage: <see cref="Open"/>, schedule Writer() once and any number of CopyTask objects (none is fine: the
    /// producer then runs the batches itself when a ring is full and in Close); then append with
    /// Write/WriteDeferred/EndBatch from one producer; then Close; then <see cref="StopCopyTasks"/> with the
    /// number of copy tasks, and wait for all tasks, the writer task included, to end before destroying the
    /// object (Close does not wait for that, see <see cref="Close"/>). Needs
    /// <see cref="EnableAsyncIo"/>. Producer calls are not thread-safe among themselves (one producer, a task
    /// or a thread after <see cref="ConvertThreadToTask"/>), and they may suspend it: WriteDeferred, EndBatch
    /// and LowestPendingSource can wait for or run copy batches, Close waits for the writer task.
    /// </para>
    /// <para>
    /// Errors: Open returns a Win32 error code; Write, WriteDeferred and WriteAt report nothing.
    /// A failed Open or a failed append (address range exhausted, commit or allocation failed,
    /// WriteAt beyond Position) sets <see cref="Failed"/>, after which appends are dropped. A failed or short write makes the writer issue no further writes;
    /// Close returns <c>false</c> for any of these and for a failed size/patch finalization. The file
    /// is not deleted on failure.
    /// </para>
    /// </remarks>
    /// <seealso cref="OrderedFileWriterSettings"/>
    /// <seealso cref="OrderedFileWriterStats"/>
    export class OrderedFileWriter
    {
        OrderedFileWriter(const OrderedFileWriter&)            = delete;
        OrderedFileWriter& operator=(const OrderedFileWriter&) = delete;

    public:
        /// <summary>Upper bound of <see cref="OrderedFileWriterSettings::writesInFlight"/>.</summary>
        static constexpr std::uint32_t MaxWritesInFlight = 16;

        /// <summary>Sector size the last unbuffered write is padded to.</summary>
        static constexpr std::uint64_t SectorAlign = 4096;

        /// <summary>A copy task: runs batches until <see cref="StopCopyTasks"/>.</summary>
        /// <remarks>
        /// Schedule any number of them after <see cref="Open"/>; each suspends on a semaphore while there is no
        /// batch. The writer must outlive them: they end only after StopCopyTasks released them.
        /// </remarks>
        /// <seealso cref="EndBatch"/>
        class CopyTask final : public Tasks::Task
        {
        public:
            /// <summary>Creates a copy task for <paramref name="writer"/>.</summary>
            /// <param name="writer">The writer whose batches it runs; borrowed, must outlive the task.</param>
            explicit CopyTask(OrderedFileWriter& writer) noexcept : writer_(writer) {}

            /// <summary>Runs submitted batches.</summary>
            /// <remarks>Ends once <see cref="OrderedFileWriter::StopCopyTasks"/> has released it and no batch is left.</remarks>
            void Run() noexcept override
            {
                for (;;) {
                    if (writer_.TryRunBatch())
                        continue;
                    if (writer_.stopCopies_.load(std::memory_order_acquire))
                        break;
                    writer_.work_.Wait();
                }
            }

        private:
            OrderedFileWriter& writer_;
        };

        /// <summary>Creates a closed writer; nothing is allocated until <see cref="Open"/>.</summary>
        OrderedFileWriter() noexcept : writerTask_(*this) {}

        /// <summary>Frees the memory and the patches.</summary>
        /// <remarks>
        /// <see cref="Close"/> must have returned, and all copy tasks and the writer task (<see cref="Writer"/>) must
        /// have ended: nothing is waited for here.
        /// </remarks>
        ~OrderedFileWriter() noexcept
        {
            if (base_ != nullptr)
                VirtualFree(base_, 0, MEM_RELEASE);
            if (jobs_ != nullptr)
                HeapFree(GetProcessHeap(), 0, jobs_);
            if (batches_ != nullptr)
                HeapFree(GetProcessHeap(), 0, batches_);
            while (patches_ != nullptr) {
                Patch* next = patches_->next;
                HeapFree(GetProcessHeap(), 0, patches_);
                patches_ = next;
            }
            if (path_ != nullptr)
                HeapFree(GetProcessHeap(), 0, path_);
        }

        /// <summary>The writer task.</summary>
        /// <returns>The writer task, owned by this writer; schedule it, do not destroy it.</returns>
        /// <remarks>
        /// Schedule it once (before or after <see cref="Open"/>); it ends with <see cref="Close"/>.
        /// It must be scheduled: Close waits until it has completed its last write. It suspends while there is
        /// nothing to write and while it waits for its writes.
        /// It may still be finishing when Close returns: to know when it has ended (before destroying the
        /// writer), count it into a <see cref="Synchronization::TaskGroup"/> or give it a notify callback
        /// (<see cref="Tasks::Task::SetNotifyCallback"/>) before scheduling it.
        /// </remarks>
        /// <seealso cref="CopyTask"/>
        Tasks::Task& Writer() noexcept { return writerTask_; }

        /// <summary>Creates the file.</summary>
        /// <param name="path">Path of the file; must not be <c>nullptr</c>; copied, need not stay valid.</param>
        /// <param name="bound">The largest size the file may reach (only address space is reserved).</param>
        /// <param name="settings">
        /// Tuning of the writer; out-of-range values are corrected (see <see cref="OrderedFileWriterSettings"/>).
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the file is open; otherwise the Win32 error code (see remarks) and the writer is
        /// marked failed.
        /// </returns>
        /// <remarks>
        /// Call once, before any append, from the producer. Creates or truncates <paramref name="path"/>
        /// (<c>CREATE_ALWAYS</c>, <c>GENERIC_WRITE</c>, <c>FILE_SHARE_READ</c>) and needs <see cref="EnableAsyncIo"/>. Does not
        /// suspend. Returns a Win32 error code: <c>ERROR_NOT_ENOUGH_MEMORY</c> for the bookkeeping, the
        /// <c>CreateFileW</c>/association error, or the error of reserving bound bytes (rounded up to a
        /// segment, plus one) of address space. On any error the writer is marked failed
        /// (<see cref="Failed"/> is <c>true</c>): appends are dropped, <see cref="Close"/> returns <c>false</c>, and the
        /// destructor frees what was allocated.
        /// </remarks>
        /// <seealso cref="OrderedFileWriterSettings"/>
        /// <seealso cref="Close"/>
        Windows::ErrorCode Open(const wchar_t* path, std::uint64_t bound, const OrderedFileWriterSettings& settings = {}) noexcept
        {
            settings_ = settings;
            if (settings_.writesInFlight == 0 || settings_.writesInFlight > MaxWritesInFlight)
                settings_.writesInFlight = settings_.writesInFlight == 0 ? 1 : MaxWritesInFlight;
            if (settings_.segmentSize < 65536)
                settings_.segmentSize = 65536;
            settings_.segmentSize = RoundUp(settings_.segmentSize, 65536);
            if (settings_.batchRing < 2)
                settings_.batchRing = 2;
            if (settings_.jobRing < 2)
                settings_.jobRing = 2;

            const auto length = wcslen(path) + 1;
            path_             = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, length * sizeof(wchar_t)));
            jobs_             = static_cast<Job*>(HeapAlloc(GetProcessHeap(), 0, settings_.jobRing * sizeof(Job)));
            batches_          = static_cast<Batch*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, settings_.batchRing * sizeof(Batch)));
            if (path_ == nullptr || jobs_ == nullptr || batches_ == nullptr) {
                failed_ = true;
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            for (std::uint32_t i = 0; i < settings_.batchRing; ++i)
                new (&batches_[i]) Batch{};
            std::memcpy(path_, path, length * sizeof(wchar_t));

            auto error = file_.Create(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
                                      settings_.unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN);
            if (Windows::Error::Failed(error)) {
                failed_ = true;
                return error;
            }
            reserved_ = RoundUp(bound, settings_.segmentSize) + settings_.segmentSize;
            base_     = static_cast<std::byte*>(VirtualAlloc(nullptr, static_cast<std::size_t>(reserved_), MEM_RESERVE, PAGE_READWRITE));
            if (base_ == nullptr) {
                failed_ = true;
                return Windows::Error::GetLastError();
            }
            open_ = true;
            return Windows::Error::Success;
        }

        /// <summary>Bytes appended so far: the file offset of the next append. Producer only.</summary>
        /// <returns>The number of bytes appended so far.</returns>
        std::uint64_t Position() const noexcept { return position_; }

        /// <summary><c>true</c> once the writer has failed.</summary>
        /// <returns><c>true</c> if the writer has failed; <c>false</c> while all went well.</returns>
        /// <remarks>
        /// Set when <see cref="Open"/>, an append or <see cref="WriteAt"/> has failed (see the class remarks);
        /// <see cref="Close"/> then returns <c>false</c>.
        /// </remarks>
        bool Failed() const noexcept { return failed_; }

        /// <summary>Appends bytes.</summary>
        /// <param name="data">The bytes; <c>nullptr</c>: zeros.</param>
        /// <param name="size">Number of bytes to append; must fit within the bound given to <see cref="Open"/>.</param>
        /// <remarks>Copies data at once (data may be reused after the call); never suspends.</remarks>
        /// <seealso cref="WriteDeferred"/>
        void Write(const void* data, std::size_t size) noexcept
        {
            if (!Reserve(size))
                return;
            if (data != nullptr)
                std::memcpy(base_ + position_, data, size); // else: fresh pages are zero
            position_ += size;
        }

        /// <summary>Appends room for bytes that a copy task fills later.</summary>
        /// <param name="source">
        /// The bytes to copy; borrowed, must stay valid and unchanged until the copy has run (see remarks).
        /// </param>
        /// <param name="size">
        /// Number of bytes to append; <c>0</c> does nothing; must fit within the bound given to <see cref="Open"/>.
        /// </param>
        /// <remarks>
        /// A copy task copies them from <paramref name="source"/> after the next <see cref="EndBatch"/>.
        /// source is borrowed: it must stay valid and unchanged until <see cref="LowestPendingSource"/> is above
        /// it (or <c>nullptr</c>), at the latest until <see cref="Close"/> returns. May suspend the producer when the job
        /// ring is full (it then runs or waits for the oldest batch); a batch is submitted early
        /// when it reaches half the job ring. The bytes are not written to the file before the
        /// copy has finished.
        /// </remarks>
        /// <seealso cref="Write"/>
        /// <seealso cref="CopyTask"/>
        void WriteDeferred(const void* source, std::size_t size) noexcept
        {
            const auto* src = static_cast<const std::byte*>(source);
            if (size == 0 || !Reserve(size))
                return;
            if (jobCount_ - batchJobBegin_ >= settings_.jobRing / 2)
                Submit(); // a batch with very many copies: split it
            while (jobCount_ - retiredJobs_ >= settings_.jobRing)
                WaitForOldest();
            if (jobCount_ == batchJobBegin_) {
                batchOutBegin_ = position_;
                batchMinSrc_   = src;
            } else if (src < batchMinSrc_) {
                batchMinSrc_ = src;
            }
            Job& job = jobs_[jobCount_ % settings_.jobRing];
            job.dst  = base_ + position_;
            job.src  = src;
            job.size = size;
            ++jobCount_;
            ++deferredCopies_;
            deferredBytes_ += size;
            position_ += size;
        }

        /// <summary>Hands the copies since the last batch to the copy tasks.</summary>
        /// <remarks>
        /// Also publishes the finished prefix to the writer (woken only when a new whole segment is
        /// complete). May suspend the producer when the batch ring is full. Without EndBatch the
        /// writer sees no progress until <see cref="Close"/>.
        /// </remarks>
        /// <seealso cref="WriteDeferred"/>
        void EndBatch() noexcept
        {
            Submit();
            Retire();
            const std::uint64_t done = DonePrefix();
            if (done / settings_.segmentSize > published_ / settings_.segmentSize)
                writerWake_.Set();
            published_ = done;
            doneTo_.store(done, std::memory_order_release);
        }

        /// <summary>Lowest source address the unfinished copies still read (<c>nullptr</c>: none).</summary>
        /// <returns>
        /// The lowest source address still in use, or <c>nullptr</c> if no deferred copy is unfinished.
        /// </returns>
        /// <remarks>
        /// Producer only; never suspends. Tracked per batch (its lowest source), so it is
        /// conservative: memory below the result may be reused or freed, memory at or above it may
        /// still be read. Most useful when sources are appended in rising address order.
        /// </remarks>
        /// <seealso cref="WriteDeferred"/>
        const std::byte* LowestPendingSource() noexcept
        {
            Retire();
            const std::byte*    low = nullptr;
            const std::uint64_t n   = submitted_.load(std::memory_order_relaxed);
            for (std::uint64_t i = retired_; i < n; ++i) {
                const std::byte* s = batches_[i % settings_.batchRing].minSrc;
                if (low == nullptr || s < low)
                    low = s;
            }
            if (jobCount_ != batchJobBegin_ && (low == nullptr || batchMinSrc_ < low))
                low = batchMinSrc_;
            return low;
        }

        /// <summary>Overwrites bytes appended earlier.</summary>
        /// <param name="pos">File offset in bytes of the first byte to overwrite.</param>
        /// <param name="data">The new bytes; copied at once, may be reused after the call.</param>
        /// <param name="size">
        /// Number of bytes to overwrite; <paramref name="pos"/> + <paramref name="size"/> must not exceed
        /// <see cref="Position"/>.
        /// </param>
        /// <remarks>
        /// The range must lie before <see cref="Position"/> (the position does not change) and must not overlap a
        /// deferred copy that may still be running. Producer only; never suspends (it briefly takes the lock the writer issues under). A
        /// range beyond Position, or a failed allocation for a patch, sets <see cref="Failed"/>. Bytes already
        /// issued to the file are kept as a patch and written by <see cref="Close"/> after the last write.
        /// </remarks>
        void WriteAt(std::uint64_t pos, const void* data, std::size_t size) noexcept
        {
            const auto* p = static_cast<const std::byte*>(data);
            if (pos + size > position_) {
                failed_ = true;
                return;
            }
            // the writer issues no write while a patch decides between the file and the memory
            AcquireSRWLockExclusive(&issueLock_);
            if (pos < issuedTo_ && size > 0) {
                const auto n     = static_cast<std::size_t>(pos + size <= issuedTo_ ? size : issuedTo_ - pos);
                auto*      patch = static_cast<Patch*>(HeapAlloc(GetProcessHeap(), 0, sizeof(Patch) + n));
                if (patch == nullptr) {
                    failed_ = true;
                    ReleaseSRWLockExclusive(&issueLock_);
                    return;
                }
                patch->pos  = pos;
                patch->size = n;
                patch->next = nullptr;
                std::memcpy(patch + 1, p, n);
                *patchTail_ = patch;
                patchTail_  = &patch->next;
                pos += n;
                p += n;
                size -= n;
            }
            if (size > 0)
                std::memcpy(base_ + pos, p, size);
            ReleaseSRWLockExclusive(&issueLock_);
        }

        /// <summary>Finishes the file.</summary>
        /// <returns><c>true</c> if all went well.</returns>
        /// <remarks>
        /// Finishes the copies and writes, sets the size and applies the patches.
        /// Producer only. Suspends the producer until all batches are done (running them itself
        /// if no copy task does) and the writer task has completed all its writes, so the
        /// writer task (<see cref="Writer"/>) must have been scheduled. Size and patches go through a second, buffered
        /// handle after the overlapped one is closed. After a failed <see cref="Open"/> it only lets the writer
        /// task end and returns <c>false</c>. A second call does nothing but return the result again.
        /// On return the file is complete and closed and <see cref="Stats"/> is final, but the writer task
        /// may still be on its way out (it signals the producer just before it returns). Usage rule: destroy
        /// the writer only after the writer task has ended as well (its exit notification, e.g. through a
        /// <see cref="Synchronization::TaskGroup"/>), and after the copy tasks have ended
        /// (<see cref="StopCopyTasks"/>).
        /// </remarks>
        /// <seealso cref="Open"/>
        /// <seealso cref="StopCopyTasks"/>
        bool Close() noexcept
        {
            if (!open_) {
                StopWriter(); // never opened, failed to open, or closed already: the writer has nothing to do
                return !failed_;
            }
            open_ = false;
            Submit();
            while (retired_ < submitted_.load(std::memory_order_relaxed))
                WaitForOldest();
            doneTo_.store(position_, std::memory_order_release);
            StopWriter();
            if (writeFailed_.load(std::memory_order_acquire))
                failed_ = true;
            if (Windows::Error::Failed(file_.Close()))
                failed_ = true;
            if (settings_.unbuffered || patches_ != nullptr)
                Finalize();
            return !failed_;
        }

        /// <summary>Lets copy tasks end.</summary>
        /// <param name="count">Number of copy tasks to release; at least the number scheduled.</param>
        /// <remarks>
        /// Call after <see cref="Close"/>, or when giving up. <paramref name="count"/> must be at least the number of
        /// copy tasks scheduled, or the rest stay suspended.
        /// Batches still submitted are run before a copy task ends. Any thread or task; never
        /// suspends.
        /// </remarks>
        /// <seealso cref="CopyTask"/>
        void StopCopyTasks(std::uint32_t count) noexcept
        {
            stopCopies_.store(true, std::memory_order_release);
            work_.Release(count);
        }

        /// <summary>Counters (after <see cref="Close"/>).</summary>
        /// <returns>A snapshot of the counters.</returns>
        /// <remarks>The writer task's counters are only safe to read once Close has returned.</remarks>
        /// <seealso cref="OrderedFileWriterStats"/>
        OrderedFileWriterStats Stats() const noexcept
        {
            OrderedFileWriterStats s;
            s.bytes          = position_;
            s.deferredCopies = deferredCopies_;
            s.deferredBytes  = deferredBytes_;
            s.producerWaited = producerWaited_;
            s.writerWaited   = writerWaited_;
            s.writerIssuing  = writerIssuing_;
            return s;
        }

    private:
        using Clock = std::chrono::steady_clock;

        struct Job {
            std::byte*       dst;
            const std::byte* src;
            std::size_t      size;
        };

        struct Batch {
            std::uint64_t             jobBegin = 0;
            std::uint64_t             jobEnd   = 0;
            std::uint64_t             outBegin = 0;
            const std::byte*          minSrc   = nullptr;
            std::atomic<std::uint8_t> done{0};
        };

        struct Patch {
            std::uint64_t pos;
            std::size_t   size;
            Patch*        next;
        };

        struct Range {
            std::uint64_t pos;
            std::uint64_t bytes;
        };

        class WriterTask final : public Tasks::Task
        {
        public:
            explicit WriterTask(OrderedFileWriter& writer) noexcept : writer_(writer) {}
            void Run() noexcept override { writer_.WriteLoop(); }

        private:
            OrderedFileWriter& writer_;
        };

        static std::uint64_t RoundUp(std::uint64_t n, std::uint64_t granule) noexcept { return (n + granule - 1) / granule * granule; }
        static double        Seconds(Clock::time_point from, Clock::time_point to) noexcept { return std::chrono::duration<double>(to - from).count(); }

        /// <summary>Commits room for more bytes.</summary>
        /// <param name="size">Number of bytes to append at the current position.</param>
        /// <returns><c>false</c> (and failed) if the reservation is exhausted.</returns>
        bool Reserve(std::size_t size) noexcept
        {
            const std::uint64_t end = position_ + size;
            if (failed_ || !open_ || end > reserved_ - settings_.segmentSize) {
                failed_ = true;
                return false;
            }
            if (end > committed_) {
                const std::uint64_t to = RoundUp(end, settings_.segmentSize);
                if (VirtualAlloc(base_ + committed_, static_cast<std::size_t>(to - committed_), MEM_COMMIT, PAGE_READWRITE) == nullptr) {
                    failed_ = true;
                    return false;
                }
                committed_ = to;
            }
            return true;
        }

        void Submit() noexcept
        {
            if (jobCount_ == batchJobBegin_)
                return;
            while (submitted_.load(std::memory_order_relaxed) - retired_ >= settings_.batchRing)
                WaitForOldest();
            const std::uint64_t n = submitted_.load(std::memory_order_relaxed);
            Batch&              b = batches_[n % settings_.batchRing];
            b.jobBegin            = batchJobBegin_;
            b.jobEnd              = jobCount_;
            b.outBegin            = batchOutBegin_;
            b.minSrc              = batchMinSrc_;
            b.done.store(0, std::memory_order_relaxed);
            batchJobBegin_ = jobCount_;
            submitted_.store(n + 1, std::memory_order_release);
            work_.Release();
        }

        void Retire() noexcept
        {
            const std::uint64_t n = submitted_.load(std::memory_order_relaxed);
            while (retired_ < n && batches_[retired_ % settings_.batchRing].done.load(std::memory_order_acquire) != 0) {
                retiredJobs_ = batches_[retired_ % settings_.batchRing].jobEnd;
                ++retired_;
            }
        }

        /// <summary>Runs the next batch nobody claimed yet; false if there is none.</summary>
        /// <returns><c>true</c> if a batch was run; <c>false</c> if none was waiting.</returns>
        bool TryRunBatch() noexcept
        {
            std::uint64_t i = claimed_.load(std::memory_order_relaxed);
            while (i < submitted_.load(std::memory_order_acquire)) {
                if (claimed_.compare_exchange_weak(i, i + 1, std::memory_order_acq_rel)) {
                    Batch& b = batches_[i % settings_.batchRing];
                    for (std::uint64_t j = b.jobBegin; j < b.jobEnd; ++j) {
                        const Job& job = jobs_[j % settings_.jobRing];
                        std::memcpy(job.dst, job.src, job.size);
                    }
                    b.done.store(1, std::memory_order_release);
                    doneEvent_.Set();
                    return true;
                }
            }
            return false;
        }

        /// <summary>Helps with a batch or waits for one, until the oldest one is retired.</summary>
        void WaitForOldest() noexcept
        {
            const auto before = retired_;
            Retire();
            while (retired_ == before) {
                if (!TryRunBatch()) {
                    const auto t0 = Clock::now();
                    doneEvent_.Wait();
                    producerWaited_ += Seconds(t0, Clock::now());
                }
                Retire();
            }
        }

        /// <summary>End of the output that is complete in memory.</summary>
        /// <returns>The file offset up to which all bytes are in place.</returns>
        std::uint64_t DonePrefix() const noexcept
        {
            // Write bytes are in place at once, so only deferred copies hold the prefix back: it
            // ends where the oldest unretired batch (or the open, unsubmitted one) starts.
            if (retired_ < submitted_.load(std::memory_order_relaxed))
                return batches_[retired_ % settings_.batchRing].outBegin;
            return jobCount_ != batchJobBegin_ ? batchOutBegin_ : position_;
        }

        void StopWriter() noexcept
        {
            if (writerJoined_)
                return;
            final_.store(true, std::memory_order_release);
            writerWake_.Set();
            writerDone_.Wait();
            writerJoined_ = true;
        }

        // writer task ------------------------------------------------------------------------------------------------

        void WriteLoop() noexcept
        {
            const std::uint64_t segment = settings_.segmentSize;
            for (;;) {
                // The copy tasks' bytes are visible here through a chain of release/acquire pairs:
                // Batch::done (copy task -> producer's Retire), then doneTo_ (producer -> writer).
                // Read final_ first: when it is set, doneTo_ already holds the final position.
                const bool          final = final_.load(std::memory_order_acquire);
                const std::uint64_t done  = doneTo_.load(std::memory_order_acquire);
                const std::uint64_t end   = final ? done : done / segment * segment;
                while (issuedTo_ < end) {
                    if (issued_ - completed_ == settings_.writesInFlight)
                        CompleteOldest(true);
                    Issue(end);
                }
                while (completed_ < issued_ && CompleteOldest(false)) {
                }
                if (final) {
                    while (completed_ < issued_)
                        CompleteOldest(true);
                    break;
                }
                writerWake_.Wait();
            }
            writerDone_.Set();
        }

        void Issue(std::uint64_t end) noexcept
        {
            const auto slot = static_cast<std::uint32_t>(issued_ % settings_.writesInFlight);
            AcquireSRWLockExclusive(&issueLock_);
            const std::uint64_t pos   = issuedTo_;
            const std::uint64_t bytes = end - pos < settings_.segmentSize ? end - pos : settings_.segmentSize;
            issuedTo_ += bytes;
            ReleaseSRWLockExclusive(&issueLock_);
            // unbuffered: the last write is rounded up to whole sectors (zero pages), the size is set at the end
            const std::uint64_t request = settings_.unbuffered ? RoundUp(bytes, SectorAlign) : bytes;
            auto&               req     = requests_[slot];
            req.Reset();
            req.SetOffset(pos);
            ranges_[slot]                     = Range{pos, request};
            const auto               t0     = Clock::now();
            const Windows::ErrorCode result = writeFailed_.load(std::memory_order_relaxed) ? static_cast<Windows::ErrorCode>(ERROR_WRITE_FAULT)
                                                                                           : file_.WriteAsync(base_ + pos, static_cast<DWORD>(request), req);
            writerIssuing_ += Seconds(t0, Clock::now());
            if (Windows::Error::Failed(result) && !Windows::Error::IsPending(result)) {
                writeFailed_.store(true, std::memory_order_relaxed);
                req.RequestCompleted(result, 0);
            }
            ++issued_;
        }

        bool CompleteOldest(bool block) noexcept
        {
            const auto slot = static_cast<std::uint32_t>(completed_ % settings_.writesInFlight);
            auto&      req  = requests_[slot];
            if (!req.TryWait()) {
                if (!block)
                    return false;
                const auto t0 = Clock::now();
                req.Wait();
                writerWaited_ += Seconds(t0, Clock::now());
            }
            if (Windows::Error::Failed(req.GetError()) || req.GetTransferredBytes() != ranges_[slot].bytes)
                writeFailed_.store(true, std::memory_order_relaxed);
            if (ranges_[slot].bytes == settings_.segmentSize && ranges_[slot].pos % settings_.segmentSize == 0)
                VirtualFree(base_ + ranges_[slot].pos, static_cast<std::size_t>(settings_.segmentSize), MEM_DECOMMIT);
            ++completed_;
            return true;
        }

        /// <summary>Sets the exact size and applies the patches.</summary>
        /// <remarks>Through a buffered handle after the last write.</remarks>
        void Finalize() noexcept
        {
            HANDLE h = CreateFileW(path_, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                failed_ = true;
                return;
            }
            if (settings_.unbuffered) {
                LARGE_INTEGER end;
                end.QuadPart = static_cast<long long>(position_);
                if (!SetFilePointerEx(h, end, nullptr, FILE_BEGIN) || !SetEndOfFile(h))
                    failed_ = true;
            }
            for (const Patch* patch = patches_; patch != nullptr; patch = patch->next) {
                OVERLAPPED ov   = {};
                ov.Offset       = static_cast<DWORD>(patch->pos);
                ov.OffsetHigh   = static_cast<DWORD>(patch->pos >> 32);
                DWORD written   = 0;
                if (!WriteFile(h, patch + 1, static_cast<DWORD>(patch->size), &written, &ov) || written != patch->size)
                    failed_ = true;
            }
            if (!CloseHandle(h))
                failed_ = true;
        }

        OrderedFileWriterSettings settings_;
        WriterTask                writerTask_;
        AsyncFile                 file_;
        wchar_t*                  path_      = nullptr;
        std::byte*                base_      = nullptr;
        std::uint64_t             reserved_  = 0;
        std::uint64_t             committed_ = 0;
        std::uint64_t             position_  = 0;
        bool                      open_      = false;
        bool                      failed_    = false;
        Patch*                    patches_   = nullptr;
        Patch**                   patchTail_ = &patches_;

        // copies (producer fills, copy tasks run)
        Job*                       jobs_          = nullptr;
        Batch*                     batches_       = nullptr;
        std::uint64_t              jobCount_      = 0;
        std::uint64_t              batchJobBegin_ = 0;
        std::uint64_t              batchOutBegin_ = 0;
        const std::byte*           batchMinSrc_   = nullptr;
        std::uint64_t              retired_       = 0;
        std::uint64_t              retiredJobs_   = 0;
        std::atomic<std::uint64_t> submitted_{0};
        std::atomic<std::uint64_t> claimed_{0};
        std::atomic<bool>          stopCopies_{false};

        Synchronization::Semaphore      work_;
        Synchronization::AutoResetEvent doneEvent_;

        // writes (writer task; issuedTo_ is also read by WriteAt under issueLock_)
        SRWLOCK                         issueLock_ = SRWLOCK_INIT;
        std::atomic<std::uint64_t>      doneTo_{0};
        std::atomic<bool>               final_{false};
        std::atomic<bool>               writeFailed_{false};
        Synchronization::AutoResetEvent writerWake_;
        Synchronization::AutoResetEvent writerDone_;
        std::uint64_t                   published_    = 0;
        bool                            writerJoined_ = false;
        std::uint64_t                   issuedTo_     = 0;
        std::uint64_t                   issued_       = 0;
        std::uint64_t                   completed_    = 0;
        WaitableAsyncFileIoRequest      requests_[MaxWritesInFlight];
        Range                           ranges_[MaxWritesInFlight] = {};

        // statistics
        std::uint64_t deferredCopies_ = 0;
        std::uint64_t deferredBytes_  = 0;
        double        producerWaited_ = 0;
        double        writerWaited_   = 0;
        double        writerIssuing_  = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::AsyncIo
