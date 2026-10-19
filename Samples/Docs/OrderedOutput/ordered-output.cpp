// Ordered output: a producer task builds a file of records with an
// OrderedFileWriter. Record headers are appended at once, payloads are deferred
// copies done by copy tasks, and the writer task writes the finished front of the
// file while the producer goes on.
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.OrderedFileWriter;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::OrderedFileWriter;

constexpr std::uint32_t Records         = 2000;
constexpr std::uint32_t MaxPayload      = 64 * 1024;
constexpr std::uint32_t RecordsPerBatch = 50;
constexpr std::uint32_t CopyTasks       = 3;

struct FileHeader {
    char          magic[8];
    std::uint64_t records; // patched with WriteAt when the count is known
};

struct RecordHeader {
    std::uint32_t index;
    std::uint32_t size;
};

// The payload of record i: a slice of the source buffer.
std::uint32_t PayloadSize(std::uint32_t i) noexcept
{
    return 1 + (i * 7919u) % MaxPayload;
}
std::size_t PayloadOffset(std::uint32_t i) noexcept
{
    return (static_cast<std::size_t>(i) * 4099u) % MaxPayload;
}

// [produce]
// The producer: appends with Write (copied at once) and WriteDeferred (copied later
// by a copy task), ends each batch, patches the header and closes the file.
bool Produce(OrderedFileWriter&            writer,
             const std::vector<std::byte>& source) noexcept
{
    const FileHeader header{{'R', 'E', 'C', 'O', 'R', 'D', 'S', '\0'}, 0};
    writer.Write(&header, sizeof(header));

    for (std::uint32_t i = 0; i < Records; ++i) {
        const RecordHeader record{i, PayloadSize(i)};
        writer.Write(&record, sizeof(record));
        // Borrowed, not copied: source must stay unchanged until the copy has run.
        writer.WriteDeferred(source.data() + PayloadOffset(i), record.size);
        // Hands the copies to the copy tasks, shows the writer the finished front.
        if ((i + 1) % RecordsPerBatch == 0)
            writer.EndBatch();
    }

    const std::uint64_t records = Records;
    writer.WriteAt(offsetof(FileHeader, records), &records, sizeof(records));
    // Finishes the copies, waits for the writer task to write everything.
    return writer.Close();
}
// [/produce]

// Reads the file back and checks every record.
bool Verify(const std::filesystem::path& path, const std::vector<std::byte>& source,
            std::uint64_t expectedSize)
{
    std::error_code     error;
    const std::uint64_t size = std::filesystem::file_size(path, error);
    if (error || size != expectedSize || size < sizeof(FileHeader))
        return false;
    std::vector<char> data(static_cast<std::size_t>(size));
    std::ifstream     file(path, std::ios::binary);
    if (!file.read(data.data(), static_cast<std::streamsize>(data.size())))
        return false;

    FileHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    if (std::memcmp(header.magic, "RECORDS", 8) != 0 || header.records != Records)
        return false;

    std::size_t pos = sizeof(FileHeader);
    for (std::uint32_t i = 0; i < Records; ++i) {
        RecordHeader record;
        if (data.size() - pos < sizeof(record))
            return false;
        std::memcpy(&record, data.data() + pos, sizeof(record));
        pos += sizeof(record);
        if (record.index != i || record.size != PayloadSize(i)
            || data.size() - pos < record.size)
            return false;
        if (std::memcmp(data.data() + pos, source.data() + PayloadOffset(i),
                        record.size)
            != 0)
            return false;
        pos += record.size;
    }
    return pos == data.size();
}

int main()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / L"ConcurrentTasks-ordered-output.bin";

    // The payload source: borrowed by WriteDeferred, so it outlives Close.
    std::vector<std::byte> source(2 * MaxPayload);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<std::byte>(i * 131 + i / 251);

    TC::ConvertThreadToTask();
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool                                closed = false;
    TC::AsyncIo::OrderedFileWriterStats stats;
    {
        // [setup]
        // The writer, its tasks and the group outlive every task: they go away
        // after Stop.
        OrderedFileWriter           writer;
        OrderedFileWriter::CopyTask copyTasks[CopyTasks]
            = {OrderedFileWriter::CopyTask(writer),
               OrderedFileWriter::CopyTask(writer),
               OrderedFileWriter::CopyTask(writer)};
        TC::Synchronization::TaskGroup_s group;

        // The bound only reserves address space; the memory is committed and
        // decommitted segment by segment as the file grows and gets written.
        const std::uint64_t bound = sizeof(FileHeader)
            + std::uint64_t{Records} * (sizeof(RecordHeader) + MaxPayload);
        const TC::Windows::ErrorCode error = writer.Open(path.c_str(), bound);

        auto producer = TC::Tasks::Task::Create([&] {
            closed = Produce(writer, source);
            writer.StopCopyTasks(CopyTasks); // after Close: lets the copy tasks end
        });

        if (TC::Windows::Error::Succeeded(error)) {
            // Writer and copy tasks after Open, the producer last.
            group.AddTask(&writer.Writer());
            scheduler->AddTask(&writer.Writer());
            for (auto& copyTask : copyTasks) {
                group.AddTask(&copyTask);
                scheduler->AddTask(&copyTask);
            }
            group.AddTask(&producer);
            scheduler->AddTask(&producer);
            group.Wait(); // all tasks have ended
            stats = writer.Stats();
        }
        // [/setup]
        scheduler->Stop();
    }
    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();

    const bool ok = closed && Verify(path, source, stats.bytes);
    std::printf("%u records, %llu bytes, %llu deferred copies (%llu bytes)\n",
                Records, static_cast<unsigned long long>(stats.bytes),
                static_cast<unsigned long long>(stats.deferredCopies),
                static_cast<unsigned long long>(stats.deferredBytes));
    std::printf("producer waited %.3f s, writer waited %.3f s (%s)\n",
                stats.producerWaited, stats.writerWaited, ok ? "ok" : "MISMATCH");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return ok ? 0 : 1;
}
