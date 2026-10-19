// Tutorial chapter 1: blockpack without ConcurrentTasks - the sequential
// reference that every later chapter has to match byte for byte.
// [names]
#include "tutorial-common.h"

using namespace Tutorial;
// [/names]

// [pack]
// How long each stage took, summed over all blocks.
struct StageTimes {
    double read   = 0;
    double encode = 0;
    double write  = 0;
};

// Reads, encodes and writes one block after the other.
bool Pack(const Arguments& arguments, StageTimes& times)
{
    File input  = File::OpenRead(arguments.input);
    File output = File::Create(arguments.output);
    if (!input.IsOpen() || !output.IsOpen())
        return false;

    std::vector<std::byte> buffer(arguments.blockSize);
    EncodedBlock           block;
    std::uint32_t          crc = 0; // carried from block to block
    bool                   ok  = true;
    for (;;) {
        const Stopwatch   read;
        const std::size_t size = input.Read(buffer);
        times.read += read.Seconds();
        if (size == 0)
            break;

        const Stopwatch encode;
        EncodeBlock(std::span(buffer).first(size), block);
        times.encode += encode.Seconds();

        const Stopwatch write;
        crc = WriteBlock(output, block, crc, ok);
        times.write += write.Seconds();
    }
    return WriteFooter(output, crc) && ok;
}
// [/pack]

// [main]
int wmain(int argc, wchar_t* argv[])
{
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments))
        return arguments.exitCode;

    StageTimes      times;
    const Stopwatch total;
    const bool      packed = Pack(arguments, times);
    std::printf("time: %.2f s (read %.2f s, encode %.2f s, write %.2f s)\n",
                total.Seconds(), times.read, times.encode, times.write);
    if (!packed) {
        std::printf("cannot pack the file\n");
        return 1;
    }
    return CheckOutput(arguments) ? 0 : 1;
}
// [/main]
