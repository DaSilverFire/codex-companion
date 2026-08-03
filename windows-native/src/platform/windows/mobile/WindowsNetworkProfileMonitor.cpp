#include "platform/windows/mobile/WindowsNetworkProfileMonitor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <netlistmgr.h>
#include <objbase.h>

#include <utility>

namespace companion {
namespace {

CompanionError networkError(
    QString code,
    QString message,
    QString operation,
    HRESULT hresult)
{
    return {
        std::move(code),
        std::move(message),
        false,
        QVariantMap{
            {QStringLiteral("operation"),
             std::move(operation)},
            {QStringLiteral("hresult"),
             static_cast<qlonglong>(hresult)},
        },
    };
}

template <typename T>
class ComPtr final {
public:
    ~ComPtr()
    {
        reset();
    }

    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put()
    {
        reset();
        return &value_;
    }

    T* get() const noexcept
    {
        return value_;
    }

    T* operator->() const noexcept
    {
        return value_;
    }

    void reset()
    {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

private:
    T* value_ = nullptr;
};

class NetworkListEventSink final
    : public INetworkListManagerEvents {
public:
    explicit NetworkListEventSink(
        std::function<void()> callback)
        : callback_(std::move(callback))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown
            || riid
                   == __uuidof(
                       INetworkListManagerEvents)) {
            *object =
                static_cast<
                    INetworkListManagerEvents*>(
                    this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs =
            InterlockedDecrement(&refs_);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE ConnectivityChanged(
        NLM_CONNECTIVITY) override
    {
        if (callback_) {
            callback_();
        }
        return S_OK;
    }

private:
    volatile LONG refs_ = 1;
    std::function<void()> callback_;
};

WindowsNetworkProfile mergeProfile(
    WindowsNetworkProfile current,
    NLM_NETWORK_CATEGORY category)
{
    if (category
        == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED) {
        return WindowsNetworkProfile::Domain;
    }
    if (category == NLM_NETWORK_CATEGORY_PRIVATE
        && current != WindowsNetworkProfile::Domain) {
        return WindowsNetworkProfile::Private;
    }
    if (category == NLM_NETWORK_CATEGORY_PUBLIC
        && current == WindowsNetworkProfile::Unavailable) {
        return WindowsNetworkProfile::Public;
    }
    return current;
}

class WindowsNativeNetworkProfileApi final
    : public IWindowsNetworkProfileApi {
public:
    WindowsNativeNetworkProfileApi()
    {
        const HRESULT initialized =
            CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
        comInitialized_ =
            SUCCEEDED(initialized);
        if (initialized == RPC_E_CHANGED_MODE) {
            comInitialized_ = false;
        }

        (void)CoCreateInstance(
            CLSID_NetworkListManager,
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(manager_.put()));
    }

    ~WindowsNativeNetworkProfileApi() override
    {
        unadvise();
        manager_.reset();
        if (comInitialized_) {
            CoUninitialize();
        }
    }

    Result<WindowsNetworkProfile> currentProfile()
        const override
    {
        if (manager_.get() == nullptr) {
            return Result<WindowsNetworkProfile>::
                failure(
                    networkError(
                        QStringLiteral(
                            "mobile.network.manager_unavailable"),
                        QStringLiteral(
                            "Windows network profile information is unavailable."),
                        QStringLiteral(
                            "CoCreateInstance"),
                        E_POINTER));
        }

        NLM_CONNECTIVITY connectivity =
            NLM_CONNECTIVITY_DISCONNECTED;
        HRESULT hr =
            manager_->GetConnectivity(
                &connectivity);
        if (FAILED(hr)) {
            return Result<WindowsNetworkProfile>::
                failure(
                    networkError(
                        QStringLiteral(
                            "mobile.network.connectivity_failed"),
                        QStringLiteral(
                            "Windows could not read network connectivity."),
                        QStringLiteral(
                            "INetworkListManager::GetConnectivity"),
                        hr));
        }
        if (connectivity
            == NLM_CONNECTIVITY_DISCONNECTED) {
            return Result<WindowsNetworkProfile>::
                success(
                    WindowsNetworkProfile::
                        Unavailable);
        }

        ComPtr<IEnumNetworks> networks;
        hr = manager_->GetNetworks(
            NLM_ENUM_NETWORK_CONNECTED,
            networks.put());
        if (FAILED(hr)) {
            return Result<WindowsNetworkProfile>::
                failure(
                    networkError(
                        QStringLiteral(
                            "mobile.network.enumeration_failed"),
                        QStringLiteral(
                            "Windows could not enumerate connected network profiles."),
                        QStringLiteral(
                            "INetworkListManager::GetNetworks"),
                        hr));
        }

        WindowsNetworkProfile result =
            WindowsNetworkProfile::Unavailable;
        while (true) {
            INetwork* rawNetwork = nullptr;
            ULONG fetched = 0;
            hr = networks->Next(
                1,
                &rawNetwork,
                &fetched);
            if (hr == S_FALSE || fetched == 0) {
                break;
            }
            if (FAILED(hr)) {
                return Result<WindowsNetworkProfile>::
                    failure(
                        networkError(
                            QStringLiteral(
                                "mobile.network.enumeration_failed"),
                            QStringLiteral(
                                "Windows could not enumerate connected network profiles."),
                            QStringLiteral(
                                "IEnumNetworks::Next"),
                            hr));
            }

            ComPtr<INetwork> network;
            *network.put() = rawNetwork;
            NLM_NETWORK_CATEGORY category =
                NLM_NETWORK_CATEGORY_PUBLIC;
            hr = network->GetCategory(
                &category);
            if (SUCCEEDED(hr)) {
                result =
                    mergeProfile(
                        result,
                        category);
            }
        }
        return Result<WindowsNetworkProfile>::success(
            result);
    }

    void setChangeCallback(
        std::function<void()> callback) override
    {
        unadvise();
        callback_ = std::move(callback);
        if (!callback_ || manager_.get() == nullptr) {
            return;
        }

        HRESULT hr =
            manager_->QueryInterface(
                IID_PPV_ARGS(points_.put()));
        if (FAILED(hr) || points_.get() == nullptr) {
            return;
        }
        hr = points_->FindConnectionPoint(
            __uuidof(INetworkListManagerEvents),
            connectionPoint_.put());
        if (FAILED(hr)
            || connectionPoint_.get() == nullptr) {
            return;
        }
        sink_ = new NetworkListEventSink(
            [this]() {
                if (callback_) {
                    callback_();
                }
            });
        hr = connectionPoint_->Advise(
            sink_,
            &adviseCookie_);
        if (FAILED(hr)) {
            sink_->Release();
            sink_ = nullptr;
            adviseCookie_ = 0;
        }
    }

private:
    void unadvise()
    {
        if (connectionPoint_.get() != nullptr
            && adviseCookie_ != 0) {
            (void)connectionPoint_->Unadvise(
                adviseCookie_);
        }
        adviseCookie_ = 0;
        connectionPoint_.reset();
        points_.reset();
        if (sink_ != nullptr) {
            sink_->Release();
            sink_ = nullptr;
        }
    }

    bool comInitialized_ = false;
    ComPtr<INetworkListManager> manager_;
    ComPtr<IConnectionPointContainer> points_;
    ComPtr<IConnectionPoint> connectionPoint_;
    DWORD adviseCookie_ = 0;
    NetworkListEventSink* sink_ = nullptr;
    std::function<void()> callback_;
};

} // namespace

WindowsNetworkProfileMonitor::
    WindowsNetworkProfileMonitor(
        IWindowsNetworkProfileApi* api,
        QObject* parent)
    : QObject(parent),
      ownedApi_(api == nullptr
                    ? std::make_unique<
                          WindowsNativeNetworkProfileApi>()
                    : nullptr),
      api_(api == nullptr ? ownedApi_.get() : api)
{
    qRegisterMetaType<WindowsNetworkProfile>(
        "WindowsNetworkProfile");
    const auto current = api_->currentProfile();
    if (current.hasValue()) {
        profile_ = current.value();
    }
    api_->setChangeCallback([this]() {
        (void)refresh();
    });
}

WindowsNetworkProfileMonitor::
    ~WindowsNetworkProfileMonitor()
{
    if (api_ != nullptr) {
        api_->setChangeCallback(nullptr);
    }
}

WindowsNetworkProfile
WindowsNetworkProfileMonitor::profile()
    const noexcept
{
    return profile_;
}

Result<WindowsNetworkProfile>
WindowsNetworkProfileMonitor::refresh()
{
    const auto current = api_->currentProfile();
    if (!current.hasValue()) {
        return current;
    }
    if (current.value() != profile_) {
        profile_ = current.value();
        emit profileChanged(profile_);
    }
    return current;
}

} // namespace companion
