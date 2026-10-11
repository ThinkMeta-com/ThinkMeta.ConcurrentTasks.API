//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Thread: a thin owner of a Win32 thread handle with one member per thread API. Every call
// returns an ErrorCode (Error::Success or the Win32 error) instead of BOOL + GetLastError.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.Thread;

export import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Owns a Win32 thread handle and closes it on destruction.</summary>
    /// <remarks>
    /// Owning the handle does not control the thread: destroying the object leaves the thread
    /// running.
    /// Members report failures as <see cref="ErrorCode"/>. Members that act on the thread require a
    /// handle (asserted in debug builds). Use one object from one thread at a time; the thread APIs
    /// it wraps are thread-safe.
    /// </remarks>
    /// <seealso cref="ConvenienceThread"/>
    export class Thread
    {
        Thread(const Thread&)            = delete;
        Thread& operator=(const Thread&) = delete;

    public:
        /// <summary>Constructs an object without a handle.</summary>
        Thread() noexcept = default;

        /// <summary>Takes over the handle of <paramref name="other"/>.</summary>
        /// <param name="other">Left without a handle.</param>
        Thread(Thread&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

        /// <summary>Closes the handle, if any.</summary>
        /// <remarks>The thread itself keeps running.</remarks>
        ~Thread() noexcept { Close(); }

        /// <summary>Takes over the handle of <paramref name="other"/>.</summary>
        /// <param name="other">Left without a handle.</param>
        /// <returns>This object.</returns>
        /// <remarks>A handle this object held before is closed first.</remarks>
        Thread& operator=(Thread&& other) noexcept
        {
            if (this != &other) {
                Close();
                handle_       = other.handle_;
                other.handle_ = nullptr;
            }
            return *this;
        }

        /// <summary>Returns whether the object holds a handle.</summary>
        /// <returns><c>true</c> if a handle is held; otherwise <c>false</c>.</returns>
        bool IsValid() const noexcept { return handle_ != nullptr; }

        /// <summary>Opens an existing thread by id (<c>OpenThread</c>).</summary>
        /// <param name="desiredAccess">
        /// Access rights the handle gets, e.g. <c>THREAD_QUERY_LIMITED_INFORMATION</c> or
        /// <c>THREAD_ALL_ACCESS</c>.
        /// </param>
        /// <param name="inheritHandle"><c>TRUE</c> to let child processes inherit the handle.</param>
        /// <param name="threadId">Id of the thread to open.</param>
        /// <param name="thread">Receives the handle; a handle it held before is closed.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="Close"/>
        static ErrorCode Open(DWORD desiredAccess, BOOL inheritHandle, DWORD threadId, Thread& thread) noexcept
        {
            thread = Thread(::OpenThread(desiredAccess, inheritHandle, threadId));
            return thread.IsValid() ? Error::Success : Error::GetLastError();
        }

        /// <summary>Creates a thread in this process (<c>CreateThread</c>) and keeps its handle.</summary>
        /// <param name="threadAttributes">
        /// Security descriptor and inheritance of the handle; <c>nullptr</c>: default security, not
        /// inheritable.
        /// </param>
        /// <param name="stackSize">Initial stack size in bytes; <c>0</c>: the executable's default.</param>
        /// <param name="startAddress">The thread function; required.</param>
        /// <param name="parameter">Passed unchanged to <paramref name="startAddress"/>; may be <c>nullptr</c>.</param>
        /// <param name="creationFlags">
        /// <c>0</c> to run at once, <c>CREATE_SUSPENDED</c> to start suspended (see <see cref="Resume"/>),
        /// <c>STACK_SIZE_PARAM_IS_A_RESERVATION</c> to reserve rather than commit
        /// <paramref name="stackSize"/>.
        /// </param>
        /// <param name="threadId">Receives the new thread's id; may be <c>nullptr</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>
        /// The object must not hold a handle yet. The new thread runs
        /// <paramref name="startAddress"/> as a plain OS thread, not as a task.
        /// </remarks>
        /// <seealso cref="CreateRemote"/>
        /// <seealso cref="Close"/>
        ErrorCode Create(SECURITY_ATTRIBUTES* threadAttributes, SIZE_T stackSize, PTHREAD_START_ROUTINE startAddress, void* parameter, DWORD creationFlags,
                         DWORD* threadId) noexcept
        {
            CT_ASSERT(!IsValid());
            handle_ = CreateThread(threadAttributes, stackSize, startAddress, parameter, creationFlags, threadId);
            return Error::GetError(IsValid());
        }

        /// <summary>Creates a thread in another process (<c>CreateRemoteThread</c>).</summary>
        /// <param name="process">
        /// Handle of the target process with <c>PROCESS_CREATE_THREAD</c>, <c>PROCESS_QUERY_INFORMATION</c>,
        /// <c>PROCESS_VM_OPERATION</c>, <c>PROCESS_VM_WRITE</c> and <c>PROCESS_VM_READ</c> access; borrowed.
        /// </param>
        /// <param name="threadAttributes">
        /// Security descriptor and inheritance of the handle; <c>nullptr</c>: default security, not
        /// inheritable.
        /// </param>
        /// <param name="stackSize">Initial stack size in bytes; <c>0</c>: the executable's default.</param>
        /// <param name="startAddress">The thread function, an address in the target process; required.</param>
        /// <param name="parameter">Passed unchanged to <paramref name="startAddress"/>; may be <c>nullptr</c>.</param>
        /// <param name="creationFlags">
        /// <c>0</c> to run at once, <c>CREATE_SUSPENDED</c> to start suspended,
        /// <c>STACK_SIZE_PARAM_IS_A_RESERVATION</c> to reserve rather than commit
        /// <paramref name="stackSize"/>.
        /// </param>
        /// <param name="threadId">Receives the new thread's id; may be <c>nullptr</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>The object must not hold a handle yet.</remarks>
        /// <seealso cref="Create"/>
        ErrorCode CreateRemote(HANDLE process, SECURITY_ATTRIBUTES* threadAttributes, SIZE_T stackSize, PTHREAD_START_ROUTINE startAddress, void* parameter,
                               DWORD creationFlags, DWORD* threadId) noexcept
        {
            CT_ASSERT(!IsValid());
            handle_ = CreateRemoteThread(process, threadAttributes, stackSize, startAddress, parameter, creationFlags, threadId);
            return Error::GetError(IsValid());
        }

        /// <summary>Creates a thread in another process (<c>CreateRemoteThreadEx</c>).</summary>
        /// <param name="process">
        /// Handle of the target process with <c>PROCESS_CREATE_THREAD</c>, <c>PROCESS_QUERY_INFORMATION</c>,
        /// <c>PROCESS_VM_OPERATION</c>, <c>PROCESS_VM_WRITE</c> and <c>PROCESS_VM_READ</c> access; borrowed.
        /// </param>
        /// <param name="threadAttributes">
        /// Security descriptor and inheritance of the handle; <c>nullptr</c>: default security, not
        /// inheritable.
        /// </param>
        /// <param name="stackSize">Initial stack size in bytes; <c>0</c>: the executable's default.</param>
        /// <param name="startAddress">The thread function, an address in the target process; required.</param>
        /// <param name="parameter">Passed unchanged to <paramref name="startAddress"/>; may be <c>nullptr</c>.</param>
        /// <param name="creationFlags">
        /// <c>0</c> to run at once, <c>CREATE_SUSPENDED</c> to start suspended,
        /// <c>STACK_SIZE_PARAM_IS_A_RESERVATION</c> to reserve rather than commit
        /// <paramref name="stackSize"/>.
        /// </param>
        /// <param name="attributeList">
        /// Extended attributes of the new thread (<c>InitializeProcThreadAttributeList</c>); may be
        /// <c>nullptr</c>.
        /// </param>
        /// <param name="threadId">Receives the new thread's id; may be <c>nullptr</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>The object must not hold a handle yet.</remarks>
        ErrorCode CreateRemote(HANDLE process, SECURITY_ATTRIBUTES* threadAttributes, SIZE_T stackSize, PTHREAD_START_ROUTINE startAddress, void* parameter,
                               DWORD creationFlags, LPPROC_THREAD_ATTRIBUTE_LIST attributeList, DWORD* threadId) noexcept
        {
            CT_ASSERT(!IsValid());
            handle_ = CreateRemoteThreadEx(process, threadAttributes, stackSize, startAddress, parameter, creationFlags, attributeList, threadId);
            return Error::GetError(IsValid());
        }

        /// <summary>Closes the handle (<c>CloseHandle</c>).</summary>
        /// <returns>
        /// <see cref="Error::Success"/> (<c>0</c>) on success, also if there is no handle; otherwise the
        /// Win32 error code.
        /// </returns>
        /// <remarks>On failure the object keeps the handle.</remarks>
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

        /// <summary>Returns the calling thread's pseudo-handle (<c>GetCurrentThread</c>).</summary>
        /// <returns>An object holding the pseudo-handle.</returns>
        /// <remarks>
        /// Valid only on the calling thread; needs no closing (<c>CloseHandle</c> on it does nothing).
        /// </remarks>
        /// <seealso cref="CurrentId"/>
        static Thread Current() noexcept { return Thread(GetCurrentThread()); }

        /// <summary>Returns the calling thread's id (<c>GetCurrentThreadId</c>).</summary>
        /// <returns>The id, unique system-wide while the thread exists.</returns>
        static DWORD CurrentId() noexcept { return GetCurrentThreadId(); }

        /// <summary>Gets the exit code (<c>GetExitCodeThread</c>).</summary>
        /// <param name="exitCode">Receives the exit code; <c>STILL_ACTIVE</c> while the thread runs.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        ErrorCode GetExitCode(DWORD& exitCode) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::GetExitCodeThread(handle_, &exitCode));
        }

        /// <summary>Sets the affinity mask (<c>SetThreadAffinityMask</c>).</summary>
        /// <param name="affinityMask">
        /// In: the new mask within the thread's processor group. Out, on success: the previous mask.
        /// </param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="SetGroupAffinity"/>
        ErrorCode SetAffinityMask(DWORD_PTR& affinityMask) noexcept
        {
            CT_ASSERT(IsValid());
            affinityMask = ::SetThreadAffinityMask(handle_, affinityMask);
            return affinityMask ? Error::Success : Error::GetLastError();
        }

        /// <summary>Gets processor group and affinity (<c>GetThreadGroupAffinity</c>).</summary>
        /// <param name="groupAffinity">Receives the thread's processor group and affinity mask in it.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="SetGroupAffinity"/>
        ErrorCode GetGroupAffinity(GROUP_AFFINITY& groupAffinity) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::GetThreadGroupAffinity(handle_, &groupAffinity));
        }

        /// <summary>Sets processor group and affinity (<c>SetThreadGroupAffinity</c>).</summary>
        /// <param name="groupAffinity">
        /// The new processor group and a nonzero affinity mask within it; the reserved fields must be
        /// <c>0</c>.
        /// </param>
        /// <param name="previousGroupAffinity">Receives the previous one; may be <c>nullptr</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="GetGroupAffinity"/>
        ErrorCode SetGroupAffinity(const GROUP_AFFINITY& groupAffinity, GROUP_AFFINITY* previousGroupAffinity = nullptr) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::SetThreadGroupAffinity(handle_, &groupAffinity, previousGroupAffinity));
        }

        /// <summary>Gets the thread id (<c>GetThreadId</c>).</summary>
        /// <param name="id">Receives the thread's id; <c>0</c> on failure.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        ErrorCode GetId(DWORD& id) noexcept
        {
            CT_ASSERT(IsValid());
            id = ::GetThreadId(handle_);
            return id ? Error::Success : Error::GetLastError();
        }

        /// <summary>Gets the ideal processor (<c>GetThreadIdealProcessorEx</c>).</summary>
        /// <param name="idealProcessor">Receives the processor group and number of the ideal processor.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="SetIdealProcessor"/>
        ErrorCode GetIdealProcessor(PROCESSOR_NUMBER& idealProcessor) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::GetThreadIdealProcessorEx(handle_, &idealProcessor));
        }

        /// <summary>Sets the ideal processor in the thread's group (<c>SetThreadIdealProcessor</c>).</summary>
        /// <param name="idealProcessor">
        /// In: the new processor (<c>MAXIMUM_PROCESSORS</c> only queries). Out, on success: the
        /// previous one.
        /// </param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="GetIdealProcessor"/>
        ErrorCode SetIdealProcessor(DWORD& idealProcessor) noexcept
        {
            CT_ASSERT(IsValid());
            idealProcessor = ::SetThreadIdealProcessor(handle_, idealProcessor);
            return idealProcessor != (DWORD)-1 ? Error::Success : Error::GetLastError();
        }

        /// <summary>Sets the ideal processor in any group (<c>SetThreadIdealProcessorEx</c>).</summary>
        /// <param name="idealProcessor">Processor group and number of the new ideal processor.</param>
        /// <param name="previousIdealProcessor">Receives the previous one; may be <c>nullptr</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        ErrorCode SetIdealProcessor(PROCESSOR_NUMBER& idealProcessor, PROCESSOR_NUMBER* previousIdealProcessor = nullptr) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::SetThreadIdealProcessorEx(handle_, &idealProcessor, previousIdealProcessor));
        }

        /// <summary>Gets whether I/O requests are pending (<c>GetThreadIOPendingFlag</c>).</summary>
        /// <param name="ioIsPending">Receives <c>TRUE</c> if the thread has I/O requests pending, else <c>FALSE</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        ErrorCode GetIoPendingFlag(BOOL& ioIsPending) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::GetThreadIOPendingFlag(handle_, &ioIsPending));
        }

        /// <summary>Gets the thread priority (<c>GetThreadPriority</c>).</summary>
        /// <param name="priority">Receives the priority, e.g. <c>THREAD_PRIORITY_NORMAL</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="SetPriority"/>
        ErrorCode GetPriority(int& priority) noexcept
        {
            CT_ASSERT(IsValid());
            priority = GetThreadPriority(handle_);
            return priority != THREAD_PRIORITY_ERROR_RETURN ? Error::Success : Error::GetLastError();
        }

        /// <summary>Sets the thread priority (<c>SetThreadPriority</c>).</summary>
        /// <param name="priority">
        /// The new priority, e.g. <c>THREAD_PRIORITY_NORMAL</c>, <c>THREAD_PRIORITY_HIGHEST</c> or
        /// <c>THREAD_MODE_BACKGROUND_BEGIN</c>.
        /// </param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="GetPriority"/>
        ErrorCode SetPriority(int priority) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::SetThreadPriority(handle_, priority));
        }

        /// <summary>Gets whether priority boosting is disabled (<c>GetThreadPriorityBoost</c>).</summary>
        /// <param name="disablePriorityBoost">Receives <c>TRUE</c> if boosting is disabled, <c>FALSE</c> if enabled.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="SetPriorityBoost"/>
        ErrorCode GetPriorityBoost(BOOL& disablePriorityBoost) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::GetThreadPriorityBoost(handle_, &disablePriorityBoost));
        }

        /// <summary>Disables or enables dynamic priority boosting (<c>SetThreadPriorityBoost</c>).</summary>
        /// <param name="disablePriorityBoost"><c>TRUE</c> disables boosting, <c>FALSE</c> enables it.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <seealso cref="GetPriorityBoost"/>
        ErrorCode SetPriorityBoost(BOOL disablePriorityBoost) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::SetThreadPriorityBoost(handle_, disablePriorityBoost));
        }

        /// <summary>Gets the thread's times (<c>GetThreadTimes</c>).</summary>
        /// <param name="creationTime">Receives the creation time (UTC, 100-ns units since 1601-01-01).</param>
        /// <param name="exitTime">Receives the exit time in the same format; undefined while the thread runs.</param>
        /// <param name="kernelTime">Receives the CPU time spent in kernel mode, in 100-ns units.</param>
        /// <param name="userTime">Receives the CPU time spent in user mode, in 100-ns units.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Creation and exit time, kernel and user CPU time.</remarks>
        /// <seealso cref="QueryCycleTime"/>
        ErrorCode GetTimes(FILETIME& creationTime, FILETIME& exitTime, FILETIME& kernelTime, FILETIME& userTime) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::GetThreadTimes(handle_, &creationTime, &exitTime, &kernelTime, &userTime));
        }

        /// <summary>Gets the CPU clock cycles the thread has used (<c>QueryThreadCycleTime</c>).</summary>
        /// <param name="cycleTime">Receives the cycle count, in user and kernel mode together.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        ErrorCode QueryCycleTime(ULONG64& cycleTime) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::QueryThreadCycleTime(handle_, &cycleTime));
        }

        /// <summary>Suspends the OS thread (<c>SuspendThread</c>).</summary>
        /// <param name="previousSuspendCount">Receives the previous suspend count.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Not a task suspension; meant for debuggers and tools.</remarks>
        /// <seealso cref="Resume"/>
        ErrorCode Suspend(DWORD& previousSuspendCount) noexcept
        {
            CT_ASSERT(IsValid());
            previousSuspendCount = ::SuspendThread(handle_);
            return previousSuspendCount != (DWORD)-1 ? Error::Success : Error::GetLastError();
        }

        /// <summary>Suspends the OS thread (<c>SuspendThread</c>).</summary>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Discards the previous suspend count.</remarks>
        /// <seealso cref="Resume"/>
        ErrorCode Suspend() noexcept
        {
            DWORD previousSuspendCount;
            return Suspend(previousSuspendCount);
        }

        /// <summary>Decrements the suspend count (<c>ResumeThread</c>).</summary>
        /// <param name="previousSuspendCount">Receives the previous suspend count.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>The thread runs again when the count reaches zero.</remarks>
        /// <seealso cref="Suspend"/>
        ErrorCode Resume(DWORD& previousSuspendCount) noexcept
        {
            CT_ASSERT(IsValid());
            previousSuspendCount = ::ResumeThread(handle_);
            return previousSuspendCount != (DWORD)-1 ? Error::Success : Error::GetLastError();
        }

        /// <summary>Decrements the suspend count (<c>ResumeThread</c>).</summary>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Discards the previous suspend count.</remarks>
        /// <seealso cref="Suspend"/>
        ErrorCode Resume() noexcept
        {
            DWORD previousSuspendCount;
            return Resume(previousSuspendCount);
        }

        /// <summary>Kills the thread (<c>TerminateThread</c>).</summary>
        /// <param name="exitCode">The exit code the thread ends with (see <see cref="GetExitCode"/>).</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Dangerous: no cleanup, held locks stay held.</remarks>
        ErrorCode Terminate(DWORD exitCode) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::TerminateThread(handle_, exitCode));
        }

        /// <summary>Waits until the thread exits (<c>WaitForSingleObject</c>).</summary>
        /// <param name="milliseconds">Timeout in milliseconds; <c>INFINITE</c> waits without one.</param>
        /// <param name="event">
        /// Receives the wait result (<c>WAIT_OBJECT_0</c>, <c>WAIT_TIMEOUT</c>, ...).
        /// </param>
        /// <returns>
        /// <see cref="Error::Success"/> unless the wait failed, so a timeout is Success too.
        /// </returns>
        /// <remarks>Blocks the whole OS thread: called from a task it blocks the scheduler thread.</remarks>
        ErrorCode Wait(DWORD milliseconds, DWORD& event) noexcept
        {
            CT_ASSERT(IsValid());
            event = WaitForSingleObject(handle_, milliseconds);
            return event != WAIT_FAILED ? Error::Success : Error::GetLastError();
        }

        /// <summary>Queues a user-mode APC to the thread (<c>QueueUserAPC</c>).</summary>
        /// <param name="apcFunction">The function to run on the thread; required.</param>
        /// <param name="data">Passed unchanged to <paramref name="apcFunction"/>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>The APC runs when the thread waits alertably.</remarks>
        ErrorCode QueueUserApc(PAPCFUNC apcFunction, ULONG_PTR data) noexcept
        {
            CT_ASSERT(IsValid());
            return Error::GetError(::QueueUserAPC(apcFunction, handle_, data));
        }

    private:
        explicit Thread(HANDLE handle) noexcept : handle_(handle) {}

        HANDLE handle_{nullptr};
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows
