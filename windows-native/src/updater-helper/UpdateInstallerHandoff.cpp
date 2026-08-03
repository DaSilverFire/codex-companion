#include "updater-helper/UpdateInstallerHandoff.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace companion {
namespace {

CompanionError handoffError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

CompanionError handoffWin32Error(
    QString code,
    QString message,
    DWORD error,
    QStringView path = {})
{
    QVariantMap context{
        {
            QStringLiteral(
                "win32Error"),
            QVariant::fromValue<
                qulonglong>(error),
        },
    };
    if (!path.isEmpty()) {
        context.insert(
            QStringLiteral("path"),
            path.toString());
    }
    return handoffError(
        std::move(code),
        std::move(message),
        std::move(context));
}

QString normalizedPath(QStringView path)
{
    return QDir::cleanPath(
        QDir::fromNativeSeparators(
            path.toString()));
}

bool isWithinTree(
    QStringView root,
    QStringView candidate)
{
    const QString normalizedRoot =
        normalizedPath(root);
    const QString normalizedCandidate =
        normalizedPath(candidate);
    return normalizedCandidate.startsWith(
        normalizedRoot
            + QLatin1Char('/'),
        Qt::CaseInsensitive);
}

QString apiPath(QStringView path)
{
    QString native =
        QDir::toNativeSeparators(
            normalizedPath(path));
    if (native.startsWith(
            QStringLiteral("\\\\?\\"))) {
        return native;
    }
    if (native.startsWith(
            QStringLiteral("\\\\"))) {
        return QStringLiteral(
                   "\\\\?\\UNC\\")
            + native.sliced(2);
    }
    return QStringLiteral("\\\\?\\")
        + native;
}

Result<void> validateDirectoryChain(
    QStringView path)
{
    QString current =
        normalizedPath(path);
    while (!current.isEmpty()) {
        const std::wstring native =
            apiPath(current)
                .toStdWString();
        const DWORD attributes =
            GetFileAttributesW(
                native.c_str());
        if (attributes
            != INVALID_FILE_ATTRIBUTES) {
            if ((attributes
                 & FILE_ATTRIBUTE_REPARSE_POINT)
                || (attributes
                    & FILE_ATTRIBUTE_DEVICE)) {
                return Result<void>::
                    failure(
                        handoffError(
                            QStringLiteral(
                                "update.handoff_unsafe_path"),
                            QStringLiteral(
                                "The updater handoff path contains a filesystem link."),
                            {
                                {
                                    QStringLiteral(
                                        "path"),
                                    current,
                                },
                            }));
            }
        } else {
            const DWORD error =
                GetLastError();
            if (error
                    != ERROR_FILE_NOT_FOUND
                && error
                    != ERROR_PATH_NOT_FOUND) {
                return Result<void>::
                    failure(
                        handoffWin32Error(
                            QStringLiteral(
                                "update.handoff_path_inspection_failed"),
                            QStringLiteral(
                                "The updater handoff path could not be inspected."),
                            error,
                            current));
            }
        }

        const QString parent =
            normalizedPath(
                QFileInfo(current)
                    .absolutePath());
        if (parent.compare(
                current,
                Qt::CaseInsensitive)
            == 0) {
            break;
        }
        current = parent;
    }
    return Result<void>::success();
}

Result<void> createTransactionDirectory(
    QStringView path)
{
    const QString normalized =
        normalizedPath(path);
    const QString parent =
        QFileInfo(normalized)
            .absolutePath();
    const auto safe =
        validateDirectoryChain(parent);
    if (!safe.hasValue()) {
        return safe;
    }
    if (!QDir().mkpath(parent)) {
        return Result<void>::failure(
            handoffError(
                QStringLiteral(
                    "update.handoff_directory_create_failed"),
                QStringLiteral(
                    "The updater handoff directory could not be created.")));
    }
    const auto ready =
        validateDirectoryChain(parent);
    if (!ready.hasValue()) {
        return ready;
    }

    const std::wstring native =
        apiPath(normalized)
            .toStdWString();
    if (!CreateDirectoryW(
            native.c_str(),
            nullptr)) {
        return Result<void>::failure(
            handoffWin32Error(
                QStringLiteral(
                    "update.handoff_directory_create_failed"),
                QStringLiteral(
                    "The unique updater handoff directory could not be created."),
                GetLastError(),
                normalized));
    }
    return validateDirectoryChain(
        normalized);
}

Result<void> copyFile(
    QStringView source,
    QStringView destination)
{
    const std::wstring sourceNative =
        apiPath(source)
            .toStdWString();
    const DWORD attributes =
        GetFileAttributesW(
            sourceNative.c_str());
    if (attributes
            == INVALID_FILE_ATTRIBUTES
        || (attributes
            & (FILE_ATTRIBUTE_DIRECTORY
               | FILE_ATTRIBUTE_REPARSE_POINT
               | FILE_ATTRIBUTE_DEVICE))) {
        return Result<void>::failure(
            handoffWin32Error(
                QStringLiteral(
                    "update.handoff_runtime_missing"),
                QStringLiteral(
                    "A required updater runtime file is missing or unsafe."),
                attributes
                        == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_INVALID_DATA,
                source));
    }

    const std::wstring destinationNative =
        apiPath(destination)
            .toStdWString();
    if (!CopyFileW(
            sourceNative.c_str(),
            destinationNative.c_str(),
            TRUE)) {
        return Result<void>::failure(
            handoffWin32Error(
                QStringLiteral(
                    "update.handoff_copy_failed"),
                QStringLiteral(
                    "A required updater runtime file could not be copied."),
                GetLastError(),
                destination));
    }
    return Result<void>::success();
}

Result<UpdateAcknowledgementHandle>
prepareEvent(QStringView name)
{
    const std::wstring nativeName =
        name.toString().toStdWString();
    HANDLE event =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nativeName.c_str());
    if (event == nullptr) {
        return Result<
            UpdateAcknowledgementHandle>::
            failure(
                handoffWin32Error(
                    QStringLiteral(
                        "update.handoff_event_create_failed"),
                    QStringLiteral(
                        "An updater handoff event could not be created."),
                    GetLastError()));
    }
    if (!ResetEvent(event)) {
        const DWORD error =
            GetLastError();
        CloseHandle(event);
        return Result<
            UpdateAcknowledgementHandle>::
            failure(
                handoffWin32Error(
                    QStringLiteral(
                        "update.handoff_event_reset_failed"),
                    QStringLiteral(
                        "An updater handoff event could not be reset."),
                    error));
    }
    return Result<
        UpdateAcknowledgementHandle>::
        success(
            UpdateAcknowledgementHandle(
                event,
                [](void* rawEvent) {
                    if (rawEvent != nullptr) {
                        CloseHandle(
                            static_cast<
                                HANDLE>(
                                rawEvent));
                    }
                }));
}

