//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Posix::File: a thin owning wrapper of a file descriptor, the counterpart of Windows::File with
// the part the asynchronous file classes need. Every function maps to one system call and returns
// its errno; nothing suspends a task. The asynchronous operations go through the core
// (AsyncIo::SubmitIo), not through this class.
//
// The file is not called File.ixx: file names stay unique across the library, since Visual Studio
// puts the objects of a project into one folder (MSB8027).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Posix.File;

export import ThinkMeta.ConcurrentTasks.Posix.Error;
export import ThinkMeta.ConcurrentTasks.Platform.FileOptions;

namespace ThinkMeta::ConcurrentTasks::Posix {
    /// <summary>Owns one file descriptor and closes it on destruction.</summary>
    /// <remarks>
    /// Holds <c>-1</c> when no file is open. All members report failures as an <c>errno</c>
    /// (<see cref="ErrorCode"/>, <see cref="Error::Success"/> on success). Members that use the
    /// descriptor require one to be held (asserted in Debug builds). Plain blocking calls: called
    /// from a task they block the scheduler thread. No member is synchronized; one object is used
    /// by one caller at a time.
    /// </remarks>
    /// <seealso cref="AsyncIo::AsyncFile"/>
    export class File
    {
        File(const File&)            = delete;
        File& operator=(const File&) = delete;

    public:
        /// <summary>Null-terminated path in UTF-8, as the system calls take it.</summary>
        using FileName = const char*;

        /// <summary>File size or offset in bytes.</summary>
        using FileSize = unsigned long long;

        /// <summary>Takes ownership of a descriptor.</summary>
        /// <param name="fd">The descriptor to own; default: none.</param>
        explicit File(int fd = -1) noexcept : fd_(fd) {}

