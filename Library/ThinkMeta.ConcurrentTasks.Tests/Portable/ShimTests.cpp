//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Tests of the portable test shim itself (CppUnitTest.h, TestMain.cpp): the assertions pass and
// fail as under MSTest, a failed assertion ends only its own method, the class initialize runs
// once before the methods, and every method gets a new instance of its class. Linux only; under
// Visual Studio MSTest is the framework.
//

#include "CppUnitTest.h"

#include <cstddef>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests::Shim {
    namespace {
        int initializeCalls = 0;
        int instances       = 0;

        // Runs an assertion that must fail; true if it did, by throwing AssertFailed.
        template <typename Assertion>
        bool Fails(Assertion assertion)
        {
            try {
                assertion();
            } catch (const AssertFailed&) {
                return true;
            }
            return false;
        }
    } // namespace

    TEST_CLASS(ShimTests)
    {
    public:
        ShimTests() : serial_(++instances) {}

        TEST_CLASS_INITIALIZE(Initialize) { ++initializeCalls; }

        TEST_METHOD(AssertionsThatHoldPass)
        {
            int  value = 0;
            int* none  = nullptr;

            Assert::IsTrue(true, L"IsTrue(true)");
            Assert::IsFalse(false);
            Assert::AreEqual(42, 42, L"int");
            Assert::AreEqual(std::size_t{3}, std::size_t{3});
            Assert::AreEqual(std::wstring(L"a"), std::wstring(L"a"));
            Assert::AreNotEqual(1, 2);
            Assert::IsNotNull(&value);
            Assert::IsNull(none);
            Logger::WriteMessage(L"a wide message");
            Logger::WriteMessage("a narrow message");
        }

        TEST_METHOD(AssertionsThatDoNotHoldThrow)
        {
            int  value = 0;
            int* none  = nullptr;

            Logger::WriteMessage("six assertion failures follow on purpose");
            Assert::IsTrue(Fails([] { Assert::IsTrue(false, L"(expected)"); }), L"IsTrue(false) did not fail");
            Assert::IsTrue(Fails([] { Assert::IsFalse(true, L"(expected)"); }), L"IsFalse(true) did not fail");
            Assert::IsTrue(Fails([] { Assert::AreEqual(1, 2, L"(expected)"); }), L"AreEqual(1, 2) did not fail");
            Assert::IsTrue(Fails([&] { Assert::IsNull(&value, L"(expected)"); }), L"IsNull(&value) did not fail");
            Assert::IsTrue(Fails([&] { Assert::IsNotNull(none, L"(expected)"); }), L"IsNotNull(nullptr) did not fail");
            Assert::IsTrue(Fails([] { Assert::Fail(L"(expected)"); }), L"Fail() did not fail");
        }

        TEST_METHOD(ClassInitializeRanOnceBeforeTheMethods) { Assert::AreEqual(1, initializeCalls, L"the class initialize did not run exactly once"); }

        TEST_METHOD(EveryMethodGetsItsOwnInstance)
        {
            // the newest instance is this method's own, also when --filter runs only some methods;
            // the methods before it each made one more
            Assert::AreEqual(instances, serial_, L"the method does not run on an instance of its own");
            Assert::IsTrue(instances >= 1, L"no instance was made");
        }

    private:
        int serial_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests::Shim
