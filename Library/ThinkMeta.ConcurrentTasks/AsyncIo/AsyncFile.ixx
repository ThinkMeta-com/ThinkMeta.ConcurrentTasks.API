//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AsyncFile: a file opened for overlapped I/O on the core's I/O completion port. Single
// operations that a task waits for (Read/Write), raw overlapped operations (ReadAsync/WriteAsync)
// and whole-file bulk transfers into or out of sequential memory (ReadFile/WriteFile).
//
// Preliminary API: everything in this file is subject to change.
//
module;

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;
import ThinkMeta.ConcurrentTasks.Windows.File;
export import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFileIoRequest;
export import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    /// <summary>The shape of the overlapped I/O of <see cref="AsyncFile::ReadFile"/> and <see cref="AsyncFile::WriteFile"/>.</summary>
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
        /// <summary>Default bytes per request (1 MB).</summary>
        static constexpr DWORD DefaultBlockSize = 1024 * 1024;

        /// <summary>Default number of requests in flight.</summary>
        static constexpr std::size_t DefaultBlocksInFlight = 5;

        /// <summary>Upper bound <see cref="blocksInFlight"/> is clamped to.</summary>
        static constexpr std::size_t MaxBlocksInFlight = 64;

        /// <summary>Bytes per request.</summary>
        /// <remarks><c>0</c> means <see cref="DefaultBlockSize"/>.</remarks>
        DWORD blockSize = DefaultBlockSize;

        /// <summary>Requests in flight at the same time.</summary>
        /// <remarks>Clamped to 1..<see cref="MaxBlocksInFlight"/>.</remarks>
        std::size_t blocksInFlight = DefaultBlocksInFlight;

        /// <summary>First byte read by <see cref="AsyncFile::ReadFile"/>.</summary>
        /// <remarks>The memory gets the rest of the file; ignored by <see cref="AsyncFile::WriteFile"/>.</remarks>
        std::uint64_t startOffset = 0;
    };

    /// <summary>A file for overlapped I/O on the core's I/O completion port.</summary>
    /// <remarks>
    /// The file is opened with <c>FILE_FLAG_OVERLAPPED</c> and associated with the core's port.
    /// Needs <see cref="EnableAsyncIo"/> (Core.h) before <see cref="Create"/>, and no operation may still be pending at
    /// <see cref="DisableAsyncIo"/>. All functions report errors as a Win32 error code (<c>ERROR_SUCCESS</c> on
    /// success; <see cref="ReadAsync"/>/<see cref="WriteAsync"/> also <c>ERROR_IO_PENDING</c>). <see cref="Read"/>, <see cref="Write"/>,
    /// <see cref="ReadFile"/> and <see cref="WriteFile"/>
    /// suspend the calling task while they wait, so call them from a task, or from an OS thread
    /// after <see cref="ConvertThreadToTask"/>. Completions run on the core's I/O thread (or on the calling
    /// thread when an operation completes at once). The object owns the handle and closes it on
    /// destruction. <see cref="Close"/> or destroy it only when no operation is pending. The handle has no file
    /// pointer: every operation carries its offset.
    /// </remarks>
    /// <seealso cref="AsyncFileIoRequest"/>
    /// <seealso cref="Windows::File"/>
    export class AsyncFile
    {
    public:
        /// <summary>Win32 error code; <c>ERROR_SUCCESS</c> (<c>0</c>) on success.</summary>
        using ErrorType = Windows::ErrorCode;

        /// <summary>Null-terminated wide path, as for <c>CreateFileW</c>.</summary>
        using FileName = Windows::File::FileName;

        /// <summary>File size in bytes.</summary>
        using FileSize = Windows::File::FileSize;

        /// <summary><c>true</c> if a file is open.</summary>
        /// <returns><c>true</c> if a file is open; <c>false</c> before <see cref="Create"/> and after <see cref="Close"/>.</returns>
        bool IsValid() const noexcept;

        /// <summary>Opens or creates the file.</summary>
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

        /// <summary>Closes the handle (success if none is open). No operation may be pending.</summary>
        /// <returns><c>ERROR_SUCCESS</c> if the file was closed or none was open; otherwise the <c>CloseHandle</c> error.</returns>
        /// <seealso cref="Create"/>
        ErrorType Close() noexcept;

        /// <summary>Gets the file size (<c>GetFileSizeEx</c>). Precondition: a file is open.</summary>
        /// <param name="size">Receives the file size in bytes; unchanged on failure.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of <c>GetFileSizeEx</c>.</returns>
        ErrorType GetSize(FileSize& size) noexcept;

        /// <summary>Starts an overlapped read.</summary>
        /// <param name="buffer">
        /// Receives the data; at least <paramref name="bytesToRead"/> bytes, sector-aligned with
        /// <c>FILE_FLAG_NO_BUFFERING</c>.
        /// </param>
        /// <param name="bytesToRead">Maximum number of bytes to read.</param>
        /// <param name="asyncIoRequest">The request; its offset is the file position to read at.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>: done at once, the request is already completed (on the calling thread).
        /// <c>ERROR_IO_PENDING</c>: the request completes later on the I/O thread. Any other code: the
        /// read was not started and the request will not complete (reading at or beyond the end of
        /// the file can fail this way with <c>ERROR_HANDLE_EOF</c>).
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
        ErrorType ReadAsync(void* buffer, DWORD bytesToRead, AsyncFileIoRequest& asyncIoRequest) noexcept;

        /// <summary>Starts an overlapped write.</summary>
        /// <param name="buffer">
        /// The data to write; at least <paramref name="bytesToWrite"/> bytes, sector-aligned with
        /// <c>FILE_FLAG_NO_BUFFERING</c>.
        /// </param>
        /// <param name="bytesToWrite">Number of bytes to write.</param>
        /// <param name="asyncIoRequest">The request; its offset is the file position to write at.</param>
        /// <returns>
        /// As <see cref="ReadAsync"/>: <c>ERROR_SUCCESS</c> (completed at once), <c>ERROR_IO_PENDING</c>, or an error (not started).
        /// </returns>
        /// <remarks>Never suspends. Ownership and lifetime of buffer and request as for ReadAsync.</remarks>
        /// <seealso cref="Write"/>
        /// <seealso cref="WaitableAsyncFileIoRequest"/>
        ErrorType WriteAsync(const void* buffer, DWORD bytesToWrite, AsyncFileIoRequest& asyncIoRequest) noexcept;

        /// <summary>Reads from the file and waits for the result.</summary>
        /// <param name="buffer">
        /// Receives the data; at least <paramref name="bytesToRead"/> bytes, sector-aligned with
        /// <c>FILE_FLAG_NO_BUFFERING</c>.
        /// </param>
        /// <param name="bytesToRead">Maximum number of bytes to read.</param>
        /// <param name="bytesRead">Bytes read (less than bytesToRead at the end of the file); <c>0</c> on error.</param>
        /// <param name="filePosition">File offset in bytes to read at.</param>
        /// <param name="timeout">
        /// <see cref="Duration::Infinite"/>: wait for the completion. Otherwise a read still pending after timeout
        /// is cancelled (<c>CancelIoEx</c>) and <c>ERROR_TIMEOUT</c> is returned, unless it completed meanwhile.
        /// A timeout needs <see cref="EnableTimers"/>; without it the wait has no limit.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the read completed; <c>ERROR_TIMEOUT</c> if it was cancelled after
        /// <paramref name="timeout"/>; otherwise the Win32 error code of the read (e.g. <c>ERROR_HANDLE_EOF</c>).
        /// </returns>
        /// <remarks>
        /// Suspends the calling task until the read has completed (also after a timeout, until the
        /// cancellation is through), so <paramref name="buffer"/> may be reused as soon as it returns.
        /// </remarks>
        /// <seealso cref="ReadAsync"/>
        /// <seealso cref="Write"/>
        ErrorType Read(void* buffer, DWORD bytesToRead, DWORD& bytesRead, std::uint64_t filePosition, Duration timeout = Duration::Infinite()) noexcept;

        /// <summary>Writes to the file and waits for the result.</summary>
        /// <param name="buffer">
        /// The data to write; at least <paramref name="bytesToWrite"/> bytes, sector-aligned with
        /// <c>FILE_FLAG_NO_BUFFERING</c>.
        /// </param>
        /// <param name="bytesToWrite">Number of bytes to write.</param>
        /// <param name="bytesWritten">Bytes written; <c>0</c> on error.</param>
        /// <param name="filePosition">File offset in bytes to write at.</param>
        /// <param name="timeout">
        /// As for <see cref="Read"/>: a write still pending after timeout is cancelled and gives <c>ERROR_TIMEOUT</c>.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the write completed; <c>ERROR_TIMEOUT</c> if it was cancelled after
        /// <paramref name="timeout"/>; otherwise the Win32 error code of the write.
        /// </returns>
        /// <remarks>Suspends the calling task until the write has completed, as Read does.</remarks>
        /// <seealso cref="WriteAsync"/>
        /// <seealso cref="Read"/>
        ErrorType Write(const void* buffer, DWORD bytesToWrite, DWORD& bytesWritten, std::uint64_t filePosition,
                        Duration timeout = Duration::Infinite()) noexcept;

        /// <summary>Reads a whole file into sequential memory.</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="access">Requested access, as for <see cref="Create"/>; must include <c>GENERIC_READ</c>.</param>
        /// <param name="shareMode">Share mode, as for <see cref="Create"/>.</param>
        /// <param name="creationDisposition">Creation disposition, as for <see cref="Create"/>; usually <c>OPEN_EXISTING</c>.</param>
        /// <param name="flagsAndAttributes">
        /// Flags and attributes, as for <see cref="Create"/>; with <c>FILE_FLAG_NO_BUFFERING</c> the memory's blocks
        /// and <see cref="BulkIoOptions::blockSize"/> must be sector-aligned.
        /// </param>
        /// <param name="writeSequentialMemory">
        /// Receives the file content; must not be <c>nullptr</c>; borrowed, must outlive the call.
        /// </param>
        /// <param name="options">The shape of the I/O; reading starts at <see cref="BulkIoOptions::startOffset"/>.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the whole file was read; otherwise the first error (see remarks).
        /// </returns>
        /// <remarks>
        /// Opens <paramref name="fileName"/> (parameters as for <see cref="Create"/>), reads it to the end into
        /// <paramref name="writeSequentialMemory"/> with overlapped requests in flight, and closes it.
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
        /// error (open, size, <c>ERROR_NOT_ENOUGH_MEMORY</c> for a <c>nullptr</c> block or the request array, or a
        /// read's Win32 error); after an error no block is released to the memory any more, but the
        /// call still waits for every read in flight before it returns.
        /// </remarks>
        /// <seealso cref="WriteFile"/>
        /// <seealso cref="BulkIoOptions"/>
        static ErrorType ReadFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition, DWORD flagsAndAttributes,
                                  Memory::IWriteSequentialMemory* writeSequentialMemory, const BulkIoOptions& options = {}) noexcept;

        /// <summary>Writes a whole file from sequential memory.</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="access">Requested access, as for <see cref="Create"/>; must include <c>GENERIC_WRITE</c>.</param>
        /// <param name="shareMode">Share mode, as for <see cref="Create"/>.</param>
        /// <param name="creationDisposition">
        /// Creation disposition, as for <see cref="Create"/>; usually <c>CREATE_ALWAYS</c>.
        /// </param>
        /// <param name="flagsAndAttributes">
        /// Flags and attributes, as for <see cref="Create"/>; with <c>FILE_FLAG_NO_BUFFERING</c> the memory's blocks
        /// and <see cref="BulkIoOptions::blockSize"/> must be sector-aligned.
        /// </param>
        /// <param name="readSequentialMemory">
        /// Supplies the data to write; must not be <c>nullptr</c>; borrowed, must outlive the call.
        /// </param>
        /// <param name="options">
        /// The shape of the I/O; <see cref="BulkIoOptions::startOffset"/> is ignored, writing starts at offset <c>0</c>.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if all data was written; otherwise the first error (see remarks).
        /// </returns>
        /// <remarks>
        /// Opens <paramref name="fileName"/> (parameters as for <see cref="Create"/>; use a creating disposition),
        /// writes the data of <paramref name="readSequentialMemory"/> from offset <c>0</c> with overlapped requests in
        /// flight, and closes it. Suspends the calling task while it waits for writes (and possibly inside the memory's
        /// <see cref="Memory::IReadSequentialMemory::RequestReadMemory"/>). Needs <see cref="EnableAsyncIo"/>. Blocks of up
        /// to <paramref name="options"/>.<see cref="BulkIoOptions::blockSize"/> are taken with
        /// RequestReadMemory until it returns <c>nullptr</c> or <c>0</c> bytes (end of data); each is handed back
        /// with <see cref="Memory::IReadSequentialMemory::ReleaseReadMemory"/>(bytes written) once its write has
        /// completed, in order. The file
        /// is not truncated to the written size (with <c>OPEN_EXISTING</c>/<c>OPEN_ALWAYS</c> older data beyond it
        /// stays). The memory is borrowed: it must outlive the call. Returns the first error (open,
        /// <c>ERROR_NOT_ENOUGH_MEMORY</c> for the request array, or a write's Win32 error); after an error
        /// no new block is requested, but the call waits for every write in flight and still
        /// releases their blocks before it returns.
        /// </remarks>
        /// <seealso cref="ReadFile"/>
        /// <seealso cref="BulkIoOptions"/>
        static ErrorType WriteFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition, DWORD flagsAndAttributes,
                                   Memory::IReadSequentialMemory* readSequentialMemory, const BulkIoOptions& options = {}) noexcept;

    private:
        // Waits for a request issued by ReadAsync/WriteAsync when error is ERROR_IO_PENDING; any
        // other error (success = completed at once, or a failure to issue) is returned as is.
        ErrorType WaitForCompletion(ErrorType error, WaitableAsyncFileIoRequest_s& asyncIoRequest, Duration timeout) noexcept;

        Windows::File file_;
    };

    inline bool AsyncFile::IsValid() const noexcept
    {
        return file_.IsValid();
    }

    inline AsyncFile::ErrorType AsyncFile::Create(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition,
                                                  DWORD flagsAndAttributes) noexcept
    {
        CT_ASSERT(!IsValid());

        file_.Close();

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

    inline AsyncFile::ErrorType AsyncFile::Close() noexcept
    {
        return file_.Close();
    }

    inline AsyncFile::ErrorType AsyncFile::GetSize(FileSize& size) noexcept
    {
        return file_.GetSize(size);
    }

    inline AsyncFile::ErrorType AsyncFile::ReadAsync(void* buffer, DWORD bytesToRead, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        // read from file
        DWORD     bytesRead;
        ErrorType error = file_.Read(buffer, bytesToRead, &bytesRead, &asyncIoRequest.GetOverlapped());

        // update I/O request if operation completed immediately
        if (Windows::Error::Succeeded(error))
            asyncIoRequest.OnRequestCompletedImmediately(bytesRead);

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::WriteAsync(const void* buffer, DWORD bytesToWrite, AsyncFileIoRequest& asyncIoRequest) noexcept
    {
        // write to file
        DWORD     bytesWrite;
        ErrorType error = file_.Write(buffer, bytesToWrite, &bytesWrite, &asyncIoRequest.GetOverlapped());

        // update I/O request if operation completed immediately
        if (Windows::Error::Succeeded(error))
            asyncIoRequest.OnRequestCompletedImmediately(bytesWrite);

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::WaitForCompletion(ErrorType error, WaitableAsyncFileIoRequest_s& asyncIoRequest, Duration timeout) noexcept
    {
        if (!Windows::Error::IsPending(error))
            return error;

        if (timeout.IsInfinite()) {
            asyncIoRequest.Wait();
            return asyncIoRequest.GetError();
        }

        if (asyncIoRequest.Wait(timeout))
            return asyncIoRequest.GetError();

        // Timed out: cancel the operation, then wait for its completion without a limit - the
        // OVERLAPPED lives in the caller's frame. If it completed meanwhile, its result stands.
        CancelIoEx(file_.GetHandle(), &asyncIoRequest.GetOverlapped());
        asyncIoRequest.Wait();

        error = asyncIoRequest.GetError();
        return error == Windows::Error::OperationAborted ? Windows::Error::TimedOut : error;
    }

    inline AsyncFile::ErrorType AsyncFile::Read(void* buffer, DWORD bytesToRead, DWORD& bytesRead, std::uint64_t filePosition, Duration timeout) noexcept
    {
        // the request lives in this frame: WaitForCompletion returns only once it has completed
        WaitableAsyncFileIoRequest_s asyncIoRequest;
        asyncIoRequest.SetOffset(filePosition);

        auto error = WaitForCompletion(ReadAsync(buffer, bytesToRead, asyncIoRequest), asyncIoRequest, timeout);

        if (Windows::Error::Succeeded(error)) {
            bytesRead = asyncIoRequest.GetTransferredBytes();
        } else {
            bytesRead = 0;
        }

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::Write(const void* buffer, DWORD bytesToWrite, DWORD& bytesWritten, std::uint64_t filePosition, Duration timeout) noexcept
    {
        WaitableAsyncFileIoRequest_s asyncIoRequest;
        asyncIoRequest.SetOffset(filePosition);

        auto error = WaitForCompletion(WriteAsync(buffer, bytesToWrite, asyncIoRequest), asyncIoRequest, timeout);

        if (Windows::Error::Succeeded(error)) {
            bytesWritten = asyncIoRequest.GetTransferredBytes();
        } else {
            bytesWritten = 0;
        }

        return error;
    }

    inline AsyncFile::ErrorType AsyncFile::ReadFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition,
                                                    DWORD flagsAndAttributes, Memory::IWriteSequentialMemory* writeSequentialMemory,
                                                    const BulkIoOptions& options) noexcept
    {
        using Windows::Error;

        AsyncFile asyncFile;
        FileSize  size  = 0;
        ErrorType error = asyncFile.Create(fileName, access, shareMode, creationDisposition, flagsAndAttributes);
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

        const DWORD       blockSize = options.blockSize > 0 ? options.blockSize : BulkIoOptions::DefaultBlockSize;
        const std::size_t inFlight  = std::clamp<std::size_t>(options.blocksInFlight, 1, BulkIoOptions::MaxBlocksInFlight);
        std::unique_ptr<WaitableAsyncFileIoRequest_s[]> requests(new (std::nothrow) WaitableAsyncFileIoRequest_s[inFlight]);
        if (!requests) {
            writeSequentialMemory->SetEndOfStream();
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        // requests[] is a ring: request n uses slot n % inFlight; issued - released are in flight
        std::uint64_t offset   = begin;
        std::size_t   issued   = 0;
        std::size_t   released = 0;
        // the oldest request has finished: its block goes to the memory, unless an error has
        // ended the stream (then only the requests in flight are waited for)
        const auto release = [&]() noexcept {
            auto& request = requests[released % inFlight];
            if (Error::Succeeded(error) && Error::Failed(request.GetError()))
                error = request.GetError();
            if (Error::Succeeded(error))
                writeSequentialMemory->ReleaseWriteMemory(request.GetTransferredBytes());
            ++released;
        };

        while (released < issued || (Error::Succeeded(error) && offset < size)) {
            // finished blocks first: the memory may wait for room in RequestWriteMemory
            while (released < issued && requests[released % inFlight].TryWait())
                release();

            if (Error::Succeeded(error) && offset < size && issued - released < inFlight) {
                const auto bytes  = static_cast<DWORD>(std::min<std::uint64_t>(blockSize, size - offset));
                std::byte* memory = writeSequentialMemory->RequestWriteMemory(bytes);
                if (memory == nullptr) {
                    error = ERROR_NOT_ENOUGH_MEMORY;
                    continue;
                }
                auto& request = requests[issued % inFlight];
                request.Reset();
                request.SetOffset(offset);
                const ErrorType result = asyncFile.ReadAsync(memory, bytes, request);
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

    inline AsyncFile::ErrorType AsyncFile::WriteFile(FileName fileName, DWORD access, DWORD shareMode, DWORD creationDisposition,
                                                     DWORD flagsAndAttributes, Memory::IReadSequentialMemory* readSequentialMemory,
                                                     const BulkIoOptions& options) noexcept
    {
        using Windows::Error;

        AsyncFile asyncFile;
        ErrorType error = asyncFile.Create(fileName, access, shareMode, creationDisposition, flagsAndAttributes);
        if (Error::Failed(error))
            return error;

        const DWORD       blockSize = options.blockSize > 0 ? options.blockSize : BulkIoOptions::DefaultBlockSize;
        const std::size_t inFlight  = std::clamp<std::size_t>(options.blocksInFlight, 1, BulkIoOptions::MaxBlocksInFlight);
        std::unique_ptr<WaitableAsyncFileIoRequest_s[]> requests(new (std::nothrow) WaitableAsyncFileIoRequest_s[inFlight]);
        if (!requests)
            return ERROR_NOT_ENOUGH_MEMORY;

        std::uint64_t offset      = 0;
        std::size_t   issued      = 0;
        std::size_t   released    = 0;
        bool          endOfStream = false;
        // the oldest request has finished: its block goes back to the memory
        const auto release = [&]() noexcept {
            auto& request = requests[released % inFlight];
            if (Error::Succeeded(error) && Error::Failed(request.GetError()))
                error = request.GetError();
            readSequentialMemory->ReleaseReadMemory(request.GetTransferredBytes());
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
                auto& request = requests[issued % inFlight];
                request.Reset();
                request.SetOffset(offset);
                const ErrorType result = asyncFile.WriteAsync(data, static_cast<DWORD>(bytes), request);
                if (Error::Failed(result) && !Error::IsPending(result)) {
                    readSequentialMemory->ReleaseReadMemory(0);
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
        return error;
    }
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo