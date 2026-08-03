#include "platform/windows/mobile/WindowsDnsSdAdvertiser.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <WinDNS.h>

#include <QSysInfo>

#include <array>
#include <utility>

namespace companion {
namespace {

CompanionError dnsSdError(
    QString code,
    QString message,
    QString operation,
    DWORD windowsError = ERROR_SUCCESS)
{
    QVariantMap context{
        {QStringLiteral("operation"),
         std::move(operation)},
    };
    if (windowsError != ERROR_SUCCESS) {
        context.insert(
            QStringLiteral("windowsError"),
            static_cast<qulonglong>(
                windowsError));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

bool canAdvertise(
    WindowsNetworkProfile profile,
    bool allowPublicNetwork) noexcept
{
    return profile == WindowsNetworkProfile::Private
        || profile == WindowsNetworkProfile::Domain
        || (profile == WindowsNetworkProfile::Public
            && allowPublicNetwork);
}

std::wstring wide(
    const QString& text)
{
    return text.toStdWString();
}

QString defaultHostName()
{
    QString host = QSysInfo::machineHostName()
                       .trimmed();
    if (host.isEmpty()) {
        host = QStringLiteral("localhost");
    }
    if (!host.endsWith(QStringLiteral(".local"),
                       Qt::CaseInsensitive)) {
        host += QStringLiteral(".local");
    }
    return host;
}

class WindowsNativeDnsSdApi final
    : public IWindowsDnsSdApi {
public:
    ~WindowsNativeDnsSdApi() override
    {
        if (active_.has_value()) {
            (void)deregisterService(
                active_->service);
        }
    }

    Result<void> registerService(
        const WindowsDnsSdService& service) override
    {
        if (active_.has_value()) {
            const auto stopped =
                deregisterService(active_->service);
            if (!stopped.hasValue()) {
                return stopped;
            }
        }

        auto registration =
            makeRegistration(service);
        if (!registration.hasValue()) {
            return Result<void>::failure(
                registration.error());
        }

        const DWORD status =
            DnsServiceRegister(
                &registration.value().request,
                nullptr);
        if (!dnsRequestAccepted(status)) {
            return Result<void>::failure(
                dnsSdError(
                    QStringLiteral(
                        "mobile.dnssd.register_failed"),
                    QStringLiteral(
                        "Windows could not advertise nearby discovery."),
                    QStringLiteral(
                        "DnsServiceRegister"),
                    status));
        }
        active_ = std::move(registration.value());
        return Result<void>::success();
    }

    Result<void> deregisterService(
        const WindowsDnsSdService& service) override
    {
        if (!active_.has_value()) {
            return Result<void>::success();
        }
        if (!(active_->service == service)) {
            return Result<void>::failure(
                dnsSdError(
                    QStringLiteral(
                        "mobile.dnssd.registration_mismatch"),
                    QStringLiteral(
                        "Windows nearby discovery registration changed before withdrawal."),
                    QStringLiteral(
                        "DnsServiceDeRegister")));
        }

        const DWORD status =
            DnsServiceDeRegister(
                &active_->request,
                nullptr);
        active_.reset();
        if (!dnsRequestAccepted(status)) {
            return Result<void>::failure(
                dnsSdError(
                    QStringLiteral(
                        "mobile.dnssd.deregister_failed"),
                    QStringLiteral(
                        "Windows could not withdraw nearby discovery."),
                    QStringLiteral(
                        "DnsServiceDeRegister"),
                    status));
        }
        return Result<void>::success();
    }

private:
    struct NativeRegistration final {
        ~NativeRegistration()
        {
            if (instance != nullptr) {
                DnsServiceFreeInstance(instance);
            }
        }

        NativeRegistration() = default;
        NativeRegistration(const NativeRegistration&) =
            delete;
        NativeRegistration& operator=(
            const NativeRegistration&) = delete;

        NativeRegistration(
            NativeRegistration&& other) noexcept
            : service(std::move(other.service)),
              instance(std::exchange(
                  other.instance,
                  nullptr)),
              request(other.request)
        {
            request.pServiceInstance = instance;
        }

        NativeRegistration& operator=(
            NativeRegistration&& other) noexcept
        {
            if (this != &other) {
                if (instance != nullptr) {
                    DnsServiceFreeInstance(instance);
                }
                service = std::move(other.service);
                instance = std::exchange(
                    other.instance,
                    nullptr);
                request = other.request;
                request.pServiceInstance = instance;
            }
            return *this;
        }

        WindowsDnsSdService service;
        PDNS_SERVICE_INSTANCE instance = nullptr;
        DNS_SERVICE_REGISTER_REQUEST request{};
    };

    static void WINAPI registrationComplete(
        DWORD,
        PVOID,
        PDNS_SERVICE_INSTANCE instance)
    {
        if (instance != nullptr) {
            DnsServiceFreeInstance(instance);
        }
    }

    static bool dnsRequestAccepted(
        DWORD status) noexcept
    {
        return status == ERROR_SUCCESS
            || status == DNS_REQUEST_PENDING;
    }

    static Result<NativeRegistration> makeRegistration(
        const WindowsDnsSdService& service)
    {
        const QString fullName =
            service.instanceName
            + QStringLiteral(".")
            + service.serviceType;
        const QString hostName =
            service.hostName.trimmed().isEmpty()
            ? defaultHostName()
            : service.hostName.trimmed();

        const std::wstring fullNameWide =
            wide(fullName);
        const std::wstring hostNameWide =
            wide(hostName);

        QVector<std::wstring> keyStorage;
        QVector<std::wstring> valueStorage;
        keyStorage.reserve(service.txt.size());
        valueStorage.reserve(service.txt.size());
        for (auto iterator = service.txt.cbegin();
             iterator != service.txt.cend();
             ++iterator) {
            keyStorage.append(
                wide(iterator.key()));
            valueStorage.append(
                wide(iterator.value()));
        }

        QVector<PCWSTR> keys;
        QVector<PCWSTR> values;
        keys.reserve(keyStorage.size());
        values.reserve(valueStorage.size());
        for (const auto& key : std::as_const(
                 keyStorage)) {
            keys.append(key.c_str());
        }
        for (const auto& value : std::as_const(
                 valueStorage)) {
            values.append(value.c_str());
        }

        PDNS_SERVICE_INSTANCE instance =
            DnsServiceConstructInstance(
                fullNameWide.c_str(),
                hostNameWide.c_str(),
                nullptr,
                nullptr,
                service.port,
                0,
                0,
                static_cast<DWORD>(keys.size()),
                keys.isEmpty() ? nullptr : keys.data(),
                values.isEmpty() ? nullptr
                                 : values.data());
        if (instance == nullptr) {
            return Result<NativeRegistration>::failure(
                dnsSdError(
                    QStringLiteral(
                        "mobile.dnssd.instance_failed"),
                    QStringLiteral(
                        "Windows could not prepare nearby discovery metadata."),
                    QStringLiteral(
                        "DnsServiceConstructInstance"),
                    GetLastError()));
        }
        instance->dwInterfaceIndex =
            service.interfaceIndex;

        DNS_SERVICE_REGISTER_REQUEST request{};
        request.Version = 1;
        request.InterfaceIndex =
            service.interfaceIndex;
        request.pServiceInstance = instance;
        request.pRegisterCompletionCallback =
            &registrationComplete;
        request.unicastEnabled = FALSE;

        NativeRegistration registration;
        registration.service = service;
        registration.instance = instance;
        registration.request = request;
        return Result<NativeRegistration>::success(
            std::move(registration));
    }

    std::optional<NativeRegistration> active_;
};

} // namespace

WindowsDnsSdAdvertiser::WindowsDnsSdAdvertiser(
    IWindowsDnsSdApi* api)
    : ownedApi_(api == nullptr
                    ? std::make_unique<
                          WindowsNativeDnsSdApi>()
                    : nullptr),
      api_(api == nullptr ? ownedApi_.get() : api)
{
}

WindowsDnsSdAdvertiser::~WindowsDnsSdAdvertiser()
{
    (void)stop();
}

Result<void> WindowsDnsSdAdvertiser::update(
    const WindowsDnsSdAdvertisement& advertisement)
{
    if (!canAdvertise(
            advertisement.networkProfile,
            advertisement.allowPublicNetwork)) {
        return stop();
    }
    if (advertisement.port == 0) {
        return Result<void>::failure(
            dnsSdError(
                QStringLiteral(
                    "mobile.dnssd.invalid_port"),
                QStringLiteral(
                    "Nearby discovery requires a listening port."),
                QStringLiteral(
                    "WindowsDnsSdAdvertiser::update")));
    }

    const auto txt =
        txtRecord(
            advertisement.installationId,
            advertisement.tlsFingerprintSha256);
    if (!txt.hasValue()) {
        return Result<void>::failure(txt.error());
    }

    WindowsDnsSdService service{
        instanceNameForComputerName(
            advertisement.computerName),
        serviceType(),
        advertisement.hostName,
        advertisement.port,
        txt.value(),
        advertisement.interfaceIndex,
    };

    if (active_.has_value()
        && active_.value() == service) {
        return Result<void>::success();
    }
    if (active_.has_value()) {
        const auto stopped =
            api_->deregisterService(*active_);
        if (!stopped.hasValue()) {
            active_.reset();
            return stopped;
        }
        active_.reset();
    }

    const auto started =
        api_->registerService(service);
    if (!started.hasValue()) {
        return started;
    }
    active_ = std::move(service);
    return Result<void>::success();
}

Result<void> WindowsDnsSdAdvertiser::stop()
{
    if (!active_.has_value()) {
        return Result<void>::success();
    }
    const WindowsDnsSdService prior = *active_;
    active_.reset();
    return api_->deregisterService(prior);
}

bool WindowsDnsSdAdvertiser::isAdvertising()
    const noexcept
{
    return active_.has_value();
}

std::optional<WindowsDnsSdService>
WindowsDnsSdAdvertiser::activeService() const
{
    return active_;
}

QString WindowsDnsSdAdvertiser::serviceType()
{
    return QStringLiteral(
        "_codex-companion._tcp.local");
}

QString WindowsDnsSdAdvertiser::fallbackInstanceName()
{
    return QStringLiteral(
        "Codex Companion Windows");
}

QString
WindowsDnsSdAdvertiser::instanceNameForComputerName(
    const QString& computerName)
{
    const QString trimmed = computerName.trimmed();
    if (trimmed.isEmpty()) {
        return fallbackInstanceName();
    }

    QString result;
    for (const QChar character : trimmed) {
        const QString candidate = result + character;
        if (candidate.toUtf8().size() > 63) {
            break;
        }
        result = candidate;
    }
    if (result.trimmed().isEmpty()) {
        return fallbackInstanceName();
    }
    return result;
}

Result<QMap<QString, QString>>
WindowsDnsSdAdvertiser::txtRecord(
    const QString& installationId,
    const QString& tlsFingerprintSha256)
{
    if (installationId.trimmed().isEmpty()) {
        return Result<QMap<QString, QString>>::failure(
            dnsSdError(
                QStringLiteral(
                    "mobile.dnssd.invalid_installation"),
                QStringLiteral(
                    "Nearby discovery requires an installation identifier."),
                QStringLiteral(
                    "WindowsDnsSdAdvertiser::txtRecord")));
    }
    const QString fingerprint =
        tlsFingerprintSha256.trimmed();
    if (fingerprint.size() != 64
        || fingerprint != fingerprint.toLower()) {
        return Result<QMap<QString, QString>>::failure(
            dnsSdError(
                QStringLiteral(
                    "mobile.dnssd.invalid_tls_fingerprint"),
                QStringLiteral(
                    "Nearby discovery requires a lowercase certificate fingerprint."),
                QStringLiteral(
                    "WindowsDnsSdAdvertiser::txtRecord")));
    }
    for (const QChar character : fingerprint) {
        const ushort value = character.unicode();
        if (!((value >= '0' && value <= '9')
              || (value >= 'a' && value <= 'f'))) {
            return Result<QMap<QString, QString>>::
                failure(
                    dnsSdError(
                        QStringLiteral(
                            "mobile.dnssd.invalid_tls_fingerprint"),
                        QStringLiteral(
                            "Nearby discovery requires a lowercase certificate fingerprint."),
                        QStringLiteral(
                            "WindowsDnsSdAdvertiser::txtRecord")));
        }
    }

    QMap<QString, QString> txt{
        {QStringLiteral("pv"), QStringLiteral("1")},
        {QStringLiteral("id"),
         installationId.trimmed()},
        {QStringLiteral("transport"),
         QStringLiteral("wss")},
        {QStringLiteral("path"),
         QStringLiteral("/companion/v1")},
        {QStringLiteral("frame"),
         QStringLiteral("1")},
        {QStringLiteral("tlsfp"), fingerprint},
    };
    for (auto iterator = txt.cbegin();
         iterator != txt.cend();
         ++iterator) {
        const qsizetype itemBytes =
            iterator.key().toUtf8().size() + 1
            + iterator.value().toUtf8().size();
        if (itemBytes > 255) {
            return Result<QMap<QString, QString>>::
                failure(
                    dnsSdError(
                        QStringLiteral(
                            "mobile.dnssd.txt_too_large"),
                        QStringLiteral(
                            "Nearby discovery metadata is too large for DNS-SD."),
                        QStringLiteral(
                            "WindowsDnsSdAdvertiser::txtRecord")));
        }
    }
    return Result<QMap<QString, QString>>::success(
        std::move(txt));
}

} // namespace companion
