// Tutorial common code: everything blockpack needs that is not about
// concurrency - the command line, files, the block encoder, the CRC and a
// stopwatch. Every chapter of the tutorial includes it and shows only what
// changes. The few lines that differ between Windows and Linux are here too:
// file names (UTF-16 on Windows, UTF-8 on Linux), plain blocking file I/O,
// Ctrl+C and the processor count.
#pragma once

#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#    include <shellapi.h> // CommandLineToArgvW
#    pragma comment(lib, "shell32.lib")
#else
#    include <fcntl.h>
#    include <signal.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <span>
#include <vector>

namespace Tutorial {

    // A character of a file name: wchar_t on Windows, char (UTF-8) on Linux - as
    // the library's own file names (Platform::FileName).
#if defined(_WIN32)
    using Char = wchar_t;
#else
    using Char = char;
#endif

    // The command line:
    //   blockpack <input> <output> [-r <reference>] [-b <block KB>] [-t <threads>]
    //             [-v <variant>]
    //   blockpack --make-input <path> <MB>
    struct Arguments {
        const Char*   input     = nullptr;
        const Char*   output    = nullptr;
        const Char*   reference = nullptr; // compare the output with this file
        std::size_t   blockSize = 1024 * 1024;
        std::uint32_t threads   = 0;       // 0: one per logical processor
        const Char*   variant   = nullptr; // -v: a variant that some chapters offer
        int           exitCode  = 0;       // when ParseArguments returns false
    };

    namespace Detail {

        // Whether a command-line word is this ASCII text.
        inline bool Is(const Char* word, const char* text) noexcept
        {
            if (word == nullptr)
                return false;
            while (*word != 0 && static_cast<Char>(*text) == *word) {
                ++word;
                ++text;
            }
            return *word == 0 && *text == 0;
        }

    } // namespace Detail

    // Whether -v named this variant.
    inline bool IsVariant(const Arguments& arguments, const char* name) noexcept
    {
        return Detail::Is(arguments.variant, name);
    }

    // An encoded block as it goes to the output file.
    struct EncodedBlock {
        std::uint32_t          rawSize = 0;
        std::vector<std::byte> data; // predictor byte, then the coded residuals
    };

    // Wall time since construction, in seconds.
    class Stopwatch
    {
    public:
        double Seconds() const noexcept
        {
            return std::chrono::duration<double>(Clock::now() - start_).count();
        }

    private:
        using Clock = std::chrono::steady_clock;

        Clock::time_point start_ = Clock::now();
    };

    // The number of logical processors the program may use.
    inline std::uint32_t ProcessorCount() noexcept
    {
#if defined(_WIN32)
        return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
        return static_cast<std::uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
#endif
    }

    // A file opened for reading or writing; the platform's plain blocking I/O.
    class File
    {
    public:
        static File OpenRead(const Char* path) noexcept
        {
#if defined(_WIN32)
            return File(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr));
#else
            return File(open(path, O_RDONLY | O_CLOEXEC));
#endif
        }

        static File Create(const Char* path) noexcept
        {
#if defined(_WIN32)
            return File(CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
#else
            return File(open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
#endif
        }

        File(File&& other) noexcept : handle_(other.handle_)
        {
            other.handle_ = Closed;
        }
        File(const File&)            = delete;
        File& operator=(const File&) = delete;
        File& operator=(File&&)      = delete;
        ~File() noexcept
        {
            if (IsOpen()) {
#if defined(_WIN32)
                CloseHandle(handle_);
#else
                close(handle_);
#endif
            }
        }

        bool IsOpen() const noexcept { return handle_ != Closed; }

        std::uint64_t Size() const noexcept
        {
#if defined(_WIN32)
            LARGE_INTEGER size{};
            return GetFileSizeEx(handle_, &size)
                ? static_cast<std::uint64_t>(size.QuadPart)
                : 0;
#else
            struct stat status{};
            return fstat(handle_, &status) == 0
                ? static_cast<std::uint64_t>(status.st_size)
                : 0;
#endif
        }

        // Reads up to buffer.size() bytes; returns how many were read, 0 at the end
        // of the file or on an error.
        std::size_t Read(std::span<std::byte> buffer) noexcept
        {
#if defined(_WIN32)
            DWORD read = 0; // DWORD: ReadFile's out parameter
            if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &read, nullptr))
                return 0;
            return read;
#else
            const ssize_t read = ::read(handle_, buffer.data(), buffer.size());
            return read > 0 ? static_cast<std::size_t>(read) : 0;
#endif
        }

