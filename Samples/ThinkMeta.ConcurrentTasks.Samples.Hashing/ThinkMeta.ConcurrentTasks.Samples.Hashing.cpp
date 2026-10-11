#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <cstddef>
#include <algorithm>
#include <vector>
#include <format>
#include <iostream>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;
import ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

namespace TC = ThinkMeta::ConcurrentTasks;

std::vector<std::byte> ComputeHash(TC::Memory::IReadSequentialMemory* readSequentialMemory, LPCWSTR algorithm)
{
    std::vector<std::byte> hashValue;

    BCRYPT_ALG_HANDLE  hAlg   = nullptr;
    BCRYPT_HASH_HANDLE hHash  = nullptr;
    NTSTATUS           status = BCryptOpenAlgorithmProvider(&hAlg, algorithm, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
        return hashValue;

    DWORD hashObjectSize = 0;
    DWORD dataLen = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjectSize), sizeof(hashObjectSize), &dataLen, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return hashValue;
    }

    std::vector<BYTE> hashObject(hashObjectSize);

    DWORD hashLen = 0;
    status        = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &dataLen, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return hashValue;
    }

    status = BCryptCreateHash(hAlg, &hHash, hashObject.data(), hashObjectSize, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return hashValue;
    }

    auto memorySize = readSequentialMemory->GetSize();

    while (memorySize > 0) {
        std::size_t      size = std::min(256zu * 1024zu, memorySize);
        const std::byte* data = readSequentialMemory->RequestReadMemory(size);
        if (data == nullptr)
            break;
        status = BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data)), static_cast<ULONG>(size), 0);
        readSequentialMemory->ReleaseReadMemory(size);
        memorySize -= size;
        if (!BCRYPT_SUCCESS(status)) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return hashValue;
        }
    }

    hashValue.resize(hashLen);
    status = BCryptFinishHash(hHash, reinterpret_cast<PUCHAR>(hashValue.data()), hashLen, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!BCRYPT_SUCCESS(status))
        hashValue.clear();
    return hashValue;
}

int wmain([[maybe_unused]] int argc, wchar_t** argv)
{
    TC::ConvertThreadToTask();

    TC::SchedulerInfo schedulerInfo = {.numberOfThreads = 2, .threadCallback = nullptr, .threadCallbackContext = nullptr};

    auto scheduler = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &schedulerInfo);
    TC::AsyncIo::EnableAsyncIo();

    scheduler->Start();

    TC::Synchronization::TaskGroup taskGroup;
    TC::Memory::SpscVirtualMemoryPipe memoryPipe;

    auto readFileTask = TC::Tasks::Task::Create([&]() { TC::AsyncIo::AsyncFile::ReadFile(argv[1], GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &memoryPipe); });

    auto md5Task = TC::Tasks::Task::Create([&]() {
        auto hashValue = ComputeHash(&memoryPipe, BCRYPT_MD5_ALGORITHM);
        for (std::byte b : hashValue)
            std::cout << std::format("{:02X} ", std::to_integer<unsigned int>(b));
    });

    taskGroup.AddTask(&readFileTask);
    taskGroup.AddTask(&md5Task);

    scheduler->AddTask(&readFileTask, TC::TaskPriority::Normal);
    scheduler->AddTask(&md5Task, TC::TaskPriority::Normal);

    taskGroup.Wait();

    scheduler->Stop();
    scheduler->Destroy();

    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();
}
