// Tutorial chapter 2: a scheduler and one task - the sequential pack loop runs
// unchanged as a single task.
// [names]
#include "tutorial-common.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

namespace TC = ThinkMeta::ConcurrentTasks;
using namespace Tutorial;
// [/names]

// The loop of chapter 1, without the stage times.
bool Pack(const Arguments& arguments)
{
    File input  = File::OpenRead(arguments.input);
    File output = File::Create(arguments.output);
    if (!input.IsOpen() || !output.IsOpen())
        return false;

    std::vector<std::byte> buffer(arguments.blockSize);
    EncodedBlock           block;
    std::uint32_t          crc = 0;
    bool                   ok  = true;
    for (;;) {
        const std::size_t size = input.Read(buffer);
        if (size == 0)
            break;
        EncodeBlock(std::span(buffer).first(size), block);
        crc = WriteBlock(output, block, crc, ok);
    }
    return WriteFooter(output, crc) && ok;
}

// [task]
// The whole pack loop as one task. WaitableTask adds Wait, so main can wait for
// it; the result stays in the object.
class PackTask : public TC::Tasks::WaitableTask
{
public:
    explicit PackTask(const Arguments& arguments) noexcept : arguments_(arguments)
    {}

    void Run() noexcept override { packed = Pack(arguments_); }

    bool packed = false;

private:
    const Arguments& arguments_;
};
// [/task]

// [main]
int wmain(int argc, wchar_t* argv[])
{
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments))
        return arguments.exitCode;

    // main becomes a task too, so it may wait for one.
    TC::ConvertThreadToTask();

    // One scheduler thread per logical processor.
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    PackTask        task(arguments); // lives until after Stop
    const Stopwatch total;
    scheduler->AddTask(&task);
    task.Wait(); // main is suspended until Run has returned
    std::printf("time: %.2f s\n", total.Seconds());

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();

    if (!task.packed) {
        std::printf("cannot pack the file\n");
        return 1;
    }
    return CheckOutput(arguments) ? 0 : 1;
}
// [/main]
