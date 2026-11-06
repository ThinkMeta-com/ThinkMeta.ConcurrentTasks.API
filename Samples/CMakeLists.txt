# The API samples that run on Linux (clang, Ninja): the documentation samples of the website
# (Docs/*, whose marked sections the website shows) and the small stand-alone samples. Windows
# builds use the .vcxproj files next to each sample; this file never takes part in them.
#
# Built through the core's CMakeLists.txt or the API's own (CT_BUILD_SAMPLES, on by default). Each
# sample is also a test: ctest runs it and expects exit code 0, as every sample checks its own
# results. Windows only, not here: BluetoothDevices, GuideUiThread, GuideThreadCallbacks, Ui,
# Capture, Bluetooth, Hashing (both use BCrypt) and the encoder samples.

# One program from one source file, linked as the unit tests are: the library as a target, the
# core as a file (see ct_add_tests in Library/ThinkMeta.ConcurrentTasks.Tests/CMakeLists.txt).
# Builds the core first when it is built here; a prebuilt core (IMPORTED) needs nothing. The same
# function as in the unit tests' CMakeLists.txt, for builds without the tests.
if(NOT COMMAND ct_depend_on_core)
    function(ct_depend_on_core name)
        get_target_property(imported ThinkMeta.ConcurrentTasks.Core IMPORTED)
        if(NOT imported)
            add_dependencies(${name} ThinkMeta.ConcurrentTasks.Core)
        endif()
    endfunction()
endif()

function(ct_add_sample name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ThinkMeta::ConcurrentTasks $<TARGET_LINKER_FILE:ThinkMeta.ConcurrentTasks.Core> pthread)
    ct_depend_on_core(${name})
    set_target_properties(${name} PROPERTIES BUILD_RPATH $<TARGET_FILE_DIR:ThinkMeta.ConcurrentTasks.Core>)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 LABELS sample)
endfunction()