        // Reads the whole file into memory.
        bool ReadAll(std::vector<std::byte>& data)
        {
            data.resize(Size());
            std::size_t filled = 0;
            while (filled < data.size()) {
                const std::size_t piece
                    = std::min<std::size_t>(data.size() - filled, 1u << 30);
                const std::size_t read
                    = Read(std::span(data).subspan(filled, piece));
                if (read == 0)
                    return false;
                filled += read;
            }
            return true;
        }

        bool Write(std::span<const std::byte> data) noexcept
        {
#if defined(_WIN32)
            DWORD written = 0; // DWORD: WriteFile's out parameter
            return WriteFile(handle_, data.data(), static_cast<DWORD>(data.size()),
                             &written, nullptr)
                && written == data.size();
#else
            // a write may take less than all: the rest follows
            while (!data.empty()) {
                const ssize_t written = ::write(handle_, data.data(), data.size());
                if (written <= 0)
                    return false;
                data = data.subspan(static_cast<std::size_t>(written));
            }
            return true;
#endif
        }

    private:
#if defined(_WIN32)
        using Handle                      = HANDLE;
        static inline const Handle Closed = INVALID_HANDLE_VALUE;
#else
        using Handle                   = int;
        static constexpr Handle Closed = -1;
#endif

        explicit File(Handle handle) noexcept : handle_(handle) {}

