// Tutorial common code: everything blockpack needs that is not about
// concurrency - the command line, files, the block encoder, the CRC and a
// stopwatch. Every chapter of the tutorial includes it and shows only what
// changes.
#pragma once

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <span>
#include <vector>

namespace Tutorial {

    // The command line:
    //   blockpack <input> <output> [-r <reference>] [-b <block KB>] [-t <threads>]
    //             [-v <variant>]
    //   blockpack --make-input <path> <MB>
    struct Arguments {
        const wchar_t* input     = nullptr;
        const wchar_t* output    = nullptr;
        const wchar_t* reference = nullptr; // compare the output with this file
        std::size_t    blockSize = 1024 * 1024;
        std::uint32_t  threads   = 0;   // 0: one per logical processor
        const wchar_t* variant   = L""; // -v: a variant that some chapters offer
        int            exitCode  = 0;   // when ParseArguments returns false
    };

    // An encoded block as it goes to the output file.
    struct EncodedBlock {
        std::uint32_t          rawSize = 0;
        std::vector<std::byte> data; // predictor byte, then the coded residuals
    };

    // Wall time since construction, in seconds.
    class Stopwatch
    {
    public:
        Stopwatch() noexcept { QueryPerformanceCounter(&start_); }

        double Seconds() const noexcept
        {
            LARGE_INTEGER now;
            LARGE_INTEGER frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(now.QuadPart - start_.QuadPart)
                / static_cast<double>(frequency.QuadPart);
        }

    private:
        LARGE_INTEGER start_;
    };

    // A file opened for reading or writing; plain blocking Windows I/O.
    class File
    {
    public:
        static File OpenRead(const wchar_t* path) noexcept
        {
            return File(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr));
        }

        static File Create(const wchar_t* path) noexcept
        {
            return File(CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        }

        File(File&& other) noexcept : handle_(other.handle_)
        {
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        File(const File&)            = delete;
        File& operator=(const File&) = delete;
        File& operator=(File&&)      = delete;
        ~File() noexcept
        {
            if (IsOpen())
                CloseHandle(handle_);
        }

        bool IsOpen() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

        std::uint64_t Size() const noexcept
        {
            LARGE_INTEGER size{};
            return GetFileSizeEx(handle_, &size)
                ? static_cast<std::uint64_t>(size.QuadPart)
                : 0;
        }

        // Reads up to buffer.size() bytes; returns how many were read, 0 at the end
        // of the file or on an error.
        std::size_t Read(std::span<std::byte> buffer) noexcept
        {
            DWORD read = 0; // DWORD: ReadFile's out parameter
            if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &read, nullptr))
                return 0;
            return read;
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
            DWORD written = 0; // DWORD: WriteFile's out parameter
            return WriteFile(handle_, data.data(), static_cast<DWORD>(data.size()),
                             &written, nullptr)
                && written == data.size();
        }

    private:
        explicit File(HANDLE handle) noexcept : handle_(handle) {}

        HANDLE handle_;
    };

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
        inline bool MakeInput(const wchar_t* path, std::size_t megabytes) noexcept
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

        inline void PrintUsage() noexcept
        {
            std::printf("usage: blockpack <input> <output> [-r <reference>] "
                        "[-b <block KB>] [-t <threads>] [-v <variant>]\n"
                        "       blockpack --make-input <path> <MB>\n");
        }

    } // namespace Detail

    // Reads the command line. Returns false when the program should end at once
    // with arguments.exitCode: after a usage error, or after --make-input.
    inline bool ParseArguments(int argc, wchar_t* argv[],
                               Arguments& arguments) noexcept
    {
        if (argc == 4 && std::wcscmp(argv[1], L"--make-input") == 0) {
            const bool made
                = Detail::MakeInput(argv[2], std::wcstoul(argv[3], nullptr, 10));
            std::printf("%s\n", made ? "input created" : "cannot create the input");
            arguments.exitCode = made ? 0 : 1;
            return false;
        }
        arguments.exitCode = 2;
        if (argc < 3) {
            Detail::PrintUsage();
            return false;
        }
        arguments.input  = argv[1];
        arguments.output = argv[2];
        for (int i = 3; i + 1 < argc; i += 2) {
            const wchar_t* value = argv[i + 1];
            if (std::wcscmp(argv[i], L"-r") == 0)
                arguments.reference = value;
            else if (std::wcscmp(argv[i], L"-b") == 0)
                arguments.blockSize
                    = std::wcstoul(value, nullptr, 10) * std::size_t{1024};
            else if (std::wcscmp(argv[i], L"-t") == 0)
                arguments.threads = std::wcstoul(value, nullptr, 10);
            else if (std::wcscmp(argv[i], L"-v") == 0)
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