void hideChildProcess(
    QProcess::CreateProcessArguments*
        arguments)
{
    arguments->flags |=
        CREATE_NO_WINDOW;
    arguments->startupInfo->dwFlags |=
        STARTF_USESHOWWINDOW;
    arguments->startupInfo->wShowWindow =
        SW_HIDE;
}

Result<quint32> launchHelper(
    QStringView executable,
    const QStringList& arguments,
    QStringView workingDirectory)
{
    QProcess process;
    process.setProgram(
        executable.toString());
    process.setArguments(arguments);
    process.setWorkingDirectory(
        workingDirectory.toString());
    process
        .setCreateProcessArgumentsModifier(
            hideChildProcess);
    qint64 processId = 0;
    if (!process.startDetached(
            &processId)
        || processId <= 0
        || processId
            > std::numeric_limits<
                quint32>::max()) {
        return Result<quint32>::
            failure(
                handoffError(
                    QStringLiteral(
                        "update.helper_launch_failed"),
                    QStringLiteral(
                        "The detached Windows updater could not be launched.")));
    }
    return Result<quint32>::success(
        static_cast<quint32>(
            processId));
}

Result<void> waitForEvent(
    const UpdateAcknowledgementHandle&
        event,
    std::chrono::milliseconds timeout)
{
    if (!event) {
        return Result<void>::failure(
            handoffError(
                QStringLiteral(
                    "update.handoff_event_unavailable"),
                QStringLiteral(
                    "The updater startup event is unavailable.")));
    }
    const DWORD milliseconds =
        static_cast<DWORD>(
            std::clamp<qint64>(
                timeout.count(),
                1,
                std::numeric_limits<
                    DWORD>::max() - 1));
    const DWORD waited =
        WaitForSingleObject(
            static_cast<HANDLE>(
                event.get()),
            milliseconds);
    if (waited == WAIT_OBJECT_0) {
        return Result<void>::success();
    }
    if (waited == WAIT_TIMEOUT) {
        return Result<void>::failure(
            handoffError(
                QStringLiteral(
                    "update.helper_ready_timeout"),
                QStringLiteral(
                    "The detached Windows updater did not confirm startup.")));
    }
    return Result<void>::failure(
        handoffWin32Error(
            QStringLiteral(
                "update.helper_ready_wait_failed"),
            QStringLiteral(
                "The updater startup handshake failed."),
            GetLastError()));
}

Result<void> terminateProcess(
    quint32 processId)
{
    HANDLE process =
        OpenProcess(
            PROCESS_TERMINATE
                | SYNCHRONIZE,
            FALSE,
            processId);
    if (process == nullptr) {
        const DWORD error =
            GetLastError();
        if (error
            == ERROR_INVALID_PARAMETER) {
            return Result<void>::
                success();
        }
        return Result<void>::failure(
            handoffWin32Error(
                QStringLiteral(
                    "update.helper_terminate_failed"),
                QStringLiteral(
                    "The failed detached updater could not be opened."),
                error));
    }
    const BOOL terminated =
        TerminateProcess(process, 1);
    const DWORD error =
        terminated
        ? ERROR_SUCCESS
        : GetLastError();
    if (terminated) {
        (void)WaitForSingleObject(
            process,
            5'000);
    }
    CloseHandle(process);
    return terminated
        ? Result<void>::success()
        : Result<void>::failure(
              handoffWin32Error(
                  QStringLiteral(
                      "update.helper_terminate_failed"),
                  QStringLiteral(
                      "The failed detached updater could not be stopped."),
                  error));
}

} // namespace

UpdateInstallerHandoff::
UpdateInstallerHandoff(
    UpdateInstallerHandoffOptions options,
    UpdateInstallerHandoffDependencies
        dependencies)
    : options_(std::move(options)),
      dependencies_(
          std::move(dependencies))
{
}

