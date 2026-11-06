//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// A portable stand-in for the MSTest C++ framework header (CppUnitTest.h of Visual Studio): the
// same macros, Assert and Logger names on a small registry of test methods, run by the main in
// TestMain.cpp. The test sources are the same under MSTest and here; only the include path
// decides which header they get. Not used in Visual Studio builds.
//
// Covered: TEST_CLASS, TEST_METHOD, TEST_CLASS_INITIALIZE, TEST_CLASS_CLEANUP, Assert::IsTrue,
// IsFalse, AreEqual, AreNotEqual, IsNull, IsNotNull, Fail, Logger::WriteMessage. A failed
// assertion throws AssertFailed, which ends the test method; the next method gets a new instance
// of the test class, as under MSTest. On the test thread only: inside a task (ITask::Run is
// noexcept) the exception ends the program, under MSTest as here - tasks record, the test asserts.
//

#pragma once

#include <strings.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <typeinfo>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#    include <cxxabi.h>
#    include <cstdlib>
#endif

namespace Microsoft::VisualStudio::CppUnitTestFramework {
    /// <summary>Thrown by a failed assertion; ends the test method.</summary>
    struct AssertFailed : std::exception {
        const char* what() const noexcept override { return "assertion failed"; }
    };

    /// <summary>
    /// Thrown by a test that cannot run on this machine (not part of MSTest); the runner counts it as
    /// skipped, not as failed.
    /// </summary>
    struct TestSkipped : std::exception {
        const char* what() const noexcept override { return "test skipped"; }
    };

    namespace Detail {
        using Body = void (*)();

        struct TestCase {
            std::string className;
            std::string methodName;
            Body        run;
        };

        struct ClassHooks {
            std::string className;
            Body        initialize = nullptr;
            Body        cleanup    = nullptr;
        };

        inline std::vector<TestCase>& Cases()
        {
            static std::vector<TestCase> cases;
            return cases;
        }

        inline std::vector<ClassHooks>& Hooks()
        {
            static std::vector<ClassHooks> hooks;
            return hooks;
        }

        inline ClassHooks& HooksOf(const std::string& className)
        {
            for (auto& hooks : Hooks()) {
                if (hooks.className == className)
                    return hooks;
            }
            Hooks().push_back({className});
            return Hooks().back();
        }

        template <typename T>
        std::string ClassName()
        {
#if defined(__GNUC__) || defined(__clang__)
            int   status    = 0;
            char* demangled = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
            std::string name = demangled && status == 0 ? demangled : typeid(T).name();
            std::free(demangled);
#else
            std::string name = typeid(T).name();
#endif
            // the unqualified name, as MSTest reports it
            const auto colon = name.rfind("::");
            return colon == std::string::npos ? name : name.substr(colon + 2);
        }

        // A test method runs on a new instance of its class. The macros give each method a tag type
        // of its own and turn its body into an overload of TestMethod (or ClassHook) taking that
        // tag: the registrar can name the tag before the body, and the call below is resolved only
        // here, where the class is complete. A member function cannot be declared twice in its
        // class, so the method itself cannot be declared ahead of the registrar.
        template <typename T, typename Tag>
        void InvokeMethod()
        {
            T instance;
            instance.TestMethod(Tag{});
        }

        template <typename T, typename Tag>
        void InvokeClassHook()
        {
            T::ClassHook(Tag{});
        }

        struct MethodRegistrar {
            MethodRegistrar(std::string className, const char* methodName, Body run) { Cases().push_back({std::move(className), methodName, run}); }
        };

        struct ClassInitializeRegistrar {
            ClassInitializeRegistrar(std::string className, Body body) { HooksOf(className).initialize = body; }
        };

        struct ClassCleanupRegistrar {
            ClassCleanupRegistrar(std::string className, Body body) { HooksOf(className).cleanup = body; }
        };

        inline std::string Narrow(const wchar_t* text)
        {
            std::string result;
            if (!text)
                return result;
            for (; *text; ++text)
                result += *text < 128 ? static_cast<char>(*text) : '?';
            return result;
        }

        /// <summary>The message of an assertion: wide as under MSTest, or narrow (UTF-8), the text type of the samples on Linux.</summary>
        struct Message {
            const wchar_t* wide   = nullptr;
            const char*    narrow = nullptr;

            Message(std::nullptr_t = nullptr) noexcept {}
            Message(const wchar_t* text) noexcept : wide(text) {}
            Message(const char* text) noexcept : narrow(text) {}
        };

        [[noreturn]] inline void Fail(const char* check, Message message)
        {
            const std::string text = message.narrow != nullptr ? std::string(message.narrow) : Narrow(message.wide);
            std::printf("    FAILED %s%s%s\n", check, text.empty() ? "" : ": ", text.c_str());
            std::fflush(stdout);
            throw AssertFailed{};
        }
    } // namespace Detail

    /// <summary>The base of every test class; TEST_CLASS derives from it.</summary>
    template <typename T>
    class TestClass
    {
    public:
        using ThisClass = T;
    };

    /// <summary>Writes a line of test output.</summary>
    struct Logger {
        static void WriteMessage(const char* message)
        {
            std::printf("    %s\n", message ? message : "");
            std::fflush(stdout);
        }

        static void WriteMessage(const wchar_t* message) { WriteMessage(Detail::Narrow(message).c_str()); }
    };

    /// <summary>The assertions; a failure throws <see cref="AssertFailed"/>.</summary>
    struct Assert {
        static void IsTrue(bool condition, Detail::Message message = nullptr)
        {
            if (!condition)
                Detail::Fail("IsTrue", message);
        }

        static void IsFalse(bool condition, Detail::Message message = nullptr)
        {
            if (condition)
                Detail::Fail("IsFalse", message);
        }

        template <typename Expected, typename Actual>
        static void AreEqual(const Expected& expected, const Actual& actual, Detail::Message message = nullptr)
        {
            if (!(expected == actual))
                Detail::Fail("AreEqual", message);
        }

        // C strings by content, as MSTest compares them; ASCII only for ignoreCase.
        static void AreEqual(const char* expected, const char* actual, bool ignoreCase = false, Detail::Message message = nullptr)
        {
            if (expected == nullptr || actual == nullptr ? expected != actual
                                                         : (ignoreCase ? strcasecmp(expected, actual) : std::strcmp(expected, actual)) != 0)
                Detail::Fail("AreEqual", message);
        }

        template <typename Expected, typename Actual>
        static void AreNotEqual(const Expected& notExpected, const Actual& actual, Detail::Message message = nullptr)
        {
            if (notExpected == actual)
                Detail::Fail("AreNotEqual", message);
        }

        template <typename T>
        static void IsNull(const T* pointer, Detail::Message message = nullptr)
        {
            if (pointer != nullptr)
                Detail::Fail("IsNull", message);
        }

        template <typename T>
        static void IsNotNull(const T* pointer, Detail::Message message = nullptr)
        {
            if (pointer == nullptr)
                Detail::Fail("IsNotNull", message);
        }

        [[noreturn]] static void Fail(Detail::Message message = nullptr) { Detail::Fail("Fail", message); }
    };
} // namespace Microsoft::VisualStudio::CppUnitTestFramework

// The class name is read through the type, so the macro needs nothing from the class body. The
// method is declared first, so the registrar can name it; its body follows the macro. Invoke is
// a template, instantiated at the end of the translation unit, where the class is complete.
#define TEST_CLASS(className) class className : public ::Microsoft::VisualStudio::CppUnitTestFramework::TestClass<className>

#define TEST_METHOD(methodName)                                                                                          \
    struct methodName##_Tag {};                                                                                          \
    static inline const ::Microsoft::VisualStudio::CppUnitTestFramework::Detail::MethodRegistrar methodName##_registrar_{ \
        ::Microsoft::VisualStudio::CppUnitTestFramework::Detail::ClassName<ThisClass>(), #methodName,                      \
        &::Microsoft::VisualStudio::CppUnitTestFramework::Detail::InvokeMethod<ThisClass, methodName##_Tag>};             \
    void TestMethod(methodName##_Tag)

#define TEST_CLASS_INITIALIZE(methodName)                                                                                           \
    struct methodName##_Tag {};                                                                                                     \
    static inline const ::Microsoft::VisualStudio::CppUnitTestFramework::Detail::ClassInitializeRegistrar methodName##_registrar_{ \
        ::Microsoft::VisualStudio::CppUnitTestFramework::Detail::ClassName<ThisClass>(),                                            \
        &::Microsoft::VisualStudio::CppUnitTestFramework::Detail::InvokeClassHook<ThisClass, methodName##_Tag>};                    \
    static void ClassHook(methodName##_Tag)

#define TEST_CLASS_CLEANUP(methodName)                                                                                           \
    struct methodName##_Tag {};                                                                                                  \
    static inline const ::Microsoft::VisualStudio::CppUnitTestFramework::Detail::ClassCleanupRegistrar methodName##_registrar_{ \
        ::Microsoft::VisualStudio::CppUnitTestFramework::Detail::ClassName<ThisClass>(),                                         \
        &::Microsoft::VisualStudio::CppUnitTestFramework::Detail::InvokeClassHook<ThisClass, methodName##_Tag>};                 \
    static void ClassHook(methodName##_Tag)
