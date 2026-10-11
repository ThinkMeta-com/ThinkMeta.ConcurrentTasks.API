//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Windows::IoCompletionPort: a thin owning wrapper of a Win32 I/O completion port handle. The
// core runs its own port (EnableAsyncIo, AssociateHandleWithIoCompletionPort in Core.h);
// applications normally use that one through AsyncFile and the sockets, not this class.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <Windows.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.IoCompletionPort;

export import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Owns one I/O completion port handle and closes it on destruction.</summary>
    /// <remarks>
    /// Holds <c>nullptr</c> when no port is open. Members report failures as a Win32 error code
    /// (<see cref="ErrorCode"/>, <c>ERROR_SUCCESS</c> on success). The Win32 port
    /// calls are thread-safe, so several threads may
    /// post and dequeue at once; <see cref="Create"/>, <see cref="Close"/> and moves must not race with them. The dequeue
    /// functions (<see cref="GetQueuedCompletionStatus"/>) block the calling OS thread (a task calling them blocks its
    /// scheduler thread). Applications normally use the core's port instead (<see cref="AsyncIo::EnableAsyncIo"/>).
    /// </remarks>
    /// <seealso cref="AsyncIo::AssociateHandleWithIoCompletionPort"/>
    export class IoCompletionPort
    {
        IoCompletionPort(const IoCompletionPort&)            = delete;
        IoCompletionPort& operator=(const IoCompletionPort&) = delete;

    public:
        /// <summary>Creates an object without a port.</summary>
        /// <remarks>Call <see cref="Create"/> to open one.</remarks>
        IoCompletionPort() noexcept = default;

        /// <summary>Takes over another object's port.</summary>
        /// <param name="other">The object to take the port from; holds no port afterwards.</param>
        /// <remarks><paramref name="other"/> is left without one.</remarks>
        IoCompletionPort(IoCompletionPort&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

        /// <summary>Closes the port.</summary>
        /// <remarks>A failure is ignored. Associated handles should be closed first.</remarks>
        ~IoCompletionPort() noexcept { Close(); }

        /// <summary>Takes over another object's port.</summary>
        /// <param name="other">The object to take the port from; holds no port afterwards.</param>
        /// <returns>This object.</returns>
        /// <remarks>
        /// Closes the own port first (ignoring a failure); <paramref name="other"/> is left without one. A self-move
        /// changes nothing.
        /// </remarks>
        IoCompletionPort& operator=(IoCompletionPort&& other) noexcept
        {
            if (this != &other) {
                Close();

                handle_       = other.handle_;
                other.handle_ = nullptr;
            }
            return *this;
        }

        /// <summary><c>true</c> if a port is held.</summary>
        /// <returns><c>true</c> if a port is held; <c>false</c> if the object holds <c>nullptr</c>.</returns>
        bool IsValid() const noexcept { return handle_ != nullptr; }

        /// <summary>Creates a new port.</summary>
        /// <param name="numberOfConcurrentThreads">Threads the port lets run at once; <c>0</c>: one per processor.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Precondition: none is held.</remarks>
        /// <seealso cref="Close"/>
        ErrorCode Create(DWORD numberOfConcurrentThreads) noexcept
        {
            CT_ASSERT(!IsValid());

            handle_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, numberOfConcurrentThreads);
            return IsValid() ? Error::Success : Error::GetLastError();
        }

        /// <summary>Associates a handle with the port.</summary>
        /// <param name="handle">A handle opened for overlapped I/O.</param>
        /// <param name="completionKey">Carried by every packet of the handle.</param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code (e.g. <c>ERROR_INVALID_PARAMETER</c> if already associated).</returns>
        /// <remarks>A handle can be associated with one port only, for its lifetime.</remarks>
        ErrorCode Associate(HANDLE handle, DWORD_PTR completionKey) noexcept
        {
            CT_ASSERT(IsValid());

            return ::CreateIoCompletionPort(handle, handle_, completionKey, 0) ? Error::Success : Error::GetLastError();
        }

        /// <summary>Closes the port.</summary>
        /// <returns><c>ERROR_SUCCESS</c> if the port was closed or none was held; otherwise the <c>CloseHandle</c> error.</returns>
        /// <remarks>Success if none is held. On failure the handle is kept.</remarks>
        /// <seealso cref="Create"/>
        ErrorCode Close() noexcept
        {
            ErrorCode error;

            if (IsValid()) {
                error = Error::GetError(CloseHandle(handle_));
                if (Error::Succeeded(error))
                    handle_ = nullptr;
            } else
                error = Error::Success;

            return error;
        }

        /// <summary>Dequeues one packet (<c>GetQueuedCompletionStatus</c>).</summary>
        /// <param name="numberOfBytes">Receives the number of bytes the dequeued operation transferred.</param>
        /// <param name="completionKey">Receives the completion key of the handle (or the posted key).</param>
        /// <param name="overlapped">
        /// Receives the <c>OVERLAPPED</c> of the dequeued operation; <c>nullptr</c> if no packet was dequeued.
        /// </param>
        /// <param name="milliseconds">How long to wait at most; <c>INFINITE</c>: no limit.</param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> for a packet of a successful operation; the operation's error for a failed one;
        /// <c>WAIT_TIMEOUT</c> if none arrived in time; otherwise the Win32 error code of the call.
        /// </returns>
        /// <remarks>
        /// Returns the packet's own error for a failed I/O operation; then <paramref name="overlapped"/> is non-null and
        /// <paramref name="numberOfBytes"/> and <paramref name="completionKey"/> are valid. <c>WAIT_TIMEOUT</c> with a
        /// <c>nullptr</c> overlapped means no
        /// packet arrived in time. The error is returned here because the thread's last error may
        /// be overwritten by any later call.
        /// </remarks>
        /// <seealso cref="PostQueuedCompletionStatus"/>
        ErrorCode GetQueuedCompletionStatus(DWORD& numberOfBytes, ULONG_PTR& completionKey, OVERLAPPED*& overlapped, DWORD milliseconds) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetQueuedCompletionStatus(handle_, &numberOfBytes, &completionKey, &overlapped, milliseconds));
        }

        /// <summary>Dequeues several packets at once (<c>GetQueuedCompletionStatusEx</c>).</summary>
        /// <param name="completionPortEntries">Receives the dequeued packets; at least <paramref name="count"/> entries.</param>
        /// <param name="count">Maximum number of packets to dequeue.</param>
        /// <param name="numEntriesRemoved">Receives the number of entries filled in.</param>
        /// <param name="milliseconds">How long to wait at most; <c>INFINITE</c>: no limit.</param>
        /// <param name="alertable">
        /// <c>TRUE</c> to let queued APCs run during the wait (it then ends with <c>WAIT_IO_COMPLETION</c>);
        /// <c>FALSE</c> otherwise.
        /// </param>
        /// <returns>
        /// <c>ERROR_SUCCESS</c> if at least one packet was dequeued, <c>WAIT_TIMEOUT</c>, <c>WAIT_IO_COMPLETION</c>, or
        /// another Win32 error code.
        /// </returns>
        /// <remarks>
        /// The per-packet result is not returned: read each entry's <c>Internal</c> (an <c>NTSTATUS</c>) and
        /// <c>dwNumberOfBytesTransferred</c>. <c>WAIT_TIMEOUT</c> if none arrived; <c>WAIT_IO_COMPLETION</c> if an APC
        /// ended an alertable wait.
        /// </remarks>
        /// <seealso cref="PostQueuedCompletionStatus"/>
        ErrorCode GetQueuedCompletionStatus(OVERLAPPED_ENTRY* completionPortEntries, ULONG count, ULONG& numEntriesRemoved, DWORD milliseconds,
                                            BOOL alertable) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::GetQueuedCompletionStatusEx(handle_, completionPortEntries, count, &numEntriesRemoved, milliseconds, alertable));
        }

        /// <summary>Posts a packet with the given values.</summary>
        /// <param name="numberOfBytesTransferred">Delivered to the dequeuer as the transferred byte count.</param>
        /// <param name="completionKey">Delivered to the dequeuer as the completion key.</param>
        /// <param name="overlapped">
        /// Delivered to the dequeuer as the <c>OVERLAPPED</c> pointer; may be <c>nullptr</c> or any value the
        /// dequeuer understands, it is not dereferenced by the system.
        /// </param>
        /// <returns><c>ERROR_SUCCESS</c>, or the Win32 error code of the failed call.</returns>
        /// <remarks>Any thread; never blocks.</remarks>
        /// <seealso cref="GetQueuedCompletionStatus"/>
        ErrorCode PostQueuedCompletionStatus(DWORD numberOfBytesTransferred, ULONG_PTR completionKey, OVERLAPPED* overlapped) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::PostQueuedCompletionStatus(handle_, numberOfBytesTransferred, completionKey, overlapped));
        }

    private:
        HANDLE handle_{nullptr};
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows