//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AsyncFile: a file opened for asynchronous I/O through the core (its I/O completion port on
// Windows, its io_uring on Linux). Single operations that a task waits for (Read/Write), raw
// asynchronous operations (ReadAsync/WriteAsync) and whole-file bulk transfers into or out of
// sequential memory (ReadFile/WriteFile).
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    define NOMINMAX
#    include <Windows.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;
import ThinkMeta.ConcurrentTasks.Platform.VirtualMemory;
export import ThinkMeta.ConcurrentTasks.Platform.File;
export import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFileIoRequest;
export import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    /// <summary>The shape of the asynchronous I/O of <see cref="AsyncFile::ReadFile"/> and <see cref="AsyncFile::WriteFile"/>.</summary>
    /// <remarks>
    /// What counts is the amount in flight (<see cref="blockSize"/> * <see cref="blocksInFlight"/>): below about
    /// 1 MB a fast disk waits for requests; measured cold on NVMe with four samples, 1 MB x 5 was best or as good
    /// as any other shape, and a block size that grows from small to large gave nothing over a fixed one.
    /// Out-of-range values are corrected, not rejected: blockSize <c>0</c> means <see cref="DefaultBlockSize"/>, and
    /// blocksInFlight is clamped to 1..<see cref="MaxBlocksInFlight"/>. The last block of ReadFile is shorter
    /// (the rest of the file); WriteFile blocks are whatever <see cref="Memory::IReadSequentialMemory::RequestReadMemory"/> hands out, at most
    /// blockSize. <see cref="startOffset"/> is used by ReadFile only (beyond the end it reads nothing);
    /// WriteFile always writes from offset <c>0</c>.
    /// </remarks>
    /// <seealso cref="AsyncFile::ReadFile"/>
    /// <seealso cref="AsyncFile::WriteFile"/>
    export struct BulkIoOptions {
        /// <summary>A byte count of one request.</summary>
        using ByteCount = AsyncIoRequestBase::ByteCount;

        /// <summary>Default bytes per request (1 MB).</summary>
        static constexpr ByteCount DefaultBlockSize = 1024 * 1024;

        /// <summary>Default number of requests in flight.</summary>
        static constexpr std::size_t DefaultBlocksInFlight = 5;

        /// <summary>Upper bound <see cref="blocksInFlight"/> is clamped to.</summary>
        static constexpr std::size_t MaxBlocksInFlight = 64;

        /// <summary>Sector size the last block of an unbuffered file is rounded up to.</summary>
        /// <remarks>
        /// Unbuffered (<c>FILE_FLAG_NO_BUFFERING</c>, <c>O_DIRECT</c>), every request must be a
        /// multiple of the sector size; <see cref="AsyncFile::ReadFile"/> reads the last block rounded
        /// up to this (the file ends the read), and <see cref="AsyncFile::WriteFile"/> writes a last
        /// block that is no multiple of it padded with zeros and cuts the file to its exact size.
        /// </remarks>
        static constexpr ByteCount SectorAlign = 4096;

        /// <summary>Bytes per request.</summary>
        /// <remarks><c>0</c> means <see cref="DefaultBlockSize"/>.</remarks>
        ByteCount blockSize = DefaultBlockSize;

        /// <summary>Requests in flight at the same time.</summary>
        /// <remarks>Clamped to 1..<see cref="MaxBlocksInFlight"/>.</remarks>
        std::size_t blocksInFlight = DefaultBlocksInFlight;

        /// <summary>First byte read by <see cref="AsyncFile::ReadFile"/>.</summary>
        /// <remarks>
        /// The memory gets the rest of the file; ignored by <see cref="AsyncFile::WriteFile"/>.
        /// Unbuffered, a multiple of <see cref="SectorAlign"/>.
        /// </remarks>
        std::uint64_t startOffset = 0;
    };

    /// <summary>A file for asynchronous I/O through the core.</summary>
    /// <remarks>
    /// On Windows the file is opened with <c>FILE_FLAG_OVERLAPPED</c> and associated with the core's
    /// I/O completion port; elsewhere its operations go to the core's io_uring
    /// (<see cref="SubmitIo"/>). Needs <see cref="EnableAsyncIo"/> (Core.h) before the first
    /// operation (on Windows before <see cref="Create"/>), and no operation may still be pending at
    /// <see cref="DisableAsyncIo"/>. All functions report errors as the platform's error code
    /// (<see cref="Platform::ErrorCode"/>: a Win32 error code on Windows, an <c>errno</c> elsewhere;
    /// <c>0</c> on success). Compare it through the names of <see cref="Platform::Error"/>
    /// (<c>EndOfFile</c>, <c>TimedOut</c>, <c>OperationAborted</c>, ...), which mean the same on every
    /// platform. <see cref="Read"/>, <see cref="Write"/>, <see cref="ReadFile"/> and <see cref="WriteFile"/>
    /// suspend the calling task while they wait, so call them from a task, or from an OS thread
    /// after <see cref="ConvertThreadToTask"/>. Completions run on the core's I/O thread (or on the calling
    /// thread when an operation completes at once on Windows). The object owns the file and closes it on
    /// destruction. <see cref="Close"/> or destroy it only when no operation is pending. The
    /// operations use no file pointer: every operation carries its offset.
    /// </remarks>
    /// <seealso cref="AsyncFileIoRequest"/>
    /// <seealso cref="Platform::File"/>
    export class AsyncFile
    {
    public:
        /// <summary>The platform's error code; <c>0</c> on success.</summary>
        using ErrorType = Platform::ErrorCode;

        /// <summary>Null-terminated path: UTF-16 on Windows (as for <c>CreateFileW</c>), UTF-8 elsewhere.</summary>
        using FileName = Platform::FileName;

        /// <summary>File size in bytes.</summary>
        using FileSize = Platform::File::FileSize;

        /// <summary>A byte count of one operation (<c>DWORD</c> on Windows, <c>std::uint32_t</c> elsewhere).</summary>
        using ByteCount = AsyncIoRequestBase::ByteCount;

        /// <summary><c>true</c> if a file is open.</summary>
        /// <returns><c>true</c> if a file is open; <c>false</c> before <see cref="Create"/> and after <see cref="Close"/>.</returns>
        bool IsValid() const noexcept;

        /// <summary>Opens or creates the file, on every platform alike.</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="options">
        /// How to open it. <see cref="Platform::FileOptions::unbuffered"/> makes the buffers, offsets
        /// and sizes of all transfers subject to sector alignment.
        /// </param>
        /// <returns><c>0</c> if the file is open; otherwise the error, and no file is open.</returns>
        /// <remarks>
        /// Precondition: no file is open. Does not suspend. On Windows the same as the overload
        /// with the flags of <c>CreateFileW</c> that the options stand for.
        /// </remarks>
        /// <seealso cref="Close"/>
        ErrorType Create(FileName fileName, const Platform::FileOptions& options) noexcept;

#if defined(CT_PLATFORM_WINDOWS)
        /// <summary>Opens or creates the file with the arguments of <c>CreateFileW</c> (Windows only).</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="access">Requested access: <c>GENERIC_READ</c>, <c>GENERIC_WRITE</c> or both.</param>
        /// <param name="shareMode">
        /// What other opens may do meanwhile: <c>0</c> (exclusive) or a combination of <c>FILE_SHARE_READ</c>,
        /// <c>FILE_SHARE_WRITE</c> and <c>FILE_SHARE_DELETE</c>.
        /// </param>
        /// <param name="creationDisposition">
        /// <c>CREATE_NEW</c>, <c>CREATE_ALWAYS</c>, <c>OPEN_EXISTING</c>, <c>OPEN_ALWAYS</c> or <c>TRUNCATE_EXISTING</c>.
        /// </param>
        /// <param name="flagsAndAttributes">
        /// <c>FILE_ATTRIBUTE_*</c> and <c>FILE_FLAG_*</c> values, e.g. <c>FILE_FLAG_NO_BUFFERING</c> or
        /// <c>FILE_FLAG_SEQUENTIAL_SCAN</c>; <c>FILE_FLAG_OVERLAPPED</c> is always added.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the file is open and associated; otherwise the <c>CreateFileW</c> or association
        /// error, and no file is open.
        /// </returns>
        /// <remarks>
        /// Calls <c>CreateFileW</c> with <c>FILE_FLAG_OVERLAPPED</c> added and associates the handle with the core's
        /// I/O completion port. Precondition: no file is open. Needs <see cref="EnableAsyncIo"/>. Does not suspend. The parameters go to CreateFileW unchanged
        /// (<c>FILE_FLAG_NO_BUFFERING</c> makes the buffers, offsets and sizes of all transfers subject
        /// to sector alignment). Returns the CreateFileW error, or the error of the association
        /// (<see cref="AssociateHandleWithIoCompletionPort"/>);
        /// if the association fails, the file is closed again, so after any error no file is open.
        /// </remarks>
        /// <seealso cref="Close"/>
        ErrorType Create(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition, DWORD flagsAndAttributes) noexcept;
#endif

        /// <summary>Closes the file (success if none is open). No operation may be pending.</summary>
        /// <returns><c>0</c> if the file was closed or none was open; otherwise the error of the close.</returns>
        /// <seealso cref="Create"/>
        ErrorType Close() noexcept;

        /// <summary>Gets the file size. Precondition: a file is open.</summary>
        /// <param name="size">Receives the file size in bytes; unchanged on failure.</param>
        /// <returns><c>0</c>, or the error of the call.</returns>
        ErrorType GetSize(FileSize& size) noexcept;

        /// <summary>Sets the file size. Precondition: a file is open.</summary>
        /// <param name="size">The new size in bytes; extending fills with zeros, shrinking cuts the file.</param>
        /// <returns><c>0</c>, or the error of the failed call.</returns>
        /// <remarks>
        /// Needs no file pointer, so it also works on a file opened unbuffered for a size inside a
        /// sector. Only while no write is in flight.
        /// </remarks>
        ErrorType SetSize(FileSize size) noexcept;

        /// <summary>Starts an asynchronous read.</summary>
        /// <param name="buffer">
        /// Receives the data; at least <paramref name="bytesToRead"/> bytes, sector-aligned for a file
        /// opened unbuffered.
        /// </param>
        /// <param name="bytesToRead">Maximum number of bytes to read.</param>
        /// <param name="asyncIoRequest">The request; its offset is the file position to read at.</param>
        /// <returns>
        /// <c>0</c>: done at once, the request is already completed (on the calling thread; Windows only).
        /// <see cref="Platform::Error::Pending"/>: the request completes later on the I/O thread. Any
        /// other code: the read was not started and the request will not complete. Reading at or
        /// beyond the end of the file fails with <see cref="Platform::Error::EndOfFile"/>: on Windows
        /// possibly at once, elsewhere always through the completion.
        /// </returns>
        /// <remarks>
        /// Never suspends. Set the offset (<see cref="AsyncFileIoRequest::SetOffset"/>, after <see cref="AsyncFileIoRequest::Reset"/>)
        /// before the call. <paramref name="buffer"/> and the
        /// request belong to the caller and must stay valid and untouched until the request has
        /// completed. A completed read may be shorter than <paramref name="bytesToRead"/> (end of file): check
        /// <see cref="AsyncIoRequestBase::GetTransferredBytes"/>.
        /// </remarks>
        /// <seealso cref="Read"/>
        /// <seealso cref="WaitableAsyncFileIoRequest"/>
        ErrorType ReadAsync(void* buffer, ByteCount bytesToRead, AsyncFileIoRequest& asyncIoRequest) noexcept;

        /// <summary>Starts an asynchronous write.</summary>
        /// <param name="buffer">
        /// The data to write; at least <paramref name="bytesToWrite"/> bytes, sector-aligned for a
        /// file opened unbuffered.
        /// </param>
        /// <param name="bytesToWrite">Number of bytes to write.</param>
        /// <param name="asyncIoRequest">The request; its offset is the file position to write at.</param>
        /// <returns>
        /// As <see cref="ReadAsync"/>: <c>0</c> (completed at once), <see cref="Platform::Error::Pending"/>, or an error (not started).
        /// </returns>
        /// <remarks>Never suspends. Ownership and lifetime of buffer and request as for ReadAsync.</remarks>
        /// <seealso cref="Write"/>
        /// <seealso cref="WaitableAsyncFileIoRequest"/>
        ErrorType WriteAsync(const void* buffer, ByteCount bytesToWrite, AsyncFileIoRequest& asyncIoRequest) noexcept;

        /// <summary>Reads from the file and waits for the result.</summary>
        /// <param name="buffer">
        /// Receives the data; at least <paramref name="bytesToRead"/> bytes, sector-aligned for a
        /// file opened unbuffered.
        /// </param>
        /// <param name="bytesToRead">Maximum number of bytes to read.</param>
        /// <param name="bytesRead">Bytes read (less than bytesToRead at the end of the file); <c>0</c> on error.</param>
        /// <param name="filePosition">File offset in bytes to read at.</param>
        /// <param name="timeout">
        /// <see cref="Duration::Infinite"/>: wait for the completion. Otherwise a read still pending after timeout
        /// is cancelled and <see cref="Platform::Error::TimedOut"/> is returned, unless it completed meanwhile.
        /// A timeout needs <see cref="EnableTimers"/>; without it the wait has no limit.
        /// </param>
        /// <returns>
        /// <c>0</c> if the read completed; <see cref="Platform::Error::TimedOut"/> if it was cancelled after
        /// <paramref name="timeout"/>; <see cref="Platform::Error::OperationAborted"/> if the task was cancelled
        /// (<see cref="CancelTask"/>); otherwise the error of the read (e.g. <see cref="Platform::Error::EndOfFile"/>).
        /// </returns>
        /// <remarks>
        /// Suspends the calling task until the read has completed (also after a timeout, until the
        /// cancellation is through), so <paramref name="buffer"/> may be reused as soon as it returns.
        /// A <see cref="CancelTask"/> of the calling task cancels a pending read like a timeout, also
        /// with <see cref="Duration::Infinite"/>. A read of a regular file may be past cancelling
        /// already; it then completes with its own result.
        /// </remarks>
        /// <seealso cref="ReadAsync"/>
        /// <seealso cref="Write"/>
        ErrorType Read(void* buffer, ByteCount bytesToRead, ByteCount& bytesRead, std::uint64_t filePosition, Duration timeout = Duration::Infinite()) noexcept;

        /// <summary>Writes to the file and waits for the result.</summary>
        /// <param name="buffer">
        /// The data to write; at least <paramref name="bytesToWrite"/> bytes, sector-aligned for a
        /// file opened unbuffered.
        /// </param>
        /// <param name="bytesToWrite">Number of bytes to write.</param>
        /// <param name="bytesWritten">Bytes written; <c>0</c> on error.</param>
        /// <param name="filePosition">File offset in bytes to write at.</param>
        /// <param name="timeout">
        /// As for <see cref="Read"/>: a write still pending after timeout is cancelled and gives <see cref="Platform::Error::TimedOut"/>.
        /// </param>
        /// <returns>
        /// <c>0</c> if the write completed; <see cref="Platform::Error::TimedOut"/> if it was cancelled after
        /// <paramref name="timeout"/>; <see cref="Platform::Error::OperationAborted"/> if the task was cancelled
        /// (<see cref="CancelTask"/>); otherwise the error of the write.
        /// </returns>
        /// <remarks>Suspends the calling task until the write has completed, and is cancelled, as Read is.</remarks>
        /// <seealso cref="WriteAsync"/>
        /// <seealso cref="Read"/>
        ErrorType Write(const void* buffer, ByteCount bytesToWrite, ByteCount& bytesWritten, std::uint64_t filePosition,
                        Duration timeout = Duration::Infinite()) noexcept;

        /// <summary>Reads a whole file into sequential memory.</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="fileOptions">
        /// How to open it, as for <see cref="Create"/>; needs read access, usually
        /// <see cref="Platform::FileCreation::OpenExisting"/>. Unbuffered, the memory's blocks and
        /// <see cref="BulkIoOptions::blockSize"/> must be sector-aligned. The last block is then requested
        /// rounded up to <see cref="BulkIoOptions::SectorAlign"/> (the memory must have room for that) and
        /// released with the bytes the file still had.
        /// </param>
        /// <param name="writeSequentialMemory">
        /// Receives the file content; must not be <c>nullptr</c>; borrowed, must outlive the call.
        /// </param>
        /// <param name="options">The shape of the I/O; reading starts at <see cref="BulkIoOptions::startOffset"/>.</param>
        /// <returns>
        /// <c>0</c> if the whole file was read; otherwise the first error (see remarks).
        /// </returns>
        /// <remarks>
        /// Opens <paramref name="fileName"/> (as <see cref="Create"/> does), reads it to the end into
        /// <paramref name="writeSequentialMemory"/> with asynchronous requests in flight, and closes it.
        /// Suspends the calling task while it waits for reads (and possibly inside the memory's
        /// <see cref="Memory::IWriteSequentialMemory::RequestWriteMemory"/>). Needs <see cref="EnableAsyncIo"/>.
        /// The memory gets <see cref="Memory::IWriteSequentialMemory::Initialize"/>(bytes to read)
        /// first, then one RequestWriteMemory/<see cref="Memory::IWriteSequentialMemory::ReleaseWriteMemory"/> per block
        /// in file order, and
        /// always <see cref="Memory::IWriteSequentialMemory::SetEndOfStream"/> at the end - also on every error.
        /// If the file cannot be opened or sized, the memory gets an empty stream (Initialize(0)).
        /// Each finished block goes to the memory at once, so a consumer can start on the first blocks
        /// while the rest is on its way; a memory with a bounded window must hold at least
        /// <see cref="BulkIoOptions::blocksInFlight"/> blocks. The memory is borrowed: it must outlive the call. Returns the first
        /// error (open, size, <see cref="Platform::Error::NotEnoughMemory"/> for a <c>nullptr</c> block or the request array, a
        /// read's error, or <see cref="Platform::Error::EndOfFile"/> if a read came back short because the file shrank
        /// meanwhile); after an error no block is released to the memory any more, but the
        /// call still waits for every read in flight before it returns.
        /// </remarks>
        /// <seealso cref="WriteFile"/>
        /// <seealso cref="BulkIoOptions"/>
        static ErrorType ReadFile(FileName fileName, const Platform::FileOptions& fileOptions, Memory::IWriteSequentialMemory* writeSequentialMemory,
                                  const BulkIoOptions& options = {}) noexcept;

        /// <summary>Writes a whole file from sequential memory.</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="fileOptions">
        /// How to open it, as for <see cref="Create"/>; needs write access, usually
        /// <see cref="Platform::FileCreation::CreateAlways"/>. Unbuffered, the memory's blocks and
        /// <see cref="BulkIoOptions::blockSize"/> must be sector-aligned (<see cref="BulkIoOptions::SectorAlign"/>).
        /// A last block that is not is taken as the end of the data: it is written as whole sectors
        /// from a zero-padded copy, and the file is then cut to its exact size.
        /// </param>
        /// <param name="readSequentialMemory">
        /// Supplies the data to write; must not be <c>nullptr</c>; borrowed, must outlive the call.
        /// </param>
        /// <param name="options">
        /// The shape of the I/O; <see cref="BulkIoOptions::startOffset"/> is ignored, writing starts at offset <c>0</c>.
        /// </param>
        /// <returns>
        /// <c>0</c> if all data was written; otherwise the first error (see remarks).
        /// </returns>
        /// <remarks>
        /// Opens <paramref name="fileName"/> (as <see cref="Create"/> does; use a creating option),
        /// writes the data of <paramref name="readSequentialMemory"/> from offset <c>0</c> with asynchronous requests in
        /// flight, and closes it. Suspends the calling task while it waits for writes (and possibly inside the memory's
        /// <see cref="Memory::IReadSequentialMemory::RequestReadMemory"/>). Needs <see cref="EnableAsyncIo"/>. Blocks of up
        /// to <paramref name="options"/>.<see cref="BulkIoOptions::blockSize"/> are taken with
        /// RequestReadMemory until it returns <c>nullptr</c> or <c>0</c> bytes (end of data); each is handed back
        /// with <see cref="Memory::IReadSequentialMemory::ReleaseReadMemory"/>(bytes written) once its write has
        /// completed, in order. The file
        /// is not truncated to the written size (with <see cref="Platform::FileCreation::OpenExisting"/> or
        /// <see cref="Platform::FileCreation::OpenAlways"/> older data beyond it stays), except unbuffered
        /// with a last block that is no multiple of
        /// <see cref="BulkIoOptions::SectorAlign"/>: then the file is cut to the written size. The memory
        /// is borrowed: it must outlive the call. Returns the first error (open,
        /// <see cref="Platform::Error::NotEnoughMemory"/> for the request array, or a write's error); after an error
        /// no new block is requested, but the call waits for every write in flight and still
        /// releases their blocks before it returns.
        /// </remarks>
        /// <seealso cref="ReadFile"/>
        /// <seealso cref="BulkIoOptions"/>
        static ErrorType WriteFile(FileName fileName, const Platform::FileOptions& fileOptions, Memory::IReadSequentialMemory* readSequentialMemory,
                                   const BulkIoOptions& options = {}) noexcept;

#if defined(CT_PLATFORM_WINDOWS)
        /// <summary>Reads a whole file into sequential memory; opened with the arguments of <c>CreateFileW</c> (Windows only).</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="access">Requested access, as for <see cref="Create"/>; must include <c>GENERIC_READ</c>.</param>
        /// <param name="shareMode">Share mode, as for <see cref="Create"/>.</param>
        /// <param name="creationDisposition">Creation disposition, as for <see cref="Create"/>; usually <c>OPEN_EXISTING</c>.</param>
        /// <param name="flagsAndAttributes">
        /// Flags and attributes, as for <see cref="Create"/>; <c>FILE_FLAG_NO_BUFFERING</c> reads unbuffered,
        /// as <see cref="Platform::FileOptions::unbuffered"/> does.
        /// </param>
        /// <param name="writeSequentialMemory">
        /// Receives the file content; must not be <c>nullptr</c>; borrowed, must outlive the call.
        /// </param>
        /// <param name="options">The shape of the I/O; reading starts at <see cref="BulkIoOptions::startOffset"/>.</param>
        /// <returns><c>ERROR_SUCCESS</c> if the whole file was read; otherwise the first error.</returns>
        /// <remarks>Otherwise as the overload with <see cref="Platform::FileOptions"/>.</remarks>
        /// <seealso cref="WriteFile"/>
        /// <seealso cref="BulkIoOptions"/>
        static ErrorType ReadFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition, DWORD flagsAndAttributes,
                                  Memory::IWriteSequentialMemory* writeSequentialMemory, const BulkIoOptions& options = {}) noexcept;

        /// <summary>Writes a whole file from sequential memory; opened with the arguments of <c>CreateFileW</c> (Windows only).</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="access">Requested access, as for <see cref="Create"/>; must include <c>GENERIC_WRITE</c>.</param>
        /// <param name="shareMode">Share mode, as for <see cref="Create"/>.</param>
        /// <param name="creationDisposition">
        /// Creation disposition, as for <see cref="Create"/>; usually <c>CREATE_ALWAYS</c>.
        /// </param>
        /// <param name="flagsAndAttributes">
        /// Flags and attributes, as for <see cref="Create"/>; <c>FILE_FLAG_NO_BUFFERING</c> writes unbuffered,
        /// as <see cref="Platform::FileOptions::unbuffered"/> does.
        /// </param>
        /// <param name="readSequentialMemory">
        /// Supplies the data to write; must not be <c>nullptr</c>; borrowed, must outlive the call.
        /// </param>
        /// <param name="options">
        /// The shape of the I/O; <see cref="BulkIoOptions::startOffset"/> is ignored, writing starts at offset <c>0</c>.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c> if all data was written; otherwise the first error.</returns>
        /// <remarks>Otherwise as the overload with <see cref="Platform::FileOptions"/>.</remarks>
        /// <seealso cref="ReadFile"/>
        /// <seealso cref="BulkIoOptions"/>
        static ErrorType WriteFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition, DWORD flagsAndAttributes,
                                   Memory::IReadSequentialMemory* readSequentialMemory, const BulkIoOptions& options = {}) noexcept;