        Handle handle_;
    };

    // Deletes a file, e.g. a half-written output.
    inline void RemoveFile(const Char* path) noexcept
    {
#if defined(_WIN32)
        DeleteFileW(path);
#else
        unlink(path);
#endif
    }

    // Calls handler when the user presses Ctrl+C, instead of ending the program. It
    // runs outside every task: on a thread of its own (Windows) or as a signal
    // handler (Linux), so it may only set a flag.
    inline void HandleCtrlC(void (*handler)() noexcept) noexcept
    {
        static void (*registered)() noexcept = nullptr;
        registered                           = handler;
#if defined(_WIN32)
        SetConsoleCtrlHandler(
            [](DWORD type) noexcept -> BOOL {
                if (type != CTRL_C_EVENT)
                    return FALSE;
                registered();
                return TRUE;
            },
            TRUE);
#else
        struct sigaction action{};
        action.sa_handler = [](int) { registered(); };
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGINT, &action, nullptr);
#endif
    }

    // CRC-32 (IEEE, as in zip and PNG), continued over consecutive pieces of data.
    inline std::uint32_t UpdateCrc32(std::uint32_t              crc,
                                     std::span<const std::byte> data) noexcept
    {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit)
                    c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();

        crc = ~crc;
        for (const std::byte b : data)
            crc = table[(crc ^ static_cast<std::uint32_t>(b)) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    namespace Detail {

        // Predicts each byte from the bytes before it; the encoder tries all and
        // keeps the one with the smallest output, as real encoders do in their mode
        // decision.
        inline std::uint8_t Predict(int predictor, std::span<const std::byte> input,
                                    std::size_t i) noexcept
        {
            const auto at = [&](std::size_t back) {
                return i >= back ? static_cast<int>(input[i - back]) : 0;
            };
            switch (predictor) {
                case 1: return static_cast<std::uint8_t>(at(1));
                case 2: return static_cast<std::uint8_t>(2 * at(1) - at(2));
                case 3: return static_cast<std::uint8_t>((at(1) + at(2)) / 2);
                default: return 0;
            }
        }

        // Residuals with runs of zeros coded as (0, run length).
        inline void EncodeWith(int predictor, std::span<const std::byte> input,
                               std::vector<std::byte>& output) noexcept
        {
            output.clear();
            output.push_back(static_cast<std::byte>(predictor));
            std::size_t zeros = 0;
            const auto  flush = [&] {
                while (zeros > 0) {
                    const std::size_t run = std::min<std::size_t>(zeros, 255);
                    output.push_back(std::byte{0});
                    output.push_back(static_cast<std::byte>(run));
                    zeros -= run;
                }
            };
            for (std::size_t i = 0; i < input.size(); ++i) {
                const auto residual
                    = static_cast<std::uint8_t>(static_cast<std::uint8_t>(input[i])
                                                - Predict(predictor, input, i));
                if (residual == 0) {
                    ++zeros;
                    continue;
                }
                flush();
                output.push_back(static_cast<std::byte>(residual));
            }
            flush();
        }

    } // namespace Detail

    // Encodes one block on its own: no state from other blocks goes in, so blocks
    // can be encoded in any order.
    inline void EncodeBlock(std::span<const std::byte> input, EncodedBlock& block)
    {
        block.rawSize = static_cast<std::uint32_t>(input.size());
        std::vector<std::byte> candidate;
        for (int predictor = 0; predictor < 4; ++predictor) {
            Detail::EncodeWith(predictor, input, candidate);
            if (predictor == 0 || candidate.size() < block.data.size())
                block.data.swap(candidate);
        }
    }

    // The header of a block in the output file: raw size, encoded size.
    inline std::array<std::uint32_t, 2>
        BlockHeader(const EncodedBlock& block) noexcept
    {
        return {block.rawSize, static_cast<std::uint32_t>(block.data.size())};
    }

    // The number of bytes a block takes in the output file.
    inline std::uint64_t BlockBytes(const EncodedBlock& block) noexcept
    {
        return sizeof(std::uint32_t) * 2 + block.data.size();
    }

    // Continues a CRC over a block as it is written: header, then data.
    inline std::uint32_t ContinueCrc(std::uint32_t       crc,
                                     const EncodedBlock& block) noexcept
    {
        const auto header = BlockHeader(block);
        crc               = UpdateCrc32(crc, std::as_bytes(std::span(header)));
        return UpdateCrc32(crc, block.data);
    }

    // The CRC of a block on its own, as if it were the whole file.
    inline std::uint32_t BlockCrc(const EncodedBlock& block) noexcept
    {
        return ContinueCrc(0, block);
    }

    namespace Detail {

        // Multiplies a 32x32 matrix over GF(2) with a vector.
        inline std::uint32_t Gf2Times(const std::array<std::uint32_t, 32>& matrix,
                                      std::uint32_t vector) noexcept
        {
            std::uint32_t sum = 0;
            for (int i = 0; vector != 0; ++i, vector >>= 1)
                if ((vector & 1) != 0)
                    sum ^= matrix[i];
            return sum;
        }

        inline void Gf2Square(std::array<std::uint32_t, 32>&       square,
                              const std::array<std::uint32_t, 32>& matrix) noexcept
        {
            for (int n = 0; n < 32; ++n)
                square[n] = Gf2Times(matrix, matrix[n]);
        }

    } // namespace Detail

    // The CRC of A followed by B, from the CRC of A, the CRC of B and the length
    // of B (the method of zlib's crc32_combine). Costs about the same for any
    // length, far less than computing the CRC of B again.
    inline std::uint32_t CombineCrc32(std::uint32_t crcA, std::uint32_t crcB,
                                      std::uint64_t lengthB) noexcept
    {
        if (lengthB == 0)
            return crcA;
        std::array<std::uint32_t, 32> even{}; // operator for 2^n zero bits
        std::array<std::uint32_t, 32> odd{};
        odd[0]            = 0xEDB88320u; // one zero bit
        std::uint32_t row = 1;
        for (int n = 1; n < 32; ++n, row <<= 1)
            odd[n] = row;
        Detail::Gf2Square(even, odd); // two zero bits
        Detail::Gf2Square(odd, even); // four zero bits
        for (;;) {
            Detail::Gf2Square(even, odd);
            if ((lengthB & 1) != 0)
                crcA = Detail::Gf2Times(even, crcA);
            lengthB >>= 1;
            if (lengthB == 0)
                break;
            Detail::Gf2Square(odd, even);
            if ((lengthB & 1) != 0)
                crcA = Detail::Gf2Times(odd, crcA);
            lengthB >>= 1;
            if (lengthB == 0)
                break;
        }
        return crcA ^ crcB;
    }

    // Writes a block: header, then data.
    inline bool WriteBlockData(File& output, const EncodedBlock& block) noexcept
    {
        const auto header = BlockHeader(block);
        return output.Write(std::as_bytes(std::span(header)))
            && output.Write(block.data);
    }

    // Writes a block and returns the CRC continued over the bytes written.
    inline std::uint32_t WriteBlock(File& output, const EncodedBlock& block,
                                    std::uint32_t crc, bool& ok) noexcept
    {
        ok = ok && WriteBlockData(output, block);
        return ContinueCrc(crc, block);
    }

    // Ends the file with the CRC over everything before it.
    inline bool WriteFooter(File& output, std::uint32_t crc) noexcept
    {
        return output.Write(std::as_bytes(std::span(&crc, 1)));
    }

    // Prints OK or MISMATCH when a reference was given; false only on a mismatch.
    inline bool CheckOutput(const Arguments& arguments) noexcept
    {
        if (arguments.reference == nullptr)
            return true;
        File a    = File::OpenRead(arguments.output);
        File b    = File::OpenRead(arguments.reference);
        bool same = a.IsOpen() && b.IsOpen() && a.Size() == b.Size();
        std::vector<std::byte> bufferA(1024 * 1024);
        std::vector<std::byte> bufferB(bufferA.size());
        while (same) {
            const std::size_t n = a.Read(bufferA);
            if (n == 0)
                break;
            same = b.Read(bufferB) == n
                && std::equal(bufferA.begin(), bufferA.begin() + n,
                              bufferB.begin());
        }
        std::printf("%s\n", same ? "OK" : "MISMATCH");
        return same;
    }

    namespace Detail {

        // A test input with structure the predictors can find: ramps, flat areas,
        // a smooth wave and some noise, always the same for the same size.
        inline bool MakeInput(const Char* path, std::size_t megabytes) noexcept
        {
            File file = File::Create(path);
            if (!file.IsOpen())
                return false;
            std::vector<std::byte> buffer(1024 * 1024);
            std::uint32_t          random = 12345;
            std::uint8_t           value  = 0;
            for (std::size_t mb = 0; mb < megabytes; ++mb) {
                for (std::size_t i = 0; i < buffer.size(); ++i) {
                    random = random * 1664525u + 1013904223u;
                    switch ((i >> 14) % 4) {
                        case 0: value = static_cast<std::uint8_t>(value + 1); break;
                        case 1: break;
                        case 2:
                            value = static_cast<std::uint8_t>(
                                128
                                + ((i & 255) < 128 ? i & 127 : 127 - (i & 127)));
                            break;
                        default:
                            value
                                = static_cast<std::uint8_t>(value + (random >> 30));
                            break;
                    }
                    buffer[i] = static_cast<std::byte>(value);
                }
                if (!file.Write(buffer))
                    return false;
            }
            return true;
        }

        inline unsigned long ToNumber(const wchar_t* text) noexcept
        {
            return std::wcstoul(text, nullptr, 10);
        }

        inline unsigned long ToNumber(const char* text) noexcept
        {
            return std::strtoul(text, nullptr, 10);
        }

        inline void PrintUsage() noexcept
        {
            std::printf("usage: blockpack <input> <output> [-r <reference>] "
                        "[-b <block KB>] [-t <threads>] [-v <variant>]\n"
                        "       blockpack --make-input <path> <MB>\n");
        }

    } // namespace Detail

    // Reads the command line as main receives it. Returns false when the program
    // should end at once with arguments.exitCode: after a usage error, or after
    // --make-input. On Windows the words are taken again in UTF-16, so that any
    // file name works; Linux passes them in UTF-8 already.
    inline bool ParseArguments(int argc, [[maybe_unused]] char* argv[],
                               Arguments& arguments) noexcept
    {
#if defined(_WIN32)
        // kept until the program ends: the arguments point into it
        Char** words = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (words == nullptr) {
            arguments.exitCode = 2;
            return false;
        }
#else
        Char** words = argv;
#endif
        if (argc == 4 && Detail::Is(words[1], "--make-input")) {
            const bool made
                = Detail::MakeInput(words[2], Detail::ToNumber(words[3]));
            std::printf("%s\n", made ? "input created" : "cannot create the input");
            arguments.exitCode = made ? 0 : 1;
            return false;
        }
        arguments.exitCode = 2;
        if (argc < 3) {
            Detail::PrintUsage();
            return false;
        }
        arguments.input  = words[1];
        arguments.output = words[2];
        for (int i = 3; i + 1 < argc; i += 2) {
            const Char* value = words[i + 1];
            if (Detail::Is(words[i], "-r"))
                arguments.reference = value;
            else if (Detail::Is(words[i], "-b"))
                arguments.blockSize = Detail::ToNumber(value) * std::size_t{1024};
            else if (Detail::Is(words[i], "-t"))
                arguments.threads = Detail::ToNumber(value);
            else if (Detail::Is(words[i], "-v"))
                arguments.variant = value;
            else {
                Detail::PrintUsage();
                return false;
            }
        }
        if ((argc - 3) % 2 != 0 || arguments.blockSize == 0) {
            Detail::PrintUsage();
            return false;
        }
        arguments.exitCode = 0;
        return true;
    }

} // namespace Tutorial