set(CT_DOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Docs")

# group A of phase 7: no platform code
ct_add_sample(Sample.HelloTask             ${CT_DOCS_DIR}/HelloTask/hello-task.cpp)
ct_add_sample(Sample.PingPong              ${CT_DOCS_DIR}/PingPong/ping-pong.cpp)
ct_add_sample(Sample.ProducerConsumer      ${CT_DOCS_DIR}/ProducerConsumer/producer-consumer.cpp)
ct_add_sample(Sample.Timeout               ${CT_DOCS_DIR}/Timeout/timeout.cpp)
ct_add_sample(Sample.WaitForAGroup         ${CT_DOCS_DIR}/WaitForAGroup/wait-for-a-group.cpp)
ct_add_sample(Sample.GuideCustomPrimitives ${CT_DOCS_DIR}/GuideCustomPrimitives/guide-custom-primitives.cpp)
ct_add_sample(Sample.GuideEvents           ${CT_DOCS_DIR}/GuideEvents/guide-events.cpp)
ct_add_sample(Sample.GuideQueuesStacks     ${CT_DOCS_DIR}/GuideQueuesStacks/guide-queues-stacks.cpp)
ct_add_sample(Sample.GuideSemaphoresLocks  ${CT_DOCS_DIR}/GuideSemaphoresLocks/guide-semaphores-locks.cpp)
ct_add_sample(Sample.GuideTimeouts         ${CT_DOCS_DIR}/GuideTimeouts/guide-timeouts.cpp)
ct_add_sample(Sample.GuideWaitingForTasks  ${CT_DOCS_DIR}/GuideWaitingForTasks/guide-waiting-for-tasks.cpp)
ct_add_sample(Sample.PingPongStandalone    ${CMAKE_CURRENT_SOURCE_DIR}/ThinkMeta.ConcurrentTasks.Samples.PingPong/ThinkMeta.ConcurrentTasks.Samples.PingPong.cpp)
# a benchmark of 10 million round trips: minutes under the sanitizers
set_tests_properties(Sample.PingPongStandalone PROPERTIES TIMEOUT 900)

# group B: the tutorial chapters, one program each, with the shared tutorial-common.h. ctest runs
# them as a chain: make an input, pack it with chapter 1 (the sequential reference), then every
# later chapter packs the same input and compares its output with chapter 1's (-r).
set(CT_TUTORIAL_DIR "${CMAKE_CURRENT_BINARY_DIR}/tutorial")
file(MAKE_DIRECTORY "${CT_TUTORIAL_DIR}")

function(ct_add_tutorial number name source)
    set(target Sample.Tutorial${number})
    add_executable(${target} ${CT_DOCS_DIR}/Tutorial${number}${name}/${source})
    target_include_directories(${target} PRIVATE ${CT_DOCS_DIR}/TutorialCommon)
    target_link_libraries(${target} PRIVATE ThinkMeta::ConcurrentTasks $<TARGET_LINKER_FILE:ThinkMeta.ConcurrentTasks.Core> pthread)
    ct_depend_on_core(${target})
    set_target_properties(${target} PROPERTIES BUILD_RPATH $<TARGET_FILE_DIR:ThinkMeta.ConcurrentTasks.Core>)
    if(number STREQUAL "01")
        add_test(NAME Sample.TutorialInput COMMAND ${target} --make-input ${CT_TUTORIAL_DIR}/input.bin 16)
        set_tests_properties(Sample.TutorialInput PROPERTIES FIXTURES_SETUP TutorialInput LABELS sample)
        add_test(NAME ${target} COMMAND ${target} ${CT_TUTORIAL_DIR}/input.bin ${CT_TUTORIAL_DIR}/reference.bin)
        set_tests_properties(${target} PROPERTIES FIXTURES_REQUIRED TutorialInput FIXTURES_SETUP TutorialReference)
    else()
        add_test(NAME ${target} COMMAND ${target} ${CT_TUTORIAL_DIR}/input.bin ${CT_TUTORIAL_DIR}/out${number}.bin
                 -r ${CT_TUTORIAL_DIR}/reference.bin)
        set_tests_properties(${target} PROPERTIES FIXTURES_REQUIRED "TutorialInput;TutorialReference")
    endif()
    set_tests_properties(${target} PROPERTIES TIMEOUT 300 LABELS sample)
endfunction()

ct_add_tutorial(01 Sequential  tutorial-01-sequential.cpp)
ct_add_tutorial(02 Scheduler   tutorial-02-scheduler.cpp)
ct_add_tutorial(03 ForkJoin    tutorial-03-fork-join.cpp)
ct_add_tutorial(04 Sizing      tutorial-04-sizing.cpp)
ct_add_tutorial(05 AsyncRead   tutorial-05-async-read.cpp)
ct_add_tutorial(06 Pipeline    tutorial-06-pipeline.cpp)
ct_add_tutorial(07 SerialState tutorial-07-serial-state.cpp)
ct_add_tutorial(08 Priorities  tutorial-08-priorities.cpp)
ct_add_tutorial(09 Shutdown    tutorial-09-shutdown.cpp)
ct_add_tutorial(10 ReadyMade   tutorial-10-ready-made.cpp)

# group C: asynchronous files; each works on files of its own in the temp directory
ct_add_sample(Sample.AsyncFileCopy       ${CT_DOCS_DIR}/AsyncFileCopy/async-file-copy.cpp)
ct_add_sample(Sample.GuideAsyncFiles     ${CT_DOCS_DIR}/GuideAsyncFiles/guide-async-files.cpp)
ct_add_sample(Sample.GuidePipes          ${CT_DOCS_DIR}/GuidePipes/guide-pipes.cpp)
ct_add_sample(Sample.GuideStreamingFiles ${CT_DOCS_DIR}/GuideStreamingFiles/guide-streaming-files.cpp)
ct_add_sample(Sample.GuideTaskGraphs     ${CT_DOCS_DIR}/GuideTaskGraphs/guide-task-graphs.cpp)
ct_add_sample(Sample.OrderedOutput       ${CT_DOCS_DIR}/OrderedOutput/ordered-output.cpp)

# group D: TCP sockets on 127.0.0.1 with ephemeral ports
ct_add_sample(Sample.GuideTcpSockets   ${CT_DOCS_DIR}/GuideTcpSockets/guide-tcp-sockets.cpp)
ct_add_sample(Sample.TcpEchoServer     ${CT_DOCS_DIR}/TcpEchoServer/tcp-echo-server.cpp)
ct_add_sample(Sample.GuideCancellation ${CT_DOCS_DIR}/GuideCancellation/guide-cancellation.cpp)
