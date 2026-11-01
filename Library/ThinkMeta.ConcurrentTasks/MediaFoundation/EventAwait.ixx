//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Await: waits for the next event of a Media Foundation event generator (asynchronous transforms,
// media sources, media sessions) by suspending the calling task instead of blocking its thread.
// Uses only the interfaces from mfobjects.h; the caller links Media Foundation as it needs.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <new>

#include <Windows.h>

#include <mfobjects.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.MediaFoundation.Await;

import ThinkMeta.ConcurrentTasks.Synchronization.Await;

namespace ThinkMeta::ConcurrentTasks::MediaFoundation {
    // The callback of BeginGetEvent. Media Foundation releases it after Invoke has returned, so it
    // lives on the heap; Invoke touches the waiting task's stack only before the completion.
    class EventCallback final : public IMFAsyncCallback
    {
    public:
        EventCallback(IMFMediaEventGenerator* generator, IMFMediaEvent** event, HRESULT* result) noexcept
            : generator_(generator),
              event_(event),
              result_(result)
        {}

        void Start(Synchronization::Completion& completion) noexcept { completion_ = &completion; }

        STDMETHODIMP QueryInterface(REFIID iid, void** object) noexcept override
        {
            if (object == nullptr)
                return E_POINTER;
            // IsEqualGUID, not ==: callers may define _NO_SYS_GUID_OPERATOR_EQ_
            if (IsEqualGUID(iid, __uuidof(IUnknown)) || IsEqualGUID(iid, __uuidof(IMFAsyncCallback))) {
                *object = static_cast<IMFAsyncCallback*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() noexcept override { return InterlockedIncrement(&refs_); }

        STDMETHODIMP_(ULONG) Release() noexcept override
        {
            const auto refs = InterlockedDecrement(&refs_);
            if (refs == 0)
                delete this;
            return refs;
        }

        STDMETHODIMP GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }

        STDMETHODIMP Invoke(IMFAsyncResult* asyncResult) noexcept override
        {
            *result_ = generator_->EndGetEvent(asyncResult, event_);
            (*completion_)(); // last access to the waiting task
            return S_OK;
        }

    private:
        LONG                         refs_ = 1;
        IMFMediaEventGenerator*      generator_;
        IMFMediaEvent**              event_;
        HRESULT*                     result_;
        Synchronization::Completion* completion_ = nullptr;
    };

    /// <summary>Waits for the next event of a Media Foundation event generator.</summary>
    /// <param name="generator">
    /// The generator, for example an asynchronous transform (<c>IMFTransform</c> queried for
    /// <c>IMFMediaEventGenerator</c>), a media source or a media session.
    /// </param>
    /// <param name="event">Receives the event; release it after use.</param>
    /// <returns>
    /// <c>S_OK</c>, or the error of <c>BeginGetEvent</c> or <c>EndGetEvent</c>, for example
    /// <c>MF_E_SHUTDOWN</c> once the generator has been shut down.
    /// </returns>
    /// <remarks>
    /// Suspends the calling task until the generator queues an event; the scheduler thread runs
    /// other tasks meanwhile. The asynchronous counterpart of <c>IMFMediaEventGenerator::GetEvent</c>,
    /// which would block the thread.
    /// A generator accepts one outstanding request: wait for its events from one task at a time.
    /// Task or converted thread only. There is no timed variant, and shutting the generator down does
    /// not end a pending wait: wait only for events the generator will send, for example
    /// <c>METransformDrainComplete</c> after <c>MFT_MESSAGE_COMMAND_DRAIN</c>. After a shutdown, the
    /// function returns <c>MF_E_SHUTDOWN</c> at once.
    /// Needs the multithreaded apartment and a started Media Foundation (<c>MFStartup</c>).
    /// </remarks>
    /// <seealso cref="Synchronization::Await"/>
    export inline HRESULT Await(IMFMediaEventGenerator* generator, IMFMediaEvent** event) noexcept
    {
        if (event == nullptr)
            return E_POINTER;
        *event = nullptr;

        HRESULT result   = S_OK;
        auto*   callback = new (std::nothrow) EventCallback{generator, event, &result};
        if (callback == nullptr)
            return E_OUTOFMEMORY;

        Synchronization::Await([&](Synchronization::Completion& completion) noexcept {
            callback->Start(completion);
            const HRESULT hr = generator->BeginGetEvent(callback, nullptr);
            if (FAILED(hr)) {
                result = hr;
                completion();
            }
        });

        callback->Release();
        return result;
    }
} // namespace ThinkMeta::ConcurrentTasks::MediaFoundation
