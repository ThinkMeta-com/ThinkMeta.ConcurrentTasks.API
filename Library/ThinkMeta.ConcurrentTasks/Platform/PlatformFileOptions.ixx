//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// FileOptions: how to open a file, said the same way on every platform. Windows::File and
// Posix::File turn it into the flags of CreateFileW and open; the overloads with the platform's own
// flags stay for what the options do not cover.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <cstdint>

export module ThinkMeta.ConcurrentTasks.Platform.FileOptions;

namespace ThinkMeta::ConcurrentTasks::Platform {
    /// <summary>What the opener may do with the file.</summary>
    export enum class FileAccess : std::uint8_t {
        /// <summary>Read only (<c>GENERIC_READ</c>, <c>O_RDONLY</c>).</summary>
        Read,

        /// <summary>Write only (<c>GENERIC_WRITE</c>, <c>O_WRONLY</c>).</summary>
        Write,

        /// <summary>Read and write.</summary>
        ReadWrite,
    };

    /// <summary>What happens when the file exists, or does not.</summary>
    export enum class FileCreation : std::uint8_t {
        /// <summary>Open an existing file; fail if there is none (<c>OPEN_EXISTING</c>).</summary>
        OpenExisting,

        /// <summary>Create the file, or empty an existing one (<c>CREATE_ALWAYS</c>, <c>O_CREAT | O_TRUNC</c>).</summary>
        CreateAlways,

        /// <summary>Create the file; fail if it exists (<c>CREATE_NEW</c>, <c>O_CREAT | O_EXCL</c>).</summary>
        CreateNew,

        /// <summary>Open the file, creating it if there is none (<c>OPEN_ALWAYS</c>, <c>O_CREAT</c>).</summary>
        OpenAlways,

        /// <summary>Open an existing file and empty it (<c>TRUNCATE_EXISTING</c>, <c>O_TRUNC</c>).</summary>
        TruncateExisting,
    };

    /// <summary>How to open a file, on every platform alike.</summary>
    export struct FileOptions {
        /// <summary>What the opener may do with the file.</summary>
        FileAccess access = FileAccess::Read;

        /// <summary>What happens when the file exists, or does not.</summary>
        FileCreation creation = FileCreation::OpenExisting;

        /// <summary>
        /// Past the system's cache (<c>FILE_FLAG_NO_BUFFERING</c>, <c>O_DIRECT</c>): buffers, offsets
        /// and lengths must then be multiples of the sector size.
        /// </summary>
        bool unbuffered = false;

        /// <summary>Windows: others may read the file meanwhile (<c>FILE_SHARE_READ</c>). Linux has no share modes.</summary>
        bool shareRead = true;

        /// <summary>Windows: others may write the file meanwhile (<c>FILE_SHARE_WRITE</c>). Linux has no share modes.</summary>
        bool shareWrite = false;
    };
} // namespace ThinkMeta::ConcurrentTasks::Platform
