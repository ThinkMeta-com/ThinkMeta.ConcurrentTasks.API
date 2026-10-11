// Guide "Streaming large files": OrderedFileWriter builds a file of records (Write,
// WriteDeferred, EndBatch, WriteAt, copy tasks, writer task), StreamedFileReader
// reads it back for two users with bounded memory. Uses a file in %TEMP% that it
// deletes; checks its own results outside the regions that the guide shows.
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.AsyncIo.OrderedFileWriter;
import ThinkMeta.ConcurrentTasks.AsyncIo.StreamedFileReader;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::OrderedFileWriter;
using TC::AsyncIo::StreamedFileReader;

// [layout]
// The file: a header, then one record per item, its 8-byte id followed by its
// payload.
struct Header {
    std::uint64_t magic;
    std::uint64_t count;
};
constexpr std::uint64_t Magic       = 0x5345525547544321; // any marker
constexpr std::size_t   PayloadSize = 60'000;
constexpr std::size_t   RecordSize  = sizeof(std::uint64_t) + PayloadSize;

// An item to export. Its payload belongs to someone else; nullptr: nothing to
// export.
struct Item {
    std::uint64_t    id;
    const std::byte* payload; // PayloadSize bytes
};
// [/layout]

// Test driver state: the items that were exported, and what the reader has seen of
// them.
static std::span<const Item> exported;
static std::uint64_t         recordsSeen = 0;
static std::uint64_t         badRecords  = 0;

static void Process(std::uint64_t id, const std::byte* payload) noexcept
{
    ++recordsSeen;
    if (id >= exported.size() || exported[id].payload == nullptr
        || std::memcmp(payload, exported[id].payload, PayloadSize) != 0)
        ++badRecords;
}

// [writer-produce]
// The single producer appends the file in order. The payloads are not copied here:
// copy tasks copy them into place later, so they must stay unchanged until Close
// has returned.
bool WriteItems(OrderedFileWriter& writer, std::span<const Item> items) noexcept
{
    Header header{}; // the count is known only at the end: patched below
    writer.Write(&header, sizeof(header));

    std::uint64_t count = 0;
    for (const Item& item : items) {
        if (item.payload == nullptr)
            continue;                            // nothing to export
        writer.Write(&item.id, sizeof(item.id)); // copied at once
        writer.WriteDeferred(item.payload,
                             PayloadSize); // copied later by a copy task
        if (++count % 16 == 0)
            writer
                .EndBatch(); // hand the copies to the copy tasks, publish progress
    }

    header = {Magic, count};
    writer.WriteAt(0, &header,
                   sizeof(header)); // in memory, or a patch if already written
    return writer.Close();          // finishes copies and writes, applies the patch
}
// [/writer-produce]

// [reader-use]
// User 1 reads the header and releases everything at once.
Header ReadHeader(StreamedFileReader& reader) noexcept
{
    Header header{};
    if (reader.Wait(sizeof(Header)) >= sizeof(Header)) // less only after Stop
        std::memcpy(&header, reader.Data(), sizeof(header));
    reader.Release(1, ~std::uint64_t{0}); // done with everything
    return header;
}

// User 0 walks the records: Wait before touching bytes, Release when done with
// them.
void ReadRecords(StreamedFileReader& reader, std::uint64_t count) noexcept
{
    const std::byte* data = reader.Data(); // byte 0 of the file
    std::uint64_t    pos  = sizeof(Header);
    for (std::uint64_t i = 0; i < count; ++i, pos += RecordSize) {
        if (reader.Wait(pos + RecordSize) < pos + RecordSize)
            break; // only after Stop
        std::uint64_t id;
        std::memcpy(&id, data + pos, sizeof(id));
        Process(id, data + pos + sizeof(id));
        reader.Release(0, pos + RecordSize); // user 0 is done below this position
    }
    reader.Release(0, ~std::uint64_t{0}); // done with the file
}
// [/reader-use]

