#pragma once

#include "core/Result.h"

#include <QObject>

#include <functional>
#include <memory>

namespace companion {

enum class WindowsNetworkProfile {
    Unavailable,
    Public,
    Private,
    Domain,
};

class IWindowsNetworkProfileApi {
public:
    virtual ~IWindowsNetworkProfileApi() = default;

    virtual Result<WindowsNetworkProfile>
    currentProfile() const = 0;

    virtual void setChangeCallback(
        std::function<void()> callback) = 0;
};

class WindowsNetworkProfileMonitor final
    : public QObject {
    Q_OBJECT

public:
    explicit WindowsNetworkProfileMonitor(
        IWindowsNetworkProfileApi* api = nullptr,
        QObject* parent = nullptr);
    ~WindowsNetworkProfileMonitor() override;

    WindowsNetworkProfile profile() const noexcept;
    Result<WindowsNetworkProfile> refresh();

signals:
    void profileChanged(
        companion::WindowsNetworkProfile profile);

private:
    std::unique_ptr<IWindowsNetworkProfileApi>
        ownedApi_;
    IWindowsNetworkProfileApi* api_ = nullptr;
    WindowsNetworkProfile profile_ =
        WindowsNetworkProfile::Unavailable;
};

} // namespace companion

Q_DECLARE_METATYPE(companion::WindowsNetworkProfile)
