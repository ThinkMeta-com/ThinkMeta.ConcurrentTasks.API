#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Platform.File;
import ThinkMeta.ConcurrentTasks.Platform.Mutex;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The platform-neutral file layer: FileOptions, Platform::File, Platform::FileName with CT_PATH
// and the error names it reports, the same on Windows and Linux.

namespace ThinkMeta::ConcurrentTasks::Tests {
    TEST_CLASS(PlatformFileTests)
    {
    public:
        TEST_METHOD(TestCreateOpenAndDelete)
        {
            // CT_PATH writes a literal of the platform's file name type
            constexpr Platform::FileName literal = CT_PATH("ctPlatformFileTest.tmp");
            Assert::IsTrue(literal[0] == 'c', L"CT_PATH did not give the literal");

            const auto                 tempPath = TempFilePath("ctPlatformFile");
            const Platform::FileName   TestPath = tempPath.c_str();

            {
                Platform::File file;
                Platform::FileOptions create{.access = Platform::FileAccess::ReadWrite, .creation = Platform::FileCreation::CreateNew};
                Assert::IsTrue(Platform::Error::Succeeded(file.Create(TestPath, create)), L"CreateNew of a new file failed");

                Platform::File::FileSize size = 1;
                Assert::IsTrue(Platform::Error::Succeeded(file.GetSize(size)), L"GetSize failed");
                Assert::AreEqual(0ull, static_cast<unsigned long long>(size), L"a new file must be empty");
                Assert::IsTrue(Platform::Error::Succeeded(file.Close()), L"Close failed");
                Assert::IsFalse(file.IsValid(), L"a closed file must not be valid");

                Assert::IsTrue(Platform::Error::Failed(file.Create(TestPath, create)), L"CreateNew of an existing file must fail");
                Assert::IsFalse(file.IsValid(), L"a failed Create must leave no file open");
            }

            {
                Platform::File file;
                Assert::IsTrue(Platform::Error::Succeeded(file.Create(TestPath, Platform::FileOptions{})), L"opening the existing file failed");
            }

            Assert::IsTrue(Platform::Error::Succeeded(Platform::File::Delete(TestPath)), L"Delete failed");

            Platform::File missing;
            const auto     error = missing.Create(TestPath, Platform::FileOptions{});
            Assert::IsTrue(error == Platform::Error::FileNotFound, L"opening a missing file must report FileNotFound");
        }

        TEST_METHOD(TestMutexGuard)
        {
            Platform::Mutex mutex;
            int             counter = 0;
            {
                Platform::MutexGuard guard(mutex);
                ++counter;
            }
            {
                Platform::MutexGuard guard(mutex); // released by the first guard
                ++counter;
            }
            Assert::AreEqual(2, counter);
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