int main()
{
    wchar_t     path[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, path);
    if (length == 0 || length + 32 > MAX_PATH)
        return 1;
    lstrcatW(path, L"ct-guide-streaming.bin");

    TC::ConvertThreadToTask();
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();
    bool ok = true;

    // 400 items whose payloads are slices of one source buffer; every 50th has
    // none.
    std::vector<std::byte> source(1'000'000 + PayloadSize);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<std::byte>(i * 31 + i / 977);
    std::vector<Item> items(400);
    std::uint64_t     withPayload = 0;
    for (std::uint64_t id = 0; id < items.size(); ++id) {
        const bool missing = id % 50 == 49;
        items[id] = Item{id, missing ? nullptr : &source[id * 7919 % 1'000'000]};
        withPayload += !missing;
    }
    exported = items;

    TC::Synchronization::TaskGroup_s
        tasks; // the writer's tasks, later the reader's tasks

    // [writer-setup]
    OrderedFileWriter                      writer;
    OrderedFileWriter::CopyTask            copy1(writer);
    OrderedFileWriter::CopyTask            copy2(writer);
    TC::AsyncIo::OrderedFileWriterSettings settings;
    settings.segmentSize = 1 << 20; // write and decommit in 1 MB steps

    const std::uint64_t bound = sizeof(Header)
        + items.size() * RecordSize; // the largest size the file may reach
    if (TC::Windows::Error::Failed(writer.Open(path, bound, settings)))
        return 1;

    TC::Tasks::Task* writerTasks[] = {&writer.Writer(), &copy1, &copy2};
    for (TC::Tasks::Task* task : writerTasks) {
        tasks.AddTask(task);
        scheduler->AddTask(task);
    }
    // [/writer-setup]

    // [writer-close]
    const bool closed = WriteItems(
        writer, items);      // main is the producer (a converted thread may be one)
    writer.StopCopyTasks(2); // two copy tasks were scheduled
    tasks.Wait();            // writer task and copy tasks have exited
    // [/writer-close]

    const auto          stats    = writer.Stats();
    const std::uint64_t fileSize = sizeof(Header) + withPayload * RecordSize;
    std::printf("written:  %llu records, close %s\n", stats.deferredCopies,
                closed ? "ok" : "failed");
    ok = ok && closed && stats.bytes == fileSize
        && stats.deferredCopies == withPayload;

    // [reader-setup]
    StreamedFileReader                      reader;
    TC::AsyncIo::StreamedFileReaderSettings readSettings;
    readSettings.segmentSize = 1 << 20; // read, commit and decommit in 1 MB steps
    readSettings.readAhead   = 4
        << 20; // read at most 4 MB past the oldest byte in use
    readSettings.readsInFlight = 4;

    if (TC::Windows::Error::Failed(reader.Open(path, readSettings, 2))) // two users
        return 1;
    tasks.AddTask(&reader);
    scheduler->AddTask(&reader); // the reader is a task itself
    // [/reader-setup]

    // [reader-run]
    // The record walk starts only after user 1 has released: until every user has
    // released, nothing can be decommitted.
    Header header{};
    auto   walkRecords = TC::Tasks::Task::Create([&reader, &header]() noexcept {
        if (header.magic == Magic)
            ReadRecords(reader, header.count);
        else
            reader.Release(
                0, ~std::uint64_t{0}); // not our file: release it all the same
    });
    auto   readHeader
        = TC::Tasks::Task::Create([&reader, &header, &walkRecords]() noexcept {
              header = ReadHeader(reader);
              walkRecords.Schedule();
          });

    tasks.AddTask(&walkRecords); // both users count in before the first one starts
    tasks.AddTask(&readHeader);
    scheduler->AddTask(&readHeader);
    tasks.Wait(); // both users and the reader task have exited
    // [/reader-run]

    const std::uint64_t window = readSettings.readAhead + readSettings.segmentSize;
    std::printf("read:     %llu records, bad %llu, read error %lu\n", recordsSeen,
                badRecords, reader.ReadError());
    std::printf("memory:   peak window within read-ahead + 1 segment: %s\n",
                reader.PeakWindow() <= window ? "yes" : "no");
    ok = ok && header.count == withPayload && recordsSeen == withPayload
        && badRecords == 0 && reader.ReadError() == 0
        && reader.PeakWindow() <= window;

    scheduler->Stop();
    scheduler->Destroy();
    TC::AsyncIo::DisableAsyncIo();
    TC::ConvertTaskToThread();
    DeleteFileW(path);
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