#endif

    private:
        // Waits for a request issued by ReadAsync/WriteAsync when error is Error::Pending; any
        // other error (success = completed at once, or a failure to issue) is returned as is.
        ErrorType WaitForCompletion(ErrorType error, WaitableAsyncFileIoRequest_s& asyncIoRequest, Duration timeout) noexcept;

#if !defined(CT_PLATFORM_WINDOWS)
        // Fills the request for one read or write and submits it; Pending when the core took it.
        ErrorType Submit(IoOperation operation, void* buffer, ByteCount length, AsyncFileIoRequest& asyncIoRequest) noexcept;
#endif

        // The bulk transfers on a file Create has just tried to open (openError is its result).
        static ErrorType ReadOpenedFile(AsyncFile& asyncFile, ErrorType openError, bool unbuffered, Memory::IWriteSequentialMemory* writeSequentialMemory,
                                        const BulkIoOptions& options) noexcept;
        static ErrorType WriteOpenedFile(AsyncFile& asyncFile, ErrorType openError, bool unbuffered, Memory::IReadSequentialMemory* readSequentialMemory,
                                         const BulkIoOptions& options) noexcept;

        // whole sectors (BulkIoOptions::SectorAlign) for unbuffered requests
        template <typename T>
        static constexpr T RoundUpToSector(T bytes) noexcept
        {
            return (bytes + BulkIoOptions::SectorAlign - 1) / BulkIoOptions::SectorAlign * BulkIoOptions::SectorAlign;
        }

        Platform::File file_;
    };

    inline bool AsyncFile::IsValid() const noexcept
    {
        return file_.IsValid();
    }

