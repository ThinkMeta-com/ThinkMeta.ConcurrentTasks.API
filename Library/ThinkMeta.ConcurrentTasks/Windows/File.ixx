//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Windows::File: a thin owning wrapper of a Win32 file handle. Every function maps to one Win32
// call and returns its error as a Win32 error code; nothing suspends a task or knows about the
// scheduler. AsyncIo::AsyncFile builds the overlapped I/O on top of it.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <Windows.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.File;

export import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Owns one Win32 file handle and closes it on destruction.</summary>
    /// <remarks>
    /// Holds <c>INVALID_HANDLE_VALUE</c> when no file is open.
    /// All members report failures as a Win32 error code (<see cref="ErrorCode"/>, <c>ERROR_SUCCESS</c> on success)
    /// taken from <c>GetLastError</c> right after the call. Members that use the handle require one to be
    /// held (asserted in Debug builds). Plain blocking Win32 calls: called from a
    /// task they block the scheduler thread, except for overlapped operations on a handle opened
    /// with <c>FILE_FLAG_OVERLAPPED</c>, which return <c>ERROR_IO_PENDING</c>. Static members take a path and
    /// need no open handle. No member is synchronized; one object is used by one caller at a time.
    /// </remarks>
    /// <seealso cref="AsyncIo::AsyncFile"/>
    export class File
    {
        File(const File&)            = delete;
        File& operator=(const File&) = delete;

    public:
        /// <summary>Null-terminated wide path (the W variants of the Win32 calls are used).</summary>
        using FileName = const wchar_t*;

        /// <summary>Caller-owned output buffer for a path.</summary>
        using PathBuffer = wchar_t*;

        /// <summary>File size or offset in bytes.</summary>
        using FileSize = unsigned long long;

        /// <summary>Signed file pointer distance in bytes.</summary>
        using Distance = long long;

        /// <summary>Takes ownership of a handle.</summary>
        /// <param name="handle">The handle to own; default: none.</param>
        explicit File(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}

        /// <summary>Takes over another object's handle.</summary>
        /// <param name="other">The object to take the handle from; holds no handle afterwards.</param>
        /// <remarks><paramref name="other"/> is left without one.</remarks>
        File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }

        /// <summary>Closes the handle.</summary>
        /// <remarks>A failure is ignored.</remarks>
        ~File() noexcept { Close(); }

        /// <summary>Takes over another object's handle.</summary>
        /// <param name="other">The object to take the handle from; holds no handle afterwards.</param>
        /// <returns>This object.</returns>
        /// <remarks>
        /// Closes the own handle first (ignoring a failure); <paramref name="other"/> is left without one. A self-move
        /// changes nothing.
        /// </remarks>
        File& operator=(File&& other) noexcept
        {
            if (this != &other) {
                Close();

                handle_       = other.handle_;
                other.handle_ = INVALID_HANDLE_VALUE;
            }
            return *this;
        }

        /// <summary><c>true</c> if a handle is held.</summary>
        /// <returns><c>true</c> if a handle is held; <c>false</c> if the object holds <c>INVALID_HANDLE_VALUE</c>.</returns>
        bool IsValid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

        /// <summary>The handle, still owned by this object.</summary>
        /// <returns>The handle, or <c>INVALID_HANDLE_VALUE</c> if none is held; do not close it.</returns>
        HANDLE GetHandle() const noexcept { return handle_; }

        /// <summary><c>GetFileSizeEx</c>.</summary>
        /// <param name="size">Receives the file size in bytes; unchanged on failure.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetSize(FileSize& size) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileSizeEx(handle_, reinterpret_cast<LARGE_INTEGER*>(&size)));
        }

        /// <summary><c>GetFileType</c> (<c>FILE_TYPE_DISK</c>, <c>FILE_TYPE_PIPE</c>, ...).</summary>
        /// <param name="type">
        /// Receives the <c>FILE_TYPE_*</c> value; <c>FILE_TYPE_UNKNOWN</c> on failure or for an unknown type.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> for a known type; otherwise the <c>GetLastError</c> code, which is also
        /// <c>ERROR_SUCCESS</c> if the type is simply unknown.
        /// </returns>
        ErrorCode GetType(DWORD& type) const noexcept
        {
            CT_ASSERT(IsValid());

            type = ::GetFileType(handle_);

            return type != FILE_TYPE_UNKNOWN ? Error::Success : Error::GetLastError();
        }

        /// <summary>Closes the handle.</summary>
        /// <returns><c>ERROR_SUCCESS</c> if the handle was closed or none was held; otherwise the <c>CloseHandle</c> error.</returns>
        /// <remarks>Success if none is held. On failure the handle is kept.</remarks>
        /// <seealso cref="Create"/>
        ErrorCode Close() noexcept
        {
            ErrorCode error;

            if (IsValid()) {
                error = Error::GetError(CloseHandle(handle_));
                if (Error::Succeeded(error))
                    handle_ = INVALID_HANDLE_VALUE;
            } else
                error = Error::Success;

            return error;
        }

        /// <summary>Opens or creates a file (<c>CreateFileW</c>).</summary>
        /// <param name="fileName">Path of the file, device or pipe; must not be <c>nullptr</c>.</param>
        /// <param name="desiredAccess">
        /// Requested access: <c>GENERIC_READ</c>, <c>GENERIC_WRITE</c>, both, specific rights such as <c>DELETE</c>, or
        /// <c>0</c> to query attributes only.
        /// </param>
        /// <param name="shareMode">
        /// What other opens may do meanwhile: <c>0</c> (exclusive) or a combination of <c>FILE_SHARE_READ</c>,
        /// <c>FILE_SHARE_WRITE</c> and <c>FILE_SHARE_DELETE</c>.
        /// </param>
        /// <param name="securityAttributes">Security descriptor and handle inheritance; <c>nullptr</c> for the defaults.</param>
        /// <param name="creationDisposition">
        /// <c>CREATE_NEW</c>, <c>CREATE_ALWAYS</c>, <c>OPEN_EXISTING</c>, <c>OPEN_ALWAYS</c> or <c>TRUNCATE_EXISTING</c>.
        /// </param>
        /// <param name="flagsAndAttributes">
        /// <c>FILE_ATTRIBUTE_*</c> and <c>FILE_FLAG_*</c> values; <c>FILE_FLAG_OVERLAPPED</c> is required for
        /// <see cref="AsyncIo::AsyncFile"/>, <c>FILE_FLAG_NO_BUFFERING</c> requires sector-aligned buffers and sizes.
        /// </param>
        /// <param name="templateFile">
        /// Handle opened with <c>GENERIC_READ</c> whose attributes a new file takes over; usually <c>nullptr</c>.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>
        /// The object then owns the handle. Precondition: no handle is held. On failure the object holds no handle
        /// and the <c>GetLastError</c> code is returned.
        /// </remarks>
        /// <seealso cref="Close"/>
        ErrorCode Create(const FileName& fileName, DWORD desiredAccess, DWORD shareMode, SECURITY_ATTRIBUTES* securityAttributes, DWORD creationDisposition,
                         DWORD flagsAndAttributes, HANDLE templateFile) noexcept
        {
            CT_ASSERT(!IsValid());

            handle_ = ::CreateFile(fileName, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            return IsValid() ? Error::Success : Error::GetLastError();
        }

        /// <summary><c>DeleteFileW</c>.</summary>
        /// <param name="fileName">Path of the file to delete; must not be <c>nullptr</c>.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode Delete(const FileName& fileName) noexcept { return Error::GetError(::DeleteFile(fileName)); }

        /// <summary><c>SetEndOfFile</c> at the current file pointer.</summary>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Not meaningful for overlapped handles.</remarks>
        /// <seealso cref="SetEofInformation"/>
        ErrorCode SetEof() const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetEndOfFile(handle_));
        }

        /// <summary><c>SetFilePointerEx</c>.</summary>
        /// <param name="distanceToMove">Bytes to move the file pointer; negative values move it backwards.</param>
        /// <param name="newFilePointer">Receives the new position; may be <c>nullptr</c>.</param>
        /// <param name="moveMethod">
        /// Starting point of the move: <c>FILE_BEGIN</c>, <c>FILE_CURRENT</c> or <c>FILE_END</c>.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode SetFilePointer(const Distance& distanceToMove, Distance* newFilePointer, DWORD moveMethod) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFilePointerEx(handle_, reinterpret_cast<const LARGE_INTEGER&>(distanceToMove),
                                                      reinterpret_cast<LARGE_INTEGER*>(newFilePointer), moveMethod));
        }

        /// <summary><c>ReadFile</c>.</summary>
        /// <param name="buffer">Receives the data; at least <paramref name="bytesToRead"/> bytes.</param>
        /// <param name="bytesToRead">Maximum number of bytes to read.</param>
        /// <param name="bytesRead">
        /// Receives the number of bytes read (<c>0</c> at end of file); may be <c>nullptr</c> only with
        /// <paramref name="overlapped"/>.
        /// </param>
        /// <param name="overlapped">
        /// File offset and completion state for an overlapped handle; <c>nullptr</c> for a synchronous read at the
        /// file pointer.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the read completed, <c>ERROR_IO_PENDING</c> if it is still running (overlapped), or
        /// the Win32 error code (e.g. <c>ERROR_HANDLE_EOF</c>).
        /// </returns>
        /// <remarks>
        /// With <paramref name="overlapped"/> (handle opened with <c>FILE_FLAG_OVERLAPPED</c>) the result may be
        /// <c>ERROR_IO_PENDING</c>: <paramref name="buffer"/> and <c>OVERLAPPED</c> must then stay valid until the
        /// completion, and *<paramref name="bytesRead"/> is only meaningful when <c>ERROR_SUCCESS</c> is returned.
        /// </remarks>
        /// <seealso cref="Write"/>
        /// <seealso cref="AsyncIo::AsyncFile::ReadAsync"/>
        ErrorCode Read(void* buffer, DWORD bytesToRead, DWORD* bytesRead, OVERLAPPED* overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::ReadFile(handle_, buffer, bytesToRead, bytesRead, overlapped));
        }

        /// <summary><c>ReadFileEx</c>.</summary>
        /// <param name="buffer">
        /// Receives the data; at least <paramref name="bytesToRead"/> bytes, valid until the completion routine ran.
        /// </param>
        /// <param name="bytesToRead">Maximum number of bytes to read.</param>
        /// <param name="overlapped">
        /// File offset of the read; must not be <c>nullptr</c> and must stay valid until the completion routine ran.
        /// </param>
        /// <param name="completionRoutine">Runs as an APC in an alertable wait of the calling thread.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the read was queued (the result arrives in the completion routine); otherwise the
        /// Win32 error code.
        /// </returns>
        /// <remarks>Not usable on a handle associated with an I/O completion port (<see cref="AsyncIo::AsyncFile"/>).</remarks>
        ErrorCode Read(void* buffer, DWORD bytesToRead, OVERLAPPED* overlapped, LPOVERLAPPED_COMPLETION_ROUTINE completionRoutine) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::ReadFileEx(handle_, buffer, bytesToRead, overlapped, completionRoutine));
        }

        /// <summary><c>ReadFileScatter</c>.</summary>
        /// <param name="segments">
        /// Array of page-sized, page-aligned buffers, terminated by a <c>nullptr</c> element; valid until the
        /// completion.
        /// </param>
        /// <param name="bytesToRead">Total bytes to read; a multiple of the volume sector size.</param>
        /// <param name="overlapped">File offset and completion state; must stay valid until the completion.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the read completed, <c>ERROR_IO_PENDING</c> if it is still running, or the Win32
        /// error code.
        /// </returns>
        /// <remarks>Overlapped, unbuffered handles only; page-sized segments.</remarks>
        ErrorCode Read(FILE_SEGMENT_ELEMENT* segments, DWORD bytesToRead, OVERLAPPED& overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::ReadFileScatter(handle_, segments, bytesToRead, nullptr, &overlapped));
        }

        /// <summary><c>CancelIo</c>: cancels the calling thread's pending operations.</summary>
        /// <returns><c>ERROR_SUCCESS</c> if the cancellation was requested; otherwise the Win32 error code.</returns>
        ErrorCode CancelIo() const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::CancelIo(handle_));
        }

        /// <summary><c>CancelIoEx</c> for one operation, from any thread.</summary>
        /// <param name="overlapped">The <c>OVERLAPPED</c> of the pending operation to cancel.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the cancellation was requested, <c>ERROR_NOT_FOUND</c> if nothing was pending, or
        /// another Win32 error code.
        /// </returns>
        /// <remarks>
        /// The operation still completes (usually with <c>ERROR_OPERATION_ABORTED</c>); its <c>OVERLAPPED</c> and
        /// buffer must stay valid until then. <c>ERROR_NOT_FOUND</c> if nothing was pending.
        /// </remarks>
        ErrorCode CancelIo(OVERLAPPED& overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::CancelIoEx(handle_, &overlapped));
        }

        /// <summary><c>FlushFileBuffers</c>; blocks until the data is on the device.</summary>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode Flush() const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::FlushFileBuffers(handle_));
        }

        /// <summary><c>CreateHardLinkW</c>.</summary>
        /// <param name="fileName">Becomes a new name of <paramref name="existingFileName"/>.</param>
        /// <param name="existingFileName">Path of an existing file on the same volume.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode CreateHardLink(const FileName& fileName, const FileName& existingFileName) noexcept
        {
            return Error::GetError(::CreateHardLink(fileName, existingFileName, nullptr));
        }

        /// <summary><c>CreateSymbolicLinkW</c>.</summary>
        /// <param name="symlinkFileName">Path of the link to create.</param>
        /// <param name="targetFileName">Path the link points to; may be relative and need not exist.</param>
        /// <param name="flags">
        /// <c>0</c> for a file, <c>SYMBOLIC_LINK_FLAG_DIRECTORY</c> for a directory, optionally combined with
        /// <c>SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE</c>.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode CreateSymbolicLink(const FileName& symlinkFileName, const FileName& targetFileName, DWORD flags) noexcept
        {
            return Error::GetError(::CreateSymbolicLink(symlinkFileName, targetFileName, flags));
        }

        /// <summary><c>DecryptFileW</c> (EFS).</summary>
        /// <param name="fileName">Path of the encrypted file or directory.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="Encrypt"/>
        static ErrorCode Decrypt(const FileName& fileName) noexcept { return Error::GetError(::DecryptFile(fileName, 0)); }

        /// <summary><c>EncryptFileW</c> (EFS).</summary>
        /// <param name="fileName">Path of the file or directory to encrypt; a file must not be open elsewhere.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="Decrypt"/>
        static ErrorCode Encrypt(const FileName& fileName) noexcept { return Error::GetError(::EncryptFile(fileName)); }

        /// <summary><c>FileEncryptionStatusW</c>.</summary>
        /// <param name="fileName">Path of the file or directory.</param>
        /// <param name="status">Receives a <c>FILE_*</c> status such as <c>FILE_ENCRYPTABLE</c> or <c>FILE_IS_ENCRYPTED</c>.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode EncryptionStatus(const FileName& fileName, DWORD& status) noexcept
        {
            return Error::GetError(::FileEncryptionStatus(fileName, &status));
        }

        /// <summary><c>GetBinaryTypeW</c> (<c>SCS_*</c> value).</summary>
        /// <param name="fileName">Path of the file to examine.</param>
        /// <param name="binaryType">Receives the executable type, e.g. <c>SCS_64BIT_BINARY</c>.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> for an executable; otherwise the Win32 error code (also for a file that is not
        /// executable).
        /// </returns>
        static ErrorCode GetBinaryType(const FileName& fileName, DWORD& binaryType) noexcept { return Error::GetError(::GetBinaryType(fileName, &binaryType)); }

        /// <summary><c>GetCompressedFileSizeW</c>: bytes the file occupies on disk.</summary>
        /// <param name="fileName">Path of the file.</param>
        /// <param name="fileSize">Receives the size on disk in bytes; unchanged on failure.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode GetCompressedSize(const FileName& fileName, FileSize& fileSize) noexcept
        {
            DWORD high = 0;
            DWORD low  = ::GetCompressedFileSize(fileName, &high);
            if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
                fileSize = (static_cast<FileSize>(high) << 32) | low;
                return Error::Success;
            }
            return Error::GetLastError();
        }

        /// <summary><c>GetFileAttributesW</c>.</summary>
        /// <param name="fileName">Path of the file or directory.</param>
        /// <param name="attributes">
        /// Receives the <c>FILE_ATTRIBUTE_*</c> flags; <c>INVALID_FILE_ATTRIBUTES</c> on failure.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="SetAttributes"/>
        static ErrorCode GetAttributes(const FileName& fileName, DWORD& attributes) noexcept
        {
            attributes = ::GetFileAttributes(fileName);

            return attributes != INVALID_FILE_ATTRIBUTES ? Error::Success : Error::GetLastError();
        }

        /// <summary><c>GetFileAttributesExW</c> (attributes, times, size).</summary>
        /// <param name="fileName">Path of the file or directory.</param>
        /// <param name="attributes">Receives the attributes, times and size.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode GetAttributes(const FileName& fileName, WIN32_FILE_ATTRIBUTE_DATA& attributes) noexcept
        {
            return Error::GetError(::GetFileAttributesEx(fileName, GetFileExInfoStandard, &attributes));
        }

        /// <summary><c>GetFileBandwidthReservation</c>.</summary>
        /// <param name="periodMilliseconds">Receives the reservation period in milliseconds.</param>
        /// <param name="bytesPerPeriod">Receives the bytes reserved per period.</param>
        /// <param name="discardable">Receives whether unused bandwidth of a period is discarded.</param>
        /// <param name="transferSize">Receives the recommended transfer size in bytes.</param>
        /// <param name="numOutstandingRequests">Receives the number of requests to keep outstanding.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="SetBandwidthReservation"/>
        ErrorCode GetBandwidthReservation(DWORD& periodMilliseconds, DWORD& bytesPerPeriod, BOOL& discardable, DWORD& transferSize,
                                          DWORD& numOutstandingRequests) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(
                ::GetFileBandwidthReservation(handle_, &periodMilliseconds, &bytesPerPeriod, &discardable, &transferSize, &numOutstandingRequests));
        }

        /// <summary><c>SetFileBandwidthReservation</c>.</summary>
        /// <param name="periodMilliseconds">Length of the reservation period in milliseconds.</param>
        /// <param name="bytesPerPeriod">Bytes to reserve per period; <c>0</c> removes the reservation.</param>
        /// <param name="discardable">
        /// <c>TRUE</c> if bandwidth not used in a period may be discarded; <c>FALSE</c> to carry it over.
        /// </param>
        /// <param name="transferSize">Receives the transfer size to use.</param>
        /// <param name="numOutstandingRequests">Receives the number of requests to keep outstanding.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="GetBandwidthReservation"/>
        ErrorCode SetBandwidthReservation(DWORD periodMilliseconds, DWORD bytesPerPeriod, BOOL discardable, DWORD& transferSize,
                                          DWORD& numOutstandingRequests) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(
                ::SetFileBandwidthReservation(handle_, periodMilliseconds, bytesPerPeriod, discardable, &transferSize, &numOutstandingRequests));
        }

        /// <summary><c>GetFileInformationByHandle</c>.</summary>
        /// <param name="information">Receives attributes, times, volume serial number, size, link count and file index.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetInformation(BY_HANDLE_FILE_INFORMATION& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandle(handle_, &information));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileBasicInfo)</c>.</summary>
        /// <param name="information">Receives the creation, access, write and change times and the attributes.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="SetBasicInformation"/>
        ErrorCode GetBasicInformation(FILE_BASIC_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileBasicInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileStandardInfo)</c>.</summary>
        /// <param name="information">
        /// Receives allocation size, end of file, link count, delete-pending state and whether it is a directory.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetStandardInformation(FILE_STANDARD_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileStandardInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileNameInfo)</c>.</summary>
        /// <param name="information">Receives the name length in bytes and the path without the volume.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_MORE_DATA</c> if the name does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetNameInformation(FILE_NAME_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileNameInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileStreamInfo)</c>.</summary>
        /// <param name="information">Receives the first stream entry: name, size and allocation size.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_MORE_DATA</c> if the data does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetStreamInformation(FILE_STREAM_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileStreamInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileCompressionInfo)</c>.</summary>
        /// <param name="information">Receives the compressed size and the compression format and parameters.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetCompressionInformation(FILE_COMPRESSION_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileCompressionInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileAttributeTagInfo)</c>.</summary>
        /// <param name="information">Receives the attributes and the reparse tag.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetAttributeTagInformation(FILE_ATTRIBUTE_TAG_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileAttributeTagInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileIdBothDirectoryInfo)</c>.</summary>
        /// <param name="information">
        /// Receives the next directory entry (handle opened on a directory) with long and short name and file id.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_NO_MORE_FILES</c> after the last entry, <c>ERROR_MORE_DATA</c> if the entry
        /// does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetIdBothDirectoryInformation(FILE_ID_BOTH_DIR_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileIdBothDirectoryInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileIdBothDirectoryRestartInfo)</c>.</summary>
        /// <param name="information">
        /// Receives the first directory entry again (enumeration restarts), with long and short name and file id.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_MORE_DATA</c> if the entry does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetIdBothDirectoryRestartInformation(FILE_ID_BOTH_DIR_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileIdBothDirectoryRestartInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileRemoteProtocolInfo)</c>.</summary>
        /// <param name="information">Receives the network protocol (e.g. SMB) and its version for a remote file.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetRemoteProtocolInformation(FILE_REMOTE_PROTOCOL_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileRemoteProtocolInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileFullDirectoryInfo)</c>.</summary>
        /// <param name="information">
        /// Receives the next directory entry (handle opened on a directory) with name, times, sizes and attributes.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_NO_MORE_FILES</c> after the last entry, <c>ERROR_MORE_DATA</c> if the entry
        /// does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetFullDirectoryInformation(FILE_FULL_DIR_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileFullDirectoryInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileFullDirectoryRestartInfo)</c>.</summary>
        /// <param name="information">
        /// Receives the first directory entry again (enumeration restarts), with name, times, sizes and attributes.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_MORE_DATA</c> if the entry does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetFullDirectoryRestartInformation(FILE_FULL_DIR_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileFullDirectoryRestartInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileStorageInfo)</c>.</summary>
        /// <param name="information">Receives the logical and physical sector sizes and alignment of the volume.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetStorageInformation(FILE_STORAGE_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileStorageInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileAlignmentInfo)</c>.</summary>
        /// <param name="information">Receives the buffer alignment the device requires for unbuffered I/O.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetAlignmentInformation(FILE_ALIGNMENT_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileAlignmentInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileIdInfo)</c>.</summary>
        /// <param name="information">Receives the volume serial number and the 128-bit file id.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode GetIdInformation(FILE_ID_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileIdInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileIdExtdDirectoryInfo)</c>.</summary>
        /// <param name="information">
        /// Receives the next directory entry (handle opened on a directory) with name and 128-bit file id.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_NO_MORE_FILES</c> after the last entry, <c>ERROR_MORE_DATA</c> if the entry
        /// does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetIdExtendedDirectoryInformation(FILE_ID_EXTD_DIR_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileIdExtdDirectoryInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFileInformationByHandleEx(FileIdExtdDirectoryRestartInfo)</c>.</summary>
        /// <param name="information">
        /// Receives the first directory entry again (enumeration restarts), with name and 128-bit file id.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_MORE_DATA</c> if the entry does not fit, or another Win32 error code.
        /// </returns>
        /// <remarks>Passes <c>sizeof(information)</c> only: variable-length names that do not fit fail with <c>ERROR_MORE_DATA</c>.</remarks>
        ErrorCode GetIdExtendedDirectoryRestartInformation(FILE_ID_EXTD_DIR_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetFileInformationByHandleEx(handle_, FileIdExtdDirectoryRestartInfo, &information, sizeof(information)));
        }

        /// <summary><c>GetFinalPathNameByHandleW</c>.</summary>
        /// <param name="filePath">Receives the null-terminated path; <paramref name="filePathLength"/> characters.</param>
        /// <param name="filePathLength">
        /// In: buffer size in characters. Out: characters written, or the size needed (including
        /// the terminator) if the buffer is too small.
        /// </param>
        /// <param name="flags">
        /// <c>FILE_NAME_NORMALIZED</c> or <c>FILE_NAME_OPENED</c>, combined with a <c>VOLUME_NAME_*</c> value
        /// (<c>0</c> = normalized DOS path).
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c> (also for a too small buffer, see remarks); otherwise the Win32 error code.</returns>
        /// <remarks>
        /// A too small buffer returns success with <paramref name="filePathLength"/> larger than the buffer: compare
        /// before using the path.
        /// </remarks>
        ErrorCode GetFinalPath(PathBuffer filePath, DWORD& filePathLength, DWORD flags) const noexcept
        {
            CT_ASSERT(IsValid());

            filePathLength = ::GetFinalPathNameByHandle(handle_, filePath, filePathLength, flags);

            return filePathLength ? Error::Success : Error::GetLastError();
        }

        /// <summary><c>GetFullPathNameW</c>.</summary>
        /// <param name="fileName">Relative or absolute path to resolve against the current directory; need not exist.</param>
        /// <param name="fullPath">Receives the null-terminated full path; <paramref name="fullPathLength"/> characters.</param>
        /// <param name="fullPathLength">
        /// In: buffer size in characters. Out: characters written, or the size needed (including the terminator) if
        /// the buffer is too small.
        /// </param>
        /// <param name="filePart">
        /// Receives a pointer into <paramref name="fullPath"/> to the final name component; may be <c>nullptr</c>.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c> (also for a too small buffer); otherwise the Win32 error code.</returns>
        /// <remarks>
        /// <paramref name="fullPathLength"/> in/out as for <see cref="GetFinalPath"/> (a too small buffer returns
        /// success with the size needed).
        /// </remarks>
        static ErrorCode GetFullPath(FileName fileName, PathBuffer fullPath, DWORD& fullPathLength, PathBuffer* filePart) noexcept
        {
            fullPathLength = ::GetFullPathName(fileName, fullPathLength, fullPath, filePart);

            return fullPathLength ? Error::Success : Error::GetLastError();
        }

        /// <summary><c>GetLongPathNameW</c>.</summary>
        /// <param name="fileName">Existing path, possibly with 8.3 short name components.</param>
        /// <param name="longPath">Receives the null-terminated long path; <paramref name="longPathLength"/> characters.</param>
        /// <param name="longPathLength">
        /// In: buffer size in characters. Out: characters written, or the size needed if the buffer is too small.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c> (also for a too small buffer); otherwise the Win32 error code.</returns>
        /// <remarks><paramref name="longPathLength"/> in/out as for <see cref="GetFullPath"/>.</remarks>
        /// <seealso cref="GetShortPath"/>
        static ErrorCode GetLongPath(FileName fileName, PathBuffer longPath, DWORD& longPathLength) noexcept
        {
            longPathLength = ::GetLongPathName(fileName, longPath, longPathLength);

            return longPathLength ? Error::Success : Error::GetLastError();
        }

        /// <summary><c>GetShortPathNameW</c>.</summary>
        /// <param name="fileName">Existing path to convert.</param>
        /// <param name="shortPath">
        /// Receives the null-terminated 8.3 path; <paramref name="shortPathLength"/> characters.
        /// </param>
        /// <param name="shortPathLength">
        /// In: buffer size in characters. Out: characters written, or the size needed if the buffer is too small.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c> (also for a too small buffer); otherwise the Win32 error code.</returns>
        /// <remarks><paramref name="shortPathLength"/> in/out as for <see cref="GetFullPath"/>.</remarks>
        /// <seealso cref="GetLongPath"/>
        static ErrorCode GetShortPath(FileName fileName, PathBuffer shortPath, DWORD& shortPathLength) noexcept
        {
            shortPathLength = ::GetShortPathName(fileName, shortPath, shortPathLength);

            return shortPathLength ? Error::Success : Error::GetLastError();
        }

        /// <summary><c>LockFile</c>: locks a byte range exclusively.</summary>
        /// <param name="offset">Start of the range in bytes.</param>
        /// <param name="size">Length of the range in bytes; may extend beyond the end of the file.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_LOCK_VIOLATION</c> if the range is already locked, or another Win32 error
        /// code.
        /// </returns>
        /// <remarks>Fails at once if the range is locked.</remarks>
        /// <seealso cref="Unlock"/>
        ErrorCode Lock(const FileSize& offset, const FileSize& size) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::LockFile(handle_, static_cast<DWORD>(offset & 0xffffffff), static_cast<DWORD>(offset >> 32),
                                              static_cast<DWORD>(size & 0xffffffff), static_cast<DWORD>(size >> 32)));
        }

        /// <summary><c>LockFileEx</c>: locks a byte range.</summary>
        /// <param name="flags"><c>LOCKFILE_EXCLUSIVE_LOCK</c> / <c>LOCKFILE_FAIL_IMMEDIATELY</c>.</param>
        /// <param name="size">Length of the range in bytes; may extend beyond the end of the file.</param>
        /// <param name="overlapped">Carries the offset of the range.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the lock is held, <c>ERROR_IO_PENDING</c> if it is still being acquired
        /// (overlapped handle), <c>ERROR_LOCK_VIOLATION</c> with <c>LOCKFILE_FAIL_IMMEDIATELY</c>, or another Win32
        /// error code.
        /// </returns>
        /// <remarks>
        /// Without <c>LOCKFILE_FAIL_IMMEDIATELY</c> it blocks the thread, or returns <c>ERROR_IO_PENDING</c> on an
        /// overlapped handle.
        /// </remarks>
        /// <seealso cref="Unlock"/>
        ErrorCode Lock(DWORD flags, const FileSize& size, OVERLAPPED& overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::LockFileEx(handle_, flags, 0, static_cast<DWORD>(size & 0xffffffff), static_cast<DWORD>(size >> 32), &overlapped));
        }

        /// <summary><c>UnlockFile</c> of a range locked with <see cref="Lock"/>.</summary>
        /// <param name="offset">Start of the range in bytes; must match the locked range exactly.</param>
        /// <param name="size">Length of the range in bytes; must match the locked range exactly.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code (e.g. <c>ERROR_NOT_LOCKED</c>).</returns>
        ErrorCode Unlock(const FileSize& offset, const FileSize& size) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::UnlockFile(handle_, static_cast<DWORD>(offset & 0xffffffff), static_cast<DWORD>(offset >> 32),
                                                static_cast<DWORD>(size & 0xffffffff), static_cast<DWORD>(size >> 32)));
        }

        /// <summary><c>UnlockFileEx</c>: unlocks a byte range.</summary>
        /// <param name="size">Length of the range in bytes; must match the locked range exactly.</param>
        /// <param name="overlapped">Carries the offset of the range.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code (e.g. <c>ERROR_NOT_LOCKED</c>).</returns>
        /// <seealso cref="Lock"/>
        ErrorCode Unlock(const FileSize& size, OVERLAPPED& overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::UnlockFileEx(handle_, 0, static_cast<DWORD>(size & 0xffffffff), static_cast<DWORD>(size >> 32), &overlapped));
        }

        /// <summary><c>MoveFileW</c>.</summary>
        /// <param name="existingFileName">Current path of the file or directory.</param>
        /// <param name="newFileName">New path; must not exist.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode Move(const FileName& existingFileName, const FileName& newFileName) noexcept
        {
            return Error::GetError(::MoveFile(existingFileName, newFileName));
        }

        /// <summary><c>MoveFileExW</c> (<c>MOVEFILE_*</c> flags).</summary>
        /// <param name="existingFileName">Current path of the file or directory.</param>
        /// <param name="newFileName">
        /// New path; <c>nullptr</c> with <c>MOVEFILE_DELAY_UNTIL_REBOOT</c> deletes the file at the next boot.
        /// </param>
        /// <param name="flags">
        /// <c>MOVEFILE_*</c> values, e.g. <c>MOVEFILE_REPLACE_EXISTING</c>, <c>MOVEFILE_COPY_ALLOWED</c>,
        /// <c>MOVEFILE_WRITE_THROUGH</c>; <c>0</c> behaves like <see cref="Move"/> without flags.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        static ErrorCode Move(const FileName& existingFileName, const FileName& newFileName, DWORD flags) noexcept
        {
            return Error::GetError(::MoveFileEx(existingFileName, newFileName, flags));
        }

        /// <summary><c>MoveFileWithProgressW</c>.</summary>
        /// <param name="existingFileName">Current path of the file or directory.</param>
        /// <param name="newFileName">
        /// New path; <c>nullptr</c> with <c>MOVEFILE_DELAY_UNTIL_REBOOT</c> deletes the file at the next boot.
        /// </param>
        /// <param name="progressRoutine">May be <c>nullptr</c>; called on the calling thread with <paramref name="data"/>.</param>
        /// <param name="data">Passed unchanged to <paramref name="progressRoutine"/>; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MOVEFILE_*</c> values as for the <c>MoveFileExW</c> overload.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c>, <c>ERROR_REQUEST_ABORTED</c> if the progress routine cancelled, or another Win32
        /// error code.
        /// </returns>
        static ErrorCode Move(const FileName& existingFileName, const FileName& newFileName, LPPROGRESS_ROUTINE progressRoutine, void* data,
                              DWORD flags) noexcept
        {
            return Error::GetError(::MoveFileWithProgress(existingFileName, newFileName, progressRoutine, data, flags));
        }

        /// <summary><c>SetFileAttributesW</c>.</summary>
        /// <param name="fileName">Path of the file or directory.</param>
        /// <param name="attributes">
        /// New <c>FILE_ATTRIBUTE_*</c> flags (e.g. <c>FILE_ATTRIBUTE_READONLY</c>); replaces the settable ones,
        /// <c>FILE_ATTRIBUTE_NORMAL</c> clears them.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="GetAttributes"/>
        static ErrorCode SetAttributes(const FileName& fileName, DWORD attributes) noexcept
        {
            return Error::GetError(::SetFileAttributes(fileName, attributes));
        }

        /// <summary><c>SetFileCompletionNotificationModes</c>.</summary>
        /// <param name="flags">
        /// <c>FILE_SKIP_COMPLETION_PORT_ON_SUCCESS</c> and/or <c>FILE_SKIP_SET_EVENT_ON_HANDLE</c>; modes cannot be
        /// turned off again.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>
        /// Do not use on a handle associated through <see cref="AsyncIo::AssociateHandleWithIoCompletionPort"/>: the core
        /// already sets <c>FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE</c>, and
        /// <see cref="AsyncIo::AsyncFile"/> relies on it.
        /// </remarks>
        ErrorCode SetCompletionNotificationModes(UCHAR flags) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileCompletionNotificationModes(handle_, flags));
        }

        /// <summary><c>SetFileInformationByHandle(FileBasicInfo)</c>: times and attributes.</summary>
        /// <param name="information">
        /// New times and attributes; a time of <c>0</c> leaves it unchanged, <c>-1</c> stops its automatic update.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <seealso cref="GetBasicInformation"/>
        ErrorCode SetBasicInformation(FILE_BASIC_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileInformationByHandle(handle_, FileBasicInfo, &information, sizeof(information)));
        }

        /// <summary><c>SetFileInformationByHandle(FileRenameInfo)</c>.</summary>
        /// <param name="information">
        /// New name with its length in bytes, whether to replace an existing file and an optional root directory;
        /// the handle needs <c>DELETE</c> access.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Passes <c>sizeof(information)</c> only, so the new name must fit the structure itself.</remarks>
        ErrorCode SetRenameInformation(FILE_RENAME_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileInformationByHandle(handle_, FileRenameInfo, &information, sizeof(information)));
        }

        /// <summary><c>SetFileInformationByHandle(FileDispositionInfo)</c>: delete on close.</summary>
        /// <param name="information">
        /// <c>DeleteFile</c> member <c>TRUE</c> to delete the file when the last handle closes, <c>FALSE</c> to
        /// cancel that; the handle needs <c>DELETE</c> access.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode SetDispositionInformation(FILE_DISPOSITION_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileInformationByHandle(handle_, FileDispositionInfo, &information, sizeof(information)));
        }

        /// <summary><c>SetFileInformationByHandle(FileAllocationInfo)</c>.</summary>
        /// <param name="information">New allocation size in bytes; the file size is not changed by growing it.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode SetAllocationInformation(FILE_ALLOCATION_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileInformationByHandle(handle_, FileAllocationInfo, &information, sizeof(information)));
        }

        /// <summary><c>SetFileInformationByHandle(FileEndOfFileInfo)</c>: sets the size.</summary>
        /// <param name="information">New end of file in bytes; extending fills with zeros, shrinking truncates.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Works without a file pointer.</remarks>
        /// <seealso cref="SetEof"/>
        ErrorCode SetEofInformation(FILE_END_OF_FILE_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileInformationByHandle(handle_, FileEndOfFileInfo, &information, sizeof(information)));
        }

        /// <summary><c>SetFileInformationByHandle(FileIoPriorityHintInfo)</c>.</summary>
        /// <param name="information">
        /// I/O priority hint for this handle: <c>IoPriorityHintVeryLow</c>, <c>IoPriorityHintLow</c> or
        /// <c>IoPriorityHintNormal</c>.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        ErrorCode SetIoPriorityHintInformation(FILE_IO_PRIORITY_HINT_INFO& information) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileInformationByHandle(handle_, FileIoPriorityHintInfo, &information, sizeof(information)));
        }

        /// <summary><c>SetFileIoOverlappedRange</c>.</summary>
        /// <param name="overlappedRangeStart">
        /// Start of the memory that holds the <c>OVERLAPPED</c> structures used with this handle; must stay valid
        /// while the handle is open.
        /// </param>
        /// <param name="length">Size of that memory range in bytes.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Locks the <c>OVERLAPPED</c> range in memory for faster completion. Needs <c>SeLockMemoryPrivilege</c>.</remarks>
        ErrorCode SetIoOverlappedRange(UCHAR* overlappedRangeStart, ULONG length) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileIoOverlappedRange(handle_, overlappedRangeStart, length));
        }

        /// <summary><c>SetFileShortNameW</c>.</summary>
        /// <param name="shortName">New 8.3 short name (name only, no path).</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Needs <c>SE_RESTORE_NAME</c> and a handle opened with <c>DELETE</c> and <c>FILE_FLAG_BACKUP_SEMANTICS</c>.</remarks>
        ErrorCode SetShortName(const FileName& shortName) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileShortName(handle_, shortName));
        }

        /// <summary><c>SetFileValidData</c>: extends the valid data without zeroing it.</summary>
        /// <param name="validDataLength">
        /// New valid data length in bytes; larger than the current one and not beyond the allocated size.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Needs <c>SE_MANAGE_VOLUME_NAME</c>.</remarks>
        ErrorCode SetValidData(long long validDataLength) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::SetFileValidData(handle_, validDataLength));
        }

        /// <summary><c>WriteFile</c>.</summary>
        /// <param name="buffer">The data to write; at least <paramref name="bytesToWrite"/> bytes.</param>
        /// <param name="bytesToWrite">Number of bytes to write.</param>
        /// <param name="bytesWritten">
        /// Receives the number of bytes written; may be <c>nullptr</c> only with <paramref name="overlapped"/>.
        /// </param>
        /// <param name="overlapped">
        /// File offset and completion state for an overlapped handle; <c>nullptr</c> for a synchronous write at the
        /// file pointer.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the write completed, <c>ERROR_IO_PENDING</c> if it is still running (overlapped), or
        /// the Win32 error code.
        /// </returns>
        /// <remarks>
        /// With <paramref name="overlapped"/> the result may be <c>ERROR_IO_PENDING</c>: <paramref name="buffer"/> and
        /// <c>OVERLAPPED</c> must then stay valid until the completion, and *<paramref name="bytesWritten"/> is only
        /// meaningful on <c>ERROR_SUCCESS</c>.
        /// </remarks>
        /// <seealso cref="Read"/>
        /// <seealso cref="AsyncIo::AsyncFile::WriteAsync"/>
        ErrorCode Write(const void* buffer, DWORD bytesToWrite, DWORD* bytesWritten, OVERLAPPED* overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::WriteFile(handle_, buffer, bytesToWrite, bytesWritten, overlapped));
        }

        /// <summary><c>WriteFileEx</c>.</summary>
        /// <param name="buffer">
        /// The data to write; at least <paramref name="bytesToWrite"/> bytes, valid until the completion routine ran.
        /// </param>
        /// <param name="bytesToWrite">Number of bytes to write.</param>
        /// <param name="overlapped">
        /// File offset of the write; must not be <c>nullptr</c> and must stay valid until the completion routine ran.
        /// </param>
        /// <param name="completionRoutine">Runs as an APC in an alertable wait of the calling thread.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the write was queued (the result arrives in the completion routine); otherwise the
        /// Win32 error code.
        /// </returns>
        /// <remarks>Not usable on a handle associated with an I/O completion port (<see cref="AsyncIo::AsyncFile"/>).</remarks>
        ErrorCode Write(const void* buffer, DWORD bytesToWrite, OVERLAPPED* overlapped, LPOVERLAPPED_COMPLETION_ROUTINE completionRoutine) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::WriteFileEx(handle_, buffer, bytesToWrite, overlapped, completionRoutine));
        }

        /// <summary><c>WriteFileGather</c>.</summary>
        /// <param name="segments">
        /// Array of page-sized, page-aligned buffers, terminated by a <c>nullptr</c> element; valid until the
        /// completion.
        /// </param>
        /// <param name="bytesToWrite">Total bytes to write; a multiple of the volume sector size.</param>
        /// <param name="overlapped">File offset and completion state; must stay valid until the completion.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if the write completed, <c>ERROR_IO_PENDING</c> if it is still running, or the Win32
        /// error code.
        /// </returns>
        /// <remarks>Overlapped, unbuffered handles only; page-sized segments.</remarks>
        /// <seealso cref="Read"/>
        ErrorCode Write(FILE_SEGMENT_ELEMENT* segments, DWORD bytesToWrite, OVERLAPPED& overlapped) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::WriteFileGather(handle_, segments, bytesToWrite, nullptr, &overlapped));
        }

    private:
        HANDLE handle_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows