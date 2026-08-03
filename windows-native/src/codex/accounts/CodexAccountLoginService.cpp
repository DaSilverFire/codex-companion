#include "codex/accounts/CodexAccountLoginService.h"

#include <QProcess>
#include <QRegularExpression>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <utility>

namespace companion {
namespace {

constexpr int kProcessStartTimeoutMilliseconds =
    5'000;
constexpr int kProcessFinishTimeoutMilliseconds =
    15'000;

CompanionError loginError(
    QString code,
    QString message,
    const QString& program,
    QString detail = {})
{
    QVariantMap context{
        {
            QStringLiteral("program"),
            program,
        },
    };
    if (!detail.isEmpty()) {
        context.insert(
            QStringLiteral("detail"),
            std::move(detail));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

} // namespace

CodexAccountLoginService::
    CodexAccountLoginService(
        QString executable,
        QProcessEnvironment
            baseEnvironment,
        const CodexAccountRuntime&
            accountRuntime,
        InteractiveCommand
            interactiveCommand,
        StatusCommand statusCommand)
    : executable_(
          std::move(executable)),
      baseEnvironment_(
          std::move(baseEnvironment)),
      accountRuntime_(
          &accountRuntime),
      interactiveCommand_(
          interactiveCommand
              ? std::move(
                    interactiveCommand)
              : &CodexAccountLoginService::
                    startInteractive),
      statusCommand_(
          statusCommand
              ? std::move(statusCommand)
              : &CodexAccountLoginService::
                    runStatus)
{
}

Result<void>
CodexAccountLoginService::beginLogin(
    const CodexAccountProfile&
        profile) const
{
    const auto command =
        request(
            profile,
            {
                QStringLiteral("login"),
            },
            true);
    if (!command.hasValue()) {
        return Result<void>::failure(
            command.error());
    }
    return interactiveCommand_(
        command.value());
}

Result<void>
CodexAccountLoginService::beginLoginCurrentAccount()
    const
{
    const auto command =
        request(
            std::nullopt,
            {
                QStringLiteral("login"),
            },
            true);
    if (!command.hasValue()) {
        return Result<void>::failure(
            command.error());
    }
    return interactiveCommand_(
        command.value());
}

Result<
    CodexAccountAuthenticationStatus>
CodexAccountLoginService::readStatus(
    const CodexAccountProfile&
        profile) const
{
    return readStatus(
        std::optional<CodexAccountProfile>(
            profile));
}

Result<
    CodexAccountAuthenticationStatus>
CodexAccountLoginService::
readCurrentAccountStatus() const
{
    return readStatus(std::nullopt);
}

Result<
    CodexAccountAuthenticationStatus>
CodexAccountLoginService::readStatus(
    std::optional<CodexAccountProfile>
        profile) const
{
    const auto command =
        request(
            profile,
            {
                QStringLiteral("login"),
                QStringLiteral("status"),
            },
            false);
    if (!command.hasValue()) {
        return Result<
            CodexAccountAuthenticationStatus>::
            failure(command.error());
    }

    const auto result =
        statusCommand_(
            command.value());
    if (!result.hasValue()) {
        return Result<
            CodexAccountAuthenticationStatus>::
            failure(result.error());
    }
    const QString message =
        safeStatusMessage(
            result.value());
    if (result.value().exitCode == 0) {
        return Result<
            CodexAccountAuthenticationStatus>::
            success({
                CodexAccountAuthenticationState::
                    SignedIn,
                message,
            });
    }
    return Result<
        CodexAccountAuthenticationStatus>::
        success({
            CodexAccountAuthenticationState::
                SignedOut,
            message.isEmpty()
                ? QStringLiteral(
                      "This Codex account profile is not signed in.")
                : message,
        });
}

Result<CodexLoginProcessRequest>
CodexAccountLoginService::request(
    std::optional<CodexAccountProfile>
        profile,
    QStringList arguments,
    bool interactive) const
{
    const QString program =
        executable_.trimmed();
    if (program.isEmpty()) {
        return Result<
            CodexLoginProcessRequest>::
            failure(
                loginError(
                    QStringLiteral(
                        "codex.login_executable_unavailable"),
                    QStringLiteral(
                        "The official Codex executable is unavailable."),
                    executable_));
    }
    QProcessEnvironment environment =
        baseEnvironment_;
    if (profile.has_value()) {
        const auto preparedHome =
            accountRuntime_->prepareProfileHome(
                *profile);
        if (!preparedHome.hasValue()) {
            return Result<
                CodexLoginProcessRequest>::
                failure(
                    preparedHome.error());
        }
        const auto routedEnvironment =
            accountRuntime_->forProfile(
                baseEnvironment_,
                *profile);
        if (!routedEnvironment.hasValue()) {
            return Result<
                CodexLoginProcessRequest>::
                failure(
                    routedEnvironment.error());
        }
        environment = routedEnvironment.value();
    }
    return Result<
        CodexLoginProcessRequest>::
        success({
            program,
            std::move(arguments),
            std::move(environment),
            interactive,
            interactive,
        });
}

Result<void>
CodexAccountLoginService::
    startInteractive(
        const CodexLoginProcessRequest&
            request)
{
    QProcess process;
    process.setProgram(request.program);
    process.setArguments(
        request.arguments);
    process.setProcessEnvironment(
        request.environment);
    process.setProcessChannelMode(
        QProcess::ForwardedChannels);
    if (request.createVisibleConsole) {
        process.setCreateProcessArgumentsModifier(
            [](
                QProcess::CreateProcessArguments*
                    arguments) {
                arguments->flags &=
                    ~CREATE_NO_WINDOW;
                arguments->flags &=
                    ~DETACHED_PROCESS;
                arguments->flags |=
                    CREATE_NEW_CONSOLE;
                arguments->startupInfo
                    ->dwFlags |=
                    STARTF_USESHOWWINDOW;
                arguments->startupInfo
                    ->wShowWindow =
                    SW_SHOWNORMAL;
            });
    }
    qint64 processId = 0;
    if (!process.startDetached(
            &processId)
        || processId <= 0) {
        return Result<void>::failure(
            loginError(
                QStringLiteral(
                    "codex.login_launch_failed"),
                QStringLiteral(
                    "The official Codex sign-in process did not start."),
                request.program,
                process.errorString()));
    }
    return Result<void>::success();
}

Result<CodexLoginProcessResult>
CodexAccountLoginService::runStatus(
    const CodexLoginProcessRequest&
        request)
{
    QProcess process;
    process.setProgram(request.program);
    process.setArguments(
        request.arguments);
    process.setProcessEnvironment(
        request.environment);
    process.setProcessChannelMode(
        QProcess::SeparateChannels);
    process.setCreateProcessArgumentsModifier(
        [](
            QProcess::CreateProcessArguments*
                arguments) {
            arguments->flags |=
                CREATE_NO_WINDOW;
        });
    process.start();
    if (!process.waitForStarted(
            kProcessStartTimeoutMilliseconds)) {
        return Result<
            CodexLoginProcessResult>::
            failure(
                loginError(
                    QStringLiteral(
                        "codex.login_status_launch_failed"),
                    QStringLiteral(
                        "The official Codex sign-in status process did not start."),
                    request.program,
                    process.errorString()));
    }
    if (!process.waitForFinished(
            kProcessFinishTimeoutMilliseconds)) {
        process.kill();
        process.waitForFinished(
            kProcessStartTimeoutMilliseconds);
        return Result<
            CodexLoginProcessResult>::
            failure(
                loginError(
                    QStringLiteral(
                        "codex.login_status_timed_out"),
                    QStringLiteral(
                        "The official Codex sign-in status process timed out."),
                    request.program));
    }
    if (process.exitStatus()
        != QProcess::NormalExit) {
        return Result<
            CodexLoginProcessResult>::
            failure(
                loginError(
                    QStringLiteral(
                        "codex.login_status_failed"),
                    QStringLiteral(
                        "The official Codex sign-in status process ended unexpectedly."),
                    request.program));
    }
    return Result<
        CodexLoginProcessResult>::
        success({
            process.exitCode(),
            process.readAllStandardOutput(),
            process.readAllStandardError(),
        });
}

QString CodexAccountLoginService::
    safeStatusMessage(
        const CodexLoginProcessResult&
            result)
{
    QStringList parts;
    const QString output =
        QString::fromUtf8(
            result.standardOutput)
            .trimmed();
    const QString error =
        QString::fromUtf8(
            result.standardError)
            .trimmed();
    if (!output.isEmpty()) {
        parts.append(output);
    }
    if (!error.isEmpty()) {
        parts.append(error);
    }
    QString message =
        parts.join(
            QLatin1Char('\n'))
            .trimmed();
    if (message.isEmpty()) {
        return result.exitCode == 0
            ? QStringLiteral(
                  "Signed in with the official Codex CLI.")
            : QString{};
    }

    static const QRegularExpression
        credentialPattern(
            QStringLiteral(
                R"(\b(token|cookie|secret|authorization|bearer)\b)"),
            QRegularExpression::
                CaseInsensitiveOption);
    if (credentialPattern.match(message)
            .hasMatch()) {
        return result.exitCode == 0
            ? QStringLiteral(
                  "Signed in with the official Codex CLI.")
            : QStringLiteral(
                  "Codex sign-in status is unavailable.");
    }
    if (message.size() > 240) {
        message.truncate(240);
    }
    return message;
}

} // namespace companion
