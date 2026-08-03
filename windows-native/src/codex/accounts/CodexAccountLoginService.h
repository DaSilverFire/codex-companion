#pragma once

#include "codex/accounts/CodexAccountProfile.h"
#include "codex/accounts/CodexAccountRuntime.h"
#include "core/Result.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace companion {

enum class
CodexAccountAuthenticationState {
    SignedIn,
    SignedOut,
    Unavailable,
};

struct CodexAccountAuthenticationStatus
    final {
    CodexAccountAuthenticationState state =
        CodexAccountAuthenticationState::
            Unavailable;
    QString message;
};

struct CodexLoginProcessRequest final {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
    bool interactive = false;
    bool createVisibleConsole = false;
};

struct CodexLoginProcessResult final {
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

class CodexAccountLoginService final {
public:
    using InteractiveCommand =
        std::function<Result<void>(
            const CodexLoginProcessRequest&)>;
    using StatusCommand =
        std::function<
            Result<
                CodexLoginProcessResult>(
                const CodexLoginProcessRequest&)>;

    CodexAccountLoginService(
        QString executable,
        QProcessEnvironment
            baseEnvironment,
        const CodexAccountRuntime&
            accountRuntime,
        InteractiveCommand
            interactiveCommand = {},
        StatusCommand statusCommand = {});

    Result<void> beginLogin(
        const CodexAccountProfile&
            profile) const;
    Result<void> beginLoginCurrentAccount()
        const;
    Result<
        CodexAccountAuthenticationStatus>
    readStatus(
        const CodexAccountProfile&
            profile) const;
    Result<
        CodexAccountAuthenticationStatus>
    readCurrentAccountStatus() const;

private:
    Result<CodexLoginProcessRequest>
    request(
        std::optional<CodexAccountProfile>
            profile,
        QStringList arguments,
        bool interactive) const;
    Result<
        CodexAccountAuthenticationStatus>
    readStatus(
        std::optional<CodexAccountProfile>
            profile) const;

    static Result<void>
    startInteractive(
        const CodexLoginProcessRequest&
            request);
    static Result<
        CodexLoginProcessResult>
    runStatus(
        const CodexLoginProcessRequest&
            request);
    static QString safeStatusMessage(
        const CodexLoginProcessResult&
            result);

    QString executable_;
    QProcessEnvironment
        baseEnvironment_;
    const CodexAccountRuntime*
        accountRuntime_ = nullptr;
    InteractiveCommand
        interactiveCommand_;
    StatusCommand statusCommand_;
};

} // namespace companion
