//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// The main of a test program built with the portable CppUnitTest.h: runs every registered test
// method, class by class, with the class's initialize and cleanup around them, and exits with 1
// if any failed. A method that throws TestSkipped, or that --filter leaves out, counts as skipped.
//
// Usage: <tests> [--filter <prefix>] [--list]
//   --filter  runs only the methods whose "Class.Method" or "Method" starts with the prefix
//   --list    prints the names and runs nothing
//

#include "CppUnitTest.h"

#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    using namespace Microsoft::VisualStudio::CppUnitTestFramework;

    // A method that runs longer than this ends the program (exit code 1) with its name.
    constexpr unsigned MethodTimeLimitSeconds = 300;

    const char* volatile currentMethod = "";

    void MethodTimedOut(int) noexcept
    {
        // async-signal-safe: write and _exit only
        const char prefix[] = "\n    FAILED: time limit exceeded in ";
        [[maybe_unused]] auto written = write(2, prefix, sizeof(prefix) - 1);
        const char* name = currentMethod;
        written          = write(2, name, std::strlen(name));
        written          = write(2, "\n", 1);
        _exit(1);
    }

    bool Matches(const Detail::TestCase& test, const std::string& prefix)
    {
        if (prefix.empty())
            return true;
        const std::string full = test.className + "." + test.methodName;
        return full.compare(0, prefix.size(), prefix) == 0 || test.methodName.compare(0, prefix.size(), prefix) == 0;
    }

    enum class Outcome { Passed, Failed, Skipped };

    // Runs a body, reporting an exception as a failure; TestSkipped is no failure.
    Outcome Run(Detail::Body body, const char* what)
    {
        try {
            body();
            return Outcome::Passed;
        } catch (const AssertFailed&) {
            return Outcome::Failed;
        } catch (const TestSkipped&) {
            std::printf("    skipped\n");
            return Outcome::Skipped;
        } catch (const std::exception& exception) {
            std::printf("    FAILED %s: exception: %s\n", what, exception.what());
            return Outcome::Failed;
        } catch (...) {
            std::printf("    FAILED %s: unknown exception\n", what);
            return Outcome::Failed;
        }
    }
} // namespace

int main(int argc, char** argv)
{
    std::string prefix;
    bool        list = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            prefix = argv[++i];
        else if (std::strcmp(argv[i], "--list") == 0)
            list = true;
        else {
            std::printf("usage: %s [--filter <prefix>] [--list]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGALRM, &MethodTimedOut);

    auto& cases = Detail::Cases();
    int   passed = 0, failed = 0, skipped = 0;

    // class by class, in registration order
    std::string currentClass;
    bool        classInitialized = false;

    auto finishClass = [&] {
        if (!currentClass.empty() && classInitialized) {
            auto& hooks = Detail::HooksOf(currentClass);
            // a failed cleanup is a failure of the run, as under MSTest
            if (hooks.cleanup && Run(hooks.cleanup, "class cleanup") == Outcome::Failed)
                ++failed;
        }
        classInitialized = false;
    };

    for (const auto& test : cases) {
        if (!Matches(test, prefix)) {
            ++skipped;
            continue;
        }

        if (list) {
            std::printf("%s.%s\n", test.className.c_str(), test.methodName.c_str());
            continue;
        }

        if (test.className != currentClass) {
            finishClass();
            currentClass = test.className;
            std::printf("%s\n", currentClass.c_str());

            auto& hooks = Detail::HooksOf(currentClass);
            classInitialized = !hooks.initialize || Run(hooks.initialize, "class initialize") == Outcome::Passed;
        }

        std::printf("  %s\n", test.methodName.c_str());
        std::fflush(stdout);
        // a method that hangs ends the run with its name rather than block until an outer limit
        currentMethod = test.methodName.c_str();
        alarm(MethodTimeLimitSeconds);
        const Outcome outcome = classInitialized ? Run(test.run, test.methodName.c_str()) : Outcome::Failed;
        if (outcome == Outcome::Passed)
            ++passed;
        else if (outcome == Outcome::Skipped)
            ++skipped;
        else
            ++failed;
        alarm(0);
    }
    finishClass();

    if (!list)
        std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
