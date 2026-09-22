#include "backend/audiosharing/SystemAudioCapture_windows.cpp"
#include <QtTest>

namespace {
class ActivationResult final : public IActivateAudioInterfaceAsyncOperation {
public:
    HRESULT retrieval = S_OK, activation = E_ACCESSDENIED;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT* result, IUnknown** audio) override {
        *result = activation; *audio = nullptr; return retrieval;
    }
};
}
class WindowsAudioActivationTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))); }
    void cleanupTestCase() { CoUninitialize(); }
    void completionSupportsAgileMarshaling() {
        auto* completion = new Completion;
        QVERIFY(SUCCEEDED(completion->marshalerResult));
        Com<IUnknown> agile, marshal;
        QCOMPARE(completion->QueryInterface(__uuidof(IAgileObject), reinterpret_cast<void**>(agile.put())), S_OK);
        QCOMPARE(completion->QueryInterface(__uuidof(IMarshal), reinterpret_cast<void**>(marshal.put())), S_OK);
        completion->Release();
    }
    void cancellationRetainsParametersUntilLateCompletion() {
        auto* completion = new Completion;
        completion->AddRef(); // Windows retains the callback for its async work.
        auto* activation = &completion->activation;
        const HANDLE event = completion->event;
        completion->Release(); // The capture thread times out or is stopped.
        QCOMPARE(activation->vt, VARTYPE(VT_BLOB));
        QCOMPARE(activation->blob.cbSize, ULONG(sizeof(AUDIOCLIENT_ACTIVATION_PARAMS)));
        const auto* parameters = reinterpret_cast<const AUDIOCLIENT_ACTIVATION_PARAMS*>(activation->blob.pBlobData);
        QCOMPARE(parameters->ProcessLoopbackParams.TargetProcessId, GetCurrentProcessId());
        QCOMPARE(parameters->ProcessLoopbackParams.ProcessLoopbackMode, PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE);
        ActivationResult operation;
        QCOMPARE(completion->ActivateCompleted(&operation), S_OK);
        QCOMPARE(WaitForSingleObject(event, 0), DWORD(WAIT_OBJECT_0));
        QCOMPARE(completion->result, E_ACCESSDENIED);
        completion->Release();
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(event, &flags));
    }
    void malformedSuccessfulActivationDoesNotDereferenceNull() {
        auto* completion = new Completion;
        ActivationResult operation;
        operation.activation = S_OK;
        QCOMPARE(completion->ActivateCompleted(&operation), S_OK);
        QCOMPARE(completion->result, E_POINTER);
        completion->Release();
    }
    void retrievalFailurePreservesItsHresult() {
        auto* completion = new Completion;
        ActivationResult operation;
        operation.retrieval = E_INVALIDARG;
        QCOMPARE(completion->ActivateCompleted(&operation), S_OK);
        QCOMPARE(completion->result, E_INVALIDARG);
        QVERIFY(describe("activation", E_INVALIDARG).contains("0x80070057"));
        completion->Release();
    }
};
QTEST_GUILESS_MAIN(WindowsAudioActivationTest)
#include "tst_WindowsAudioActivation.moc"