#if defined(CT_PLATFORM_WINDOWS)
    inline AsyncFile::ErrorType AsyncFile::Create(FileName fileName, const Platform::FileOptions& options) noexcept
    {
        DWORD access;
        DWORD shareMode;
        DWORD creationDisposition;
        DWORD flagsAndAttributes;
        Windows::File::FlagsOf(options, access, shareMode, creationDisposition, flagsAndAttributes);

        return Create(fileName, access, shareMode, creationDisposition, flagsAndAttributes);
    }

    inline AsyncFile::ErrorType AsyncFile::Create(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition,
                                                  DWORD flagsAndAttributes) noexcept
    {
        CT_ASSERT(!IsValid());


        // open file with additional FILE_FLAG_OVERLAPPED attribute
        ErrorType error = file_.Create(fileName, access, shareMode, nullptr, creationDisposition, flagsAndAttributes | FILE_FLAG_OVERLAPPED, nullptr);

        if (Windows::Error::Succeeded(error)) {
            // associate file with the library's I/O completion port
            // (a file that cannot be associated would never see its completions: close it again)
            if (!AssociateHandleWithIoCompletionPort(file_.GetHandle(), &AsyncIoRequestCompleted)) {
                error = Windows::Error::GetLastError();
                file_.Close();
                if (Windows::Error::Succeeded(error))
                    error = ERROR_INVALID_HANDLE;
            }
        }

        return error;
    }