Result<UpdateInstallerHandoffReceipt>
UpdateInstallerHandoff::launch(
    const UpdateManifest& manifest,
    const VerifiedArtifact& artifact)
{
    if (manifest.version.isEmpty()
        || manifest.build <= 0
        || artifact.path.isEmpty()
        || !QDir::isAbsolutePath(
            normalizedPath(
                artifact.path))
        || artifact.size <= 0
        || artifact.sha256.size() != 32) {
        return Result<
            UpdateInstallerHandoffReceipt>::
            failure(
                handoffError(
                    QStringLiteral(
                        "update.handoff_artifact_invalid"),
                    QStringLiteral(
                        "The verified update facts are incomplete.")));
    }
    if (!options_.idFactory
        || !options_.currentProcessId
        || options_.applicationDirectory
               .isEmpty()
        || options_.temporaryRoot
               .isEmpty()) {
        return Result<
            UpdateInstallerHandoffReceipt>::
            failure(
                handoffError(
                    QStringLiteral(
                        "update.handoff_configuration_invalid"),
                    QStringLiteral(
                        "The Windows updater handoff is not configured.")));
    }

    const QUuid identifier =
        options_.idFactory();
    if (identifier.isNull()) {
        return Result<
            UpdateInstallerHandoffReceipt>::
            failure(
                handoffError(
                    QStringLiteral(
                        "update.handoff_identifier_invalid"),
                    QStringLiteral(
                        "The Windows updater handoff identifier could not be created.")));
    }
    const QString requestId =
        identifier.toString(
            QUuid::WithoutBraces);
    const QString transactionRoot =
        QDir(options_.temporaryRoot)
            .filePath(requestId);
    if (!isWithinTree(
            options_.temporaryRoot,
            transactionRoot)) {
        return Result<
            UpdateInstallerHandoffReceipt>::
            failure(
                handoffError(
                    QStringLiteral(
                        "update.handoff_path_invalid"),
                    QStringLiteral(
                        "The Windows updater handoff path is invalid.")));
    }

    if (!dependencies_
             .createTransactionDirectory) {
        return Result<
            UpdateInstallerHandoffReceipt>::
            failure(
                handoffError(
                    QStringLiteral(
                        "update.handoff_dependency_missing"),
                    QStringLiteral(
                        "The updater cannot create its handoff directory.")));
    }
    const auto directoryCreated =
        dependencies_
            .createTransactionDirectory(
                transactionRoot);
    if (!directoryCreated.hasValue()) {
        return Result<
            UpdateInstallerHandoffReceipt>::
            failure(
                directoryCreated.error());
    }

    const auto fail =
        [this, &transactionRoot](
            const CompanionError& error) {
            cleanup(transactionRoot);
            return Result<
                UpdateInstallerHandoffReceipt>::
                failure(error);
        };

    if (!dependencies_.copyFile) {
        return fail(
            handoffError(
                QStringLiteral(
                    "update.handoff_dependency_missing"),
                QStringLiteral(
                    "The updater cannot copy its runtime files.")));
    }
    for (const QString& name :
         options_.runtimeFileNames) {
        const QString source =
            QDir(
                options_
                    .applicationDirectory)
                .filePath(name);
        const QString destination =
            QDir(transactionRoot)
                .filePath(name);
        const auto copied =
            dependencies_.copyFile(
                source,
                destination);
        if (!copied.hasValue()) {
            return fail(
                copied.error());
        }
    }

    UpdateInstallRequest request;
    request.requestId = requestId;
    request.installerPath =
        normalizedPath(artifact.path);
    request.expectedSha256 =
        QString::fromLatin1(
            artifact.sha256.toHex());
    request.expectedSize =
        artifact.size;
    request.expectedVersion =
        manifest.version;
    request.expectedBuild =
        manifest.build;
    request.installRoot =
        UpdateInstallRequest::
            expectedInstallRoot();
    request.rollbackRoot =
        request.installRoot
        + QStringLiteral(".rollback.")
        + request.requestId;
    request.uninstallRegistryKey =
        UpdateInstallRequest::
            expectedUninstallRegistryKey();
    request.startMenuShortcut =
        UpdateInstallRequest::
            expectedStartMenuShortcut();
    request.acknowledgementEvent =
        UpdateInstallRequest::
            acknowledgementEventFor(
                request.requestId);
    request.parentProcessId =
        options_.currentProcessId();
    const auto requestValid =
        request.validate();
    if (!requestValid.hasValue()) {
        return fail(
            requestValid.error());
    }

    const QString requestPath =
        QDir(transactionRoot)
            .filePath(
                QStringLiteral(
                    "request.json"));
    if (!dependencies_.writeRequest) {
        return fail(
            handoffError(
                QStringLiteral(
                    "update.handoff_dependency_missing"),
                QStringLiteral(
                    "The updater cannot write its handoff request.")));
    }
    const auto written =
        dependencies_.writeRequest(
            request,
            requestPath);
    if (!written.hasValue()) {
        return fail(written.error());
    }

    if (!dependencies_.prepareEvent) {
        return fail(
            handoffError(
                QStringLiteral(
                    "update.handoff_dependency_missing"),
                QStringLiteral(
                    "The updater cannot prepare its startup events.")));
    }
    const auto acknowledgement =
        dependencies_.prepareEvent(
            request
                .acknowledgementEvent);
    if (!acknowledgement.hasValue()) {
        return fail(
            acknowledgement.error());
    }
    const auto helperReady =
        dependencies_.prepareEvent(
            UpdateInstallRequest::
                helperReadyEventFor(
                    request.requestId));
    if (!helperReady.hasValue()) {
        return fail(
            helperReady.error());
    }

    if (!dependencies_.launchHelper) {
        return fail(
            handoffError(
                QStringLiteral(
                    "update.handoff_dependency_missing"),
                QStringLiteral(
                    "The updater cannot launch its detached helper.")));
    }
    const QString helperPath =
        QDir(transactionRoot)
            .filePath(
                QStringLiteral(
                    "CodexCompanionUpdater.exe"));
    const auto launched =
        dependencies_.launchHelper(
            helperPath,
            {
                QStringLiteral("--request"),
                requestPath,
            },
            transactionRoot);
    if (!launched.hasValue()) {
        return fail(
            launched.error());
    }

    if (!dependencies_.waitForEvent) {
        if (dependencies_
                .terminateProcess) {
            (void)dependencies_
                .terminateProcess(
                    launched.value());
        }
        return fail(
            handoffError(
                QStringLiteral(
                    "update.handoff_dependency_missing"),
                QStringLiteral(
                    "The updater cannot confirm that its detached helper started.")));
    }
    const auto ready =
        dependencies_.waitForEvent(
            helperReady.value(),
            options_.helperReadyTimeout);
    if (!ready.hasValue()) {
        if (dependencies_
                .terminateProcess) {
            (void)dependencies_
                .terminateProcess(
                    launched.value());
        }
        return fail(ready.error());
    }

    return Result<
        UpdateInstallerHandoffReceipt>::
        success({
            transactionRoot,
            requestPath,
            helperPath,
            launched.value(),
        });
}

