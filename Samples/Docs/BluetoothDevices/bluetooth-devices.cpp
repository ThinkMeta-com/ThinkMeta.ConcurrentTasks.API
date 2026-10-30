// Bluetooth devices: four WinRT device searches run as tasks at the same time, then one task
// per device found counts its services. Every wait is WinRT::Await, called from ordinary
// functions. The devices found depend on the machine, so the program checks only that no
// search failed.
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.WinRT.Await;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::WinRT::Await;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;
// [/names]

// [count-services]
// Ordinary functions that wait: each Await suspends the calling task until the operation has
// completed. Cached mode reads what Windows already knows and does not connect to the device.
// Returns -1 if the services could not be read.
static std::int32_t CountRfcommServices(const winrt::hstring& id)
{
    const auto device = Await(BluetoothDevice::FromIdAsync(id));
    if (!device)
        return -1;

    const auto result = Await(device.GetRfcommServicesAsync(BluetoothCacheMode::Cached));
    return result.Error() == BluetoothError::Success ? static_cast<std::int32_t>(result.Services().Size()) : -1;
}

static std::int32_t CountGattServices(const winrt::hstring& id)
{
    const auto device = Await(BluetoothLEDevice::FromIdAsync(id));
    if (!device)
        return -1;

    const auto result = Await(device.GetGattServicesAsync(BluetoothCacheMode::Cached));
    return result.Status() == GenericAttributeProfile::GattCommunicationStatus::Success ? static_cast<std::int32_t>(result.Services().Size()) : -1;
}

// A plain function pointer, although the functions behind it wait.
using CountServices = std::int32_t (*)(const winrt::hstring& id);
// [/count-services]

// [device-task]
// Counts the services of one device.
class DeviceTask : public TC::Tasks::Task
{
public:
    void Initialize(winrt::hstring id, CountServices countServices) noexcept
    {
        id_            = std::move(id);
        countServices_ = countServices;
    }

    void Run() noexcept override
    {
        try {
            serviceCount_ = countServices_(id_);
        }
        catch (const winrt::hresult_error&) {
            // the device could not be opened; its services stay unknown
        }
    }

    std::int32_t ServiceCount() const noexcept { return serviceCount_; }

private:
    winrt::hstring id_;
    CountServices  countServices_ = nullptr;
    std::int32_t   serviceCount_  = -1;
};
// [/device-task]

class QueryTask : public TC::Tasks::Task
{
public:
    QueryTask(const wchar_t* title, winrt::hstring selector, CountServices countServices) noexcept
        : title_(title),
          selector_(std::move(selector)),
          countServices_(countServices)
    {}

    // [query]
    void Run() noexcept override
    {
        const auto start = GetTickCount64();
        try {
            const auto properties = winrt::single_threaded_vector<winrt::hstring>({AddressProperty});
            devices_              = Await(DeviceInformation::FindAllAsync(selector_, properties, DeviceInformationKind::AssociationEndpoint));
            CountAllServices();
        }
        catch (const winrt::hresult_error& e) {
            error_ = e.code();
        }
        milliseconds_ = GetTickCount64() - start;
    }
    // [/query]

    bool Failed() const noexcept { return error_ < 0; }

    void Print() const noexcept
    {
        std::wprintf(L"%ls (%llu ms)\n", title_, milliseconds_);
        if (Failed()) {
            std::wprintf(L"  error 0x%08X\n", static_cast<std::uint32_t>(error_));
            return;
        }
        if (devices_.Size() == 0)
            std::wprintf(L"  none\n");

        for (std::uint32_t i = 0; i < devices_.Size(); ++i) {
            const auto device  = devices_.GetAt(i);
            const auto address = winrt::unbox_value_or<winrt::hstring>(device.Properties().TryLookup(AddressProperty), L"?");
            const auto count   = deviceTasks_[i].ServiceCount();

            std::wprintf(L"  %ls  %-24ls ", address.c_str(), device.Name().empty() ? L"(no name)" : device.Name().c_str());
            if (count >= 0)
                std::wprintf(L"%2d services\n", count);
            else
                std::wprintf(L" ? services\n");
        }
    }

private:
    static constexpr const wchar_t* AddressProperty = L"System.Devices.Aep.DeviceAddress";

    // [fork-join]
    // One task per device; all of them wait at the same time.
    void CountAllServices()
    {
        const auto numDevices = devices_.Size();
        deviceTasks_          = std::make_unique<DeviceTask[]>(numDevices);

        // Read every ID before the first task starts: an exception must not leave the group while
        // started tasks still report to it.
        for (std::uint32_t i = 0; i < numDevices; ++i)
            deviceTasks_[i].Initialize(devices_.GetAt(i).Id(), countServices_);

        TC::Synchronization::TaskGroup_s deviceGroup;
        for (std::uint32_t i = 0; i < numDevices; ++i) {
            deviceGroup.AddTask(&deviceTasks_[i]);
            TC::GetTaskScheduler()->AddTask(&deviceTasks_[i], TC::TaskPriority::Normal);
        }

        deviceGroup.Wait();
    }
    // [/fork-join]

    const wchar_t*                title_;
    winrt::hstring                selector_;
    CountServices                 countServices_;
    DeviceInformationCollection   devices_{nullptr};
    std::unique_ptr<DeviceTask[]> deviceTasks_;
    winrt::hresult                error_;
    std::uint64_t                 milliseconds_ = 0;
};

int main()
{
    // [run]
    // The scheduler threads join the multithreaded apartment implicitly.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    QueryTask queries[] = {
        {L"Bluetooth, paired", BluetoothDevice::GetDeviceSelectorFromPairingState(true), CountRfcommServices},
        {L"Bluetooth, not paired", BluetoothDevice::GetDeviceSelectorFromPairingState(false), CountRfcommServices},
        {L"Bluetooth LE, paired", BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), CountGattServices},
        {L"Bluetooth LE, not paired", BluetoothLEDevice::GetDeviceSelectorFromPairingState(false), CountGattServices},
    };

    TC::ConvertThreadToTask();

    TC::SchedulerInfo info      = {.numberOfThreads = 2};
    auto              scheduler = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    scheduler->Start();

    const auto start = GetTickCount64();

    TC::Synchronization::TaskGroup_s queryGroup;
    for (auto& query : queries) {
        queryGroup.AddTask(&query);
        scheduler->AddTask(&query, TC::TaskPriority::Normal);
    }
    queryGroup.Wait();

    const auto milliseconds = GetTickCount64() - start;

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    // [/run]

    bool ok = true;
    for (const auto& query : queries) {
        query.Print();
        ok = ok && !query.Failed();
    }

    std::wprintf(L"All searches took %llu ms (%ls)\n", milliseconds, ok ? L"ok" : L"FAILED");
    return ok ? 0 : 1;
}