#else
    inline AsyncFile::ErrorType AsyncFile::Create(FileName fileName, const Platform::FileOptions& options) noexcept
    {
        CT_ASSERT(!IsValid());

        // io_uring needs nothing registered: every operation names its descriptor
        return file_.Create(fileName, options);
    }
#endif

    inline AsyncFile::ErrorType AsyncFile::Close() noexcept
    {
        return file_.Close();
    }

    inline AsyncFile::ErrorType AsyncFile::GetSize(FileSize& size) noexcept
    {
        return file_.GetSize(size);
    }

    inline AsyncFile::ErrorType AsyncFile::SetSize(FileSize size) noexcept
    {
#if defined(CT_PLATFORM_WINDOWS)
        FILE_END_OF_FILE_INFO endOfFile{};
        endOfFile.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        return file_.SetEofInformation(endOfFile);
#else
        return file_.SetEndOfFile(size);
#endif
    }

#if defined(CT_PLATFORM_WINDOWS)
    inline AsyncFile::ErrorType AsyncFile::ReadAsync(void* buffer, ByteCount bytesToRead, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        // read from file
        DWORD     bytesRead;
        ErrorType error = file_.Read(buffer, bytesToRead, &bytesRead, &asyncIoRequest.GetOverlapped());

        // update I/O request if operation completed immediately
        if (Windows::Error::Succeeded(error))
            asyncIoRequest.OnRequestCompletedImmediately(bytesRead);

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::WriteAsync(const void* buffer, ByteCount bytesToWrite, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        // write to file
        DWORD     bytesWrite;
        ErrorType error = file_.Write(buffer, bytesToWrite, &bytesWrite, &asyncIoRequest.GetOverlapped());

        // update I/O request if operation completed immediately
        if (Windows::Error::Succeeded(error))
            asyncIoRequest.OnRequestCompletedImmediately(bytesWrite);

        return error;
    }
#else
    inline AsyncFile::ErrorType AsyncFile::ReadAsync(void* buffer, ByteCount bytesToRead, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        return Submit(IoOperation::Read, buffer, bytesToRead, asyncIoRequest);
    }

    inline AsyncFile::ErrorType AsyncFile::WriteAsync(const void* buffer, ByteCount bytesToWrite, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        return Submit(IoOperation::Write, const_cast<void*>(buffer), bytesToWrite, asyncIoRequest); // only read by a write
    }

    inline AsyncFile::ErrorType AsyncFile::Submit(IoOperation operation, void* buffer, ByteCount length, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        // the offset is in the request already (SetOffset)
        IoRequest& request = asyncIoRequest.GetIoRequest();
        request.routine    = &AsyncIoRequestCompleted;
        request.buffer     = buffer;
        request.fd         = file_.GetHandle();
        request.length     = length;
        request.operation  = operation;

        // every accepted operation completes on the I/O thread, also one that is done at once
        return SubmitIo(&request) ? Platform::Error::Pending : Platform::Error::GetLastError();
    }
#endif

    inline AsyncFile::ErrorType AsyncFile::WaitForCompletion(ErrorType error, WaitableAsyncFileIoRequest_s& asyncIoRequest, Duration timeout) noexcept
    {
        using Platform::Error;

        if (!Error::IsPending(error))
            return error;

        // ends on the timeout or on a cancel of the task, also without a limit
        const auto status = asyncIoRequest.Wait(timeout);
        if (status == WaitStatus::Signaled)
            return asyncIoRequest.GetError();

        // Timed out or cancelled: cancel the operation, then wait for its completion without a
        // limit - the request lives in the caller's frame. If it completed meanwhile, its result
        // stands.
#if defined(CT_PLATFORM_WINDOWS)
        file_.CancelIo(asyncIoRequest.GetOverlapped());
#else
        _CancelIoRetrying(&asyncIoRequest.GetIoRequest());
#endif
        asyncIoRequest.Wait();
        return _ErrorAfterCancel<Error>(asyncIoRequest.GetError(), status);
    }

    inline AsyncFile::ErrorType AsyncFile::Read(void* buffer, ByteCount bytesToRead, ByteCount& bytesRead, std::uint64_t filePosition, Duration timeout) noexcept
    {
        // the request lives in this frame: WaitForCompletion returns only once it has completed
        WaitableAsyncFileIoRequest_s asyncIoRequest;
        asyncIoRequest.SetOffset(filePosition);

        auto error = WaitForCompletion(ReadAsync(buffer, bytesToRead, asyncIoRequest), asyncIoRequest, timeout);

        if (Platform::Error::Succeeded(error)) {
            bytesRead = asyncIoRequest.GetTransferredBytes();
        } else {
            bytesRead = 0;
        }

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::Write(const void* buffer, ByteCount bytesToWrite, ByteCount& bytesWritten, std::uint64_t filePosition,
                                                 Duration timeout) noexcept
    {
        WaitableAsyncFileIoRequest_s asyncIoRequest;
        asyncIoRequest.SetOffset(filePosition);

        auto error = WaitForCompletion(WriteAsync(buffer, bytesToWrite, asyncIoRequest), asyncIoRequest, timeout);

        if (Platform::Error::Succeeded(error)) {
            bytesWritten = asyncIoRequest.GetTransferredBytes();
        } else {
            bytesWritten = 0;
        }

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::ReadFile(FileName fileName, const Platform::FileOptions& fileOptions,
                                                    Memory::IWriteSequentialMemory* writeSequentialMemory, const BulkIoOptions& options) noexcept
    {
        AsyncFile       asyncFile;
        const ErrorType error = asyncFile.Create(fileName, fileOptions);
        return ReadOpenedFile(asyncFile, error, fileOptions.unbuffered, writeSequentialMemory, options);
    }

    inline AsyncFile::ErrorType AsyncFile::WriteFile(FileName fileName, const Platform::FileOptions& fileOptions,
                                                     Memory::IReadSequentialMemory* readSequentialMemory, const BulkIoOptions& options) noexcept
    {
        AsyncFile       asyncFile;
        const ErrorType error = asyncFile.Create(fileName, fileOptions);
        return WriteOpenedFile(asyncFile, error, fileOptions.unbuffered, readSequentialMemory, options);
    }

#if defined(CT_PLATFORM_WINDOWS)
    inline AsyncFile::ErrorType AsyncFile::ReadFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition,
                                                    DWORD flagsAndAttributes, Memory::IWriteSequentialMemory* writeSequentialMemory,
                                                    const BulkIoOptions& options) noexcept
    {
        AsyncFile       asyncFile;
        const ErrorType error = asyncFile.Create(fileName, access, shareMode, creationDisposition, flagsAndAttributes);
        return ReadOpenedFile(asyncFile, error, (flagsAndAttributes & FILE_FLAG_NO_BUFFERING) != 0, writeSequentialMemory, options);
    }

    inline AsyncFile::ErrorType AsyncFile::WriteFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition,
                                                     DWORD flagsAndAttributes, Memory::IReadSequentialMemory* readSequentialMemory,
                                                     const BulkIoOptions& options) noexcept
    {
        AsyncFile       asyncFile;
        const ErrorType error = asyncFile.Create(fileName, access, shareMode, creationDisposition, flagsAndAttributes);
        return WriteOpenedFile(asyncFile, error, (flagsAndAttributes & FILE_FLAG_NO_BUFFERING) != 0, readSequentialMemory, options);
    }
#endif

    inline AsyncFile::ErrorType AsyncFile::ReadOpenedFile(AsyncFile& asyncFile, ErrorType openError, bool unbuffered,
                                                          Memory::IWriteSequentialMemory* writeSequentialMemory, const BulkIoOptions& options) noexcept
    {
        using Platform::Error;

        FileSize  size  = 0;
        ErrorType error = openError;
        if (Error::Succeeded(error))
            error = asyncFile.GetSize(size);
        if (Error::Failed(error)) {
            // an empty stream, so that a consumer waiting for the size gets one
            writeSequentialMemory->Initialize(0);
            writeSequentialMemory->SetEndOfStream();
            return error;
        }

        const std::uint64_t begin = options.startOffset < size ? options.startOffset : size;
        writeSequentialMemory->Initialize(static_cast<std::size_t>(size - begin));

        const ByteCount   blockSize = options.blockSize > 0 ? options.blockSize : BulkIoOptions::DefaultBlockSize;
        const std::size_t inFlight  = std::clamp<std::size_t>(options.blocksInFlight, 1, BulkIoOptions::MaxBlocksInFlight);
        std::unique_ptr<WaitableAsyncFileIoRequest_s[]> requests(new (std::nothrow) WaitableAsyncFileIoRequest_s[inFlight]);
        if (!requests) {
            writeSequentialMemory->SetEndOfStream();
            return Error::NotEnoughMemory;
        }

        // requests[] is a ring: request n uses slot n % inFlight; issued - released are in flight
        std::uint64_t offset   = begin;
        std::size_t   issued   = 0;
        std::size_t   released = 0;
        CT_ASSERT(!unbuffered || begin % BulkIoOptions::SectorAlign == 0);
        // the oldest request has finished: its block goes to the memory, unless an error has
        // ended the stream (then only the requests in flight are waited for)
        const auto release = [&]() noexcept {
            auto& request = requests[released % inFlight];
            if (Error::Succeeded(error) && Error::Failed(request.GetError()))
                error = request.GetError();
            // a short read: the file shrank after GetSize; later blocks would land shifted
            if (Error::Succeeded(error) && request.GetTransferredBytes() != std::min<std::uint64_t>(blockSize, size - request.GetOffset()))
                error = Error::EndOfFile;
            if (Error::Succeeded(error))
                writeSequentialMemory->ReleaseWriteMemory(request.GetTransferredBytes());
            ++released;
        };

        while (released < issued || (Error::Succeeded(error) && offset < size)) {
            // finished blocks first: the memory may wait for room in RequestWriteMemory
            while (released < issued && requests[released % inFlight].TryWait())
                release();

            if (Error::Succeeded(error) && offset < size && issued - released < inFlight) {
                const auto bytes = static_cast<ByteCount>(std::min<std::uint64_t>(blockSize, size - offset));
                // unbuffered: whole sectors, the file ends the last read (it transfers what is left)
                const ByteCount request = unbuffered ? RoundUpToSector(bytes) : bytes;
                std::byte*      memory  = writeSequentialMemory->RequestWriteMemory(request);
                if (memory == nullptr) {
                    error = Error::NotEnoughMemory;
                    continue;
                }
                auto& asyncRequest = requests[issued % inFlight];
                asyncRequest.Reset();
                asyncRequest.SetOffset(offset);
                const ErrorType result = _StartRetrying([&] { return asyncFile.ReadAsync(memory, request, asyncRequest); });
                if (Error::Failed(result) && !Error::IsPending(result)) {
                    error = result;
                    continue;
                }
                offset += bytes;
                ++issued;
                continue;
            }

            if (released < issued) {
                requests[released % inFlight].Wait();
                release();
            }
        }

        asyncFile.Close();
        writeSequentialMemory->SetEndOfStream();
        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::WriteOpenedFile(AsyncFile& asyncFile, ErrorType openError, bool unbuffered,
                                                           Memory::IReadSequentialMemory* readSequentialMemory, const BulkIoOptions& options) noexcept
    {
        using Platform::Error;

        ErrorType error = openError;
        if (Error::Failed(error))
            return error;

        const ByteCount   blockSize = options.blockSize > 0 ? options.blockSize : BulkIoOptions::DefaultBlockSize;
        const std::size_t inFlight  = std::clamp<std::size_t>(options.blocksInFlight, 1, BulkIoOptions::MaxBlocksInFlight);
        std::unique_ptr<WaitableAsyncFileIoRequest_s[]> requests(new (std::nothrow) WaitableAsyncFileIoRequest_s[inFlight]);
        if (!requests)
            return Error::NotEnoughMemory;

        std::uint64_t offset      = 0;
        std::size_t   issued      = 0;
        std::size_t   released    = 0;
        bool          endOfStream = false;
        // unbuffered: a last block that is no whole number of sectors is written from a
        // sector-aligned copy padded with zeros, and the file is cut to its exact size before the
        // file closes
        Platform::VirtualMemory tailMemory;          // the padded copy (page-aligned, zeroed)
        std::byte*              tailCopy  = nullptr; // set once its write is issued
        std::size_t             tailBytes = 0;       // the block's own bytes
        ByteCount               expected[BulkIoOptions::MaxBlocksInFlight] = {}; // what each write is to transfer
        // the oldest request has finished: its block goes back to the memory
        const auto release = [&]() noexcept {
            auto& request = requests[released % inFlight];
            if (Error::Succeeded(error) && Error::Failed(request.GetError()))
                error = request.GetError();
            // the tail is the last request issued; it transferred the padded size, the memory gets
            // back the block's bytes
            const ByteCount transferred = request.GetTransferredBytes();
            // a short write (a full disk, a quota) did not fail, but the file is not complete
            if (Error::Succeeded(error) && transferred != expected[released % inFlight])
                error = Error::WriteFault;
            const bool      tail        = tailCopy != nullptr && released + 1 == issued;
            readSequentialMemory->ReleaseReadMemory(tail && transferred > tailBytes ? tailBytes : transferred);
            ++released;
        };

        while (released < issued || (Error::Succeeded(error) && !endOfStream)) {
            // finished blocks first: the memory may wait for data in RequestReadMemory, and its
            // producer for the room they free
            while (released < issued && requests[released % inFlight].TryWait())
                release();

            if (Error::Succeeded(error) && !endOfStream && issued - released < inFlight) {
                std::size_t      bytes = blockSize;
                const std::byte* data  = readSequentialMemory->RequestReadMemory(bytes);
                if (data == nullptr || bytes == 0) {
                    endOfStream = true;
                    continue;
                }
                const std::byte* buffer = data;
                std::size_t      size   = bytes;
                std::byte*       copy   = nullptr;
                if (unbuffered && bytes % BulkIoOptions::SectorAlign != 0) {
                    // the last block: whole sectors from a zeroed copy, nothing after it
                    endOfStream = true;
                    size        = RoundUpToSector(bytes);
                    if (Error::Failed(tailMemory.Reserve(size)) || Error::Failed(Platform::VirtualMemory::Commit(tailMemory.GetAllocationBase(), size))) {
                        tailMemory.Release();
                        readSequentialMemory->ReleaseReadMemory(0);
                        error = Error::NotEnoughMemory;
                        continue;
                    }
                    copy = static_cast<std::byte*>(tailMemory.GetAllocationBase());
                    std::memcpy(copy, data, bytes);
                    buffer = copy;
                }
                auto& request = requests[issued % inFlight];
                request.Reset();
                request.SetOffset(offset);
                expected[issued % inFlight] = static_cast<ByteCount>(size);
                const ErrorType result      = _StartRetrying([&] { return asyncFile.WriteAsync(buffer, static_cast<ByteCount>(size), request); });
                if (Error::Failed(result) && !Error::IsPending(result)) {
                    if (copy != nullptr)
                        tailMemory.Release();
                    readSequentialMemory->ReleaseReadMemory(0);
                    error = result;
                    continue;
                }
                if (copy != nullptr) {
                    // known to release only once the write is issued: the tail is the last request
                    tailCopy  = copy;
                    tailBytes = bytes;
                }
                offset += bytes;
                ++issued;
                continue;
            }

            if (released < issued) {
                requests[released % inFlight].Wait();
                release();
            }
        }

        if (tailCopy != nullptr) {
            // The padded last sector is written: cut the file to its exact size on this very file.
            // SetSize needs no file pointer, which an unbuffered Windows handle could not even be
            // set to a position inside a sector.
            if (Error::Succeeded(error))
                error = asyncFile.SetSize(offset);
            tailMemory.Release();
        }

        asyncFile.Close();
        return error;
    }
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo