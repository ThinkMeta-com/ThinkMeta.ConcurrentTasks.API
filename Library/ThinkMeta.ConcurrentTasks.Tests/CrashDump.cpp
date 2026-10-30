#include "pch.h"

// Writes a minidump on the first access violation (or similar hard fault) raised in native code, so
// that a crash of the test host, which otherwise ends the run without a word, leaves something to
// look at. Managed code (the test host is a .NET process) raises access violations of its own for
// null references in JIT-compiled code, which lives in no module; those are left alone.
// The dump goes next to the test DLL as crash-<pid>.dmp; open it with WinDbg or cdb -z.

#define NOMINMAX
#include <Windows.h>

#include <DbgHelp.h>

#include <atomic>
#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

namespace {
    std::atomic<bool> dumped{false};

    // The module an address belongs to, as a path; false for code outside any module (JIT).
    bool ModuleOf(const void* address, wchar_t (&path)[MAX_PATH]) noexcept
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
            return false;
        return GetModuleFileNameW(module, path, MAX_PATH) != 0;
    }

    bool IsRuntimeModule(const wchar_t* path) noexcept
    {
        const wchar_t* name = wcsrchr(path, L'\\');
        name                = name ? name + 1 : path;
        return _wcsnicmp(name, L"clr", 3) == 0 || _wcsnicmp(name, L"coreclr", 7) == 0 || _wcsnicmp(name, L"mscor", 5) == 0 || wcsstr(name, L".ni.dll") != nullptr;
    }

    LONG CALLBACK OnException(EXCEPTION_POINTERS* info) noexcept
    {
        const auto code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
            return EXCEPTION_CONTINUE_SEARCH;

        wchar_t faulting[MAX_PATH];
        if (!ModuleOf(info->ExceptionRecord->ExceptionAddress, faulting) || IsRuntimeModule(faulting))
            return EXCEPTION_CONTINUE_SEARCH;

        if (dumped.exchange(true))
            return EXCEPTION_CONTINUE_SEARCH;

        wchar_t path[MAX_PATH];
        if (!ModuleOf(reinterpret_cast<const void*>(&OnException), path))
            return EXCEPTION_CONTINUE_SEARCH;
        if (auto* slash = wcsrchr(path, L'\\'))
            swprintf_s(slash + 1, MAX_PATH - (slash + 1 - path), L"crash-%lu.dmp", GetCurrentProcessId());

        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo), &exception, nullptr, nullptr);
            CloseHandle(file);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Removed again when the test DLL unloads: the test host outlives it and raises exceptions of its
    // own while it shuts down, which would otherwise call into unloaded code.
    struct Install {
        Install() noexcept : handler_(AddVectoredExceptionHandler(1, OnException)) {}
        ~Install()
        {
            if (handler_)
                RemoveVectoredExceptionHandler(handler_);
        }

        void* handler_;
    } install;
} // namespace