        /// <summary>Takes over another object's descriptor.</summary>
        /// <param name="other">The object to take the descriptor from; holds none afterwards.</param>
        File(File&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

        /// <summary>Closes the descriptor.</summary>
        /// <remarks>A failure is ignored.</remarks>
        ~File() noexcept { Close(); }

        /// <summary>Takes over another object's descriptor.</summary>
        /// <param name="other">The object to take the descriptor from; holds none afterwards.</param>
        /// <returns>This object.</returns>
        /// <remarks>Closes the own descriptor first (ignoring a failure). A self-move changes nothing.</remarks>
        File& operator=(File&& other) noexcept
        {
            if (this != &other) {
                Close();

                fd_       = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        /// <summary><c>true</c> if a descriptor is held.</summary>
        /// <returns><c>true</c> if a descriptor is held; otherwise <c>false</c>.</returns>
        bool IsValid() const noexcept { return fd_ >= 0; }

        /// <summary>The descriptor, still owned by this object.</summary>
        /// <returns>The descriptor, or <c>-1</c> if none is held; do not close it.</returns>
        int GetHandle() const noexcept { return fd_; }

        /// <summary>Opens or creates a file (<c>open</c>, with <c>O_CLOEXEC</c>).</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="flags">The <c>O_*</c> flags.</param>
        /// <param name="mode">The permissions of a file that is created, before the umask.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>open</c>.</returns>
        /// <remarks>Precondition: no descriptor is held. On failure none is held.</remarks>
        ErrorCode Create(FileName fileName, int flags, mode_t mode = 0666) noexcept
        {
            CT_ASSERT(!IsValid());

            fd_ = open(fileName, flags | O_CLOEXEC, mode);
            return IsValid() ? Error::Success : Error::GetLastError();
        }

        /// <summary>Opens or creates a file as <paramref name="options"/> says.</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <param name="options">How to open it; the share modes have no meaning here.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>open</c>.</returns>
        /// <remarks>Precondition: no descriptor is held. On failure none is held.</remarks>
        ErrorCode Create(FileName fileName, const Platform::FileOptions& options) noexcept { return Create(fileName, FlagsOf(options)); }

        /// <summary>Closes the descriptor (<c>close</c>).</summary>
        /// <returns><see cref="Error::Success"/> if it was closed or none was held; otherwise the <c>errno</c> of <c>close</c>.</returns>
        /// <remarks>The object holds no descriptor afterwards, also when <c>close</c> failed: Linux has released it then.</remarks>
        ErrorCode Close() noexcept
        {
            if (!IsValid())
                return Error::Success;

            const bool closed = close(fd_) == 0;
            fd_               = -1;
            return Error::GetError(closed);
        }

        /// <summary>The size of the file (<c>fstat</c>).</summary>
        /// <param name="size">Receives the size in bytes; unchanged on failure.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>fstat</c>.</returns>
        ErrorCode GetSize(FileSize& size) const noexcept
        {
            CT_ASSERT(IsValid());

            struct stat status;
            if (fstat(fd_, &status) != 0)
                return Error::GetLastError();
            size = static_cast<FileSize>(status.st_size);
            return Error::Success;
        }

        /// <summary>Sets the size of the file (<c>ftruncate</c>).</summary>
        /// <param name="size">The new size in bytes; a larger file is cut, a smaller one grows with zeros.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>ftruncate</c>.</returns>
        ErrorCode SetEndOfFile(FileSize size) const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(ftruncate(fd_, static_cast<off_t>(size)) == 0);
        }

        /// <summary>Writes the file's data to the device (<c>fdatasync</c>); blocks until it is there.</summary>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>fdatasync</c>.</returns>
        /// <remarks>
        /// The data and what is needed to read it back (the size), not the times of last access and
        /// change; <c>FlushFileBuffers</c> on Windows writes those too. Cheaper, and all a reader needs.
        /// </remarks>
        ErrorCode Flush() const noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(fdatasync(fd_) == 0);
        }

        /// <summary>Writes at an offset and blocks until done (<c>pwrite</c>).</summary>
        /// <param name="buffer">The data.</param>
        /// <param name="bytes">Number of bytes to write.</param>
        /// <param name="offset">Where in the file.</param>
        /// <param name="written">Receives the number of bytes written; may be less than asked.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>pwrite</c>.</returns>
        ErrorCode WriteAt(const void* buffer, std::size_t bytes, FileSize offset, std::size_t& written) const noexcept
        {
            CT_ASSERT(IsValid());

            const ssize_t result = pwrite(fd_, buffer, bytes, static_cast<off_t>(offset));
            if (result < 0)
                return Error::GetLastError();
            written = static_cast<std::size_t>(result);
            return Error::Success;
        }

        /// <summary>Removes a file (<c>unlink</c>).</summary>
        /// <param name="fileName">Path of the file; must not be <c>nullptr</c>.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>unlink</c>.</returns>
        static ErrorCode Delete(FileName fileName) noexcept { return Error::GetError(unlink(fileName) == 0); }

        /// <summary>The <c>O_*</c> flags that <paramref name="options"/> stands for, without <c>O_CLOEXEC</c>.</summary>
        /// <param name="options">How to open a file.</param>
        /// <returns>The flags for <c>open</c>.</returns>
        static int FlagsOf(const Platform::FileOptions& options) noexcept
        {
            using Platform::FileAccess;
            using Platform::FileCreation;

            // Windows refuses to empty a file opened for reading only; Linux would do it: not portable
            CT_ASSERT(options.access != FileAccess::Read
                      || (options.creation != FileCreation::CreateAlways && options.creation != FileCreation::TruncateExisting));

            int flags = options.access == FileAccess::Read ? O_RDONLY : options.access == FileAccess::Write ? O_WRONLY : O_RDWR;

            switch (options.creation) {
                case FileCreation::OpenExisting:
                    break;
                case FileCreation::CreateAlways:
                    flags |= O_CREAT | O_TRUNC;
                    break;
                case FileCreation::CreateNew:
                    flags |= O_CREAT | O_EXCL;
                    break;
                case FileCreation::OpenAlways:
                    flags |= O_CREAT;
                    break;
                case FileCreation::TruncateExisting:
                    flags |= O_TRUNC;
                    break;
            }

            if (options.unbuffered)
                flags |= O_DIRECT;
            return flags;
        }

    private:
        int fd_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Posix