UpdateInstallerHandoff
UpdateInstallerHandoff::createProduction(
    QString applicationDirectory)
{
    UpdateInstallerHandoffOptions
        options;
    options.applicationDirectory =
        normalizedPath(
            applicationDirectory);
    options.temporaryRoot =
        QDir(
            QStandardPaths::
                writableLocation(
                    QStandardPaths::
                        TempLocation))
            .filePath(
                QStringLiteral(
                    "CodexCompanionUpdater"));
    options.runtimeFileNames =
        defaultRuntimeFileNames();
    options.idFactory = [] {
        return QUuid::createUuid();
    };
    options.currentProcessId = [] {
        return static_cast<quint32>(
            GetCurrentProcessId());
    };
    return {
        std::move(options),
        productionUpdateInstallerHandoffDependencies(),
    };
}

QStringList UpdateInstallerHandoff::
defaultRuntimeFileNames()
{
    return {
        QStringLiteral(
            "CodexCompanionUpdater.exe"),
        QStringLiteral("Qt6Core.dll"),
        QStringLiteral("MSVCP140.dll"),
        QStringLiteral("MSVCP140_1.dll"),
        QStringLiteral("VCRUNTIME140.dll"),
        QStringLiteral(
            "VCRUNTIME140_1.dll"),
    };
}

void UpdateInstallerHandoff::cleanup(
    QStringView transactionRoot) const
{
    if (dependencies_
            .cleanupTransactionDirectory
        && isWithinTree(
            options_.temporaryRoot,
            transactionRoot)) {
        dependencies_
            .cleanupTransactionDirectory(
                transactionRoot);
    }
}

UpdateInstallerHandoffDependencies
productionUpdateInstallerHandoffDependencies()
{
    UpdateInstallerHandoffDependencies
        dependencies;
    dependencies
        .createTransactionDirectory =
        createTransactionDirectory;
    dependencies.copyFile = copyFile;
    dependencies.writeRequest =
        [](const UpdateInstallRequest&
               request,
           QStringView path) {
            return request
                .writeAtomically(path);
        };
    dependencies.prepareEvent =
        prepareEvent;
    dependencies.launchHelper =
        launchHelper;
    dependencies.waitForEvent =
        waitForEvent;
    dependencies.terminateProcess =
        terminateProcess;
    dependencies
        .cleanupTransactionDirectory =
        [](QStringView path) {
            QDir(path.toString())
                .removeRecursively();
        };
    return dependencies;
}

} // namespace companion
