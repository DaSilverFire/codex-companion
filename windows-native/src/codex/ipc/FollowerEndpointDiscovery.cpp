#include "codex/ipc/FollowerEndpointDiscovery.h"

#include "codex/discovery/CodexDiscoverySource.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <Windows.h>
#include <TlHelp32.h>

#include <array>
#include <vector>

namespace companion {

namespace {

constexpr auto kVerifiedWindowsPipe =
    LR"(\\.\pipe\codex-ipc)";
constexpr auto kOfficialCodexPackageFamily =
    u"OpenAI.Codex_2p2nqsd0c76g0";

using NativeQueryInformationProcess = LONG(NTAPI*)(
    HANDLE,
    ULONG,
    PVOID,
    ULONG,
    PULONG);

struct NativeUnicodeString final {
    USHORT length = 0;
    USHORT maximumLength = 0;
    PWSTR buffer = nullptr;
};

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE handle = nullptr)
        : handle_(handle)
    {
    }

    ~NativeHandle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    bool valid() const noexcept
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

QString readSmallTextFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    constexpr qint64 maximumMetadataBytes = 1024 * 1024;
    if (file.size() < 0 || file.size() > maximumMetadataBytes) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool isCodexProcessName(const wchar_t* name)
{
    return _wcsicmp(name, L"ChatGPT.exe") == 0
        || _wcsicmp(name, L"codex.exe") == 0;
}

QString imagePathForProcess(HANDLE process)
{
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(
            process,
            0,
            buffer.data(),
            &size)
        || size == 0) {
        return {};
    }

    return QDir::fromNativeSeparators(
        QString::fromWCharArray(
            buffer.data(),
            static_cast<qsizetype>(size)));
}

QString commandLineForProcess(
    HANDLE process,
    NativeQueryInformationProcess query)
{
    if (query == nullptr) {
        return {};
    }

    ULONG required = 0;
    constexpr ULONG processCommandLineInformation = 60;
    query(
        process,
        processCommandLineInformation,
        nullptr,
        0,
        &required);
    if (required < sizeof(NativeUnicodeString)
        || required > 1024 * 1024) {
        return {};
    }

    QByteArray storage(
        static_cast<qsizetype>(required),
        Qt::Uninitialized);
    if (query(
            process,
            processCommandLineInformation,
            storage.data(),
            required,
            &required) < 0) {
        return {};
    }

    const auto* value =
        reinterpret_cast<const NativeUnicodeString*>(
            storage.constData());
    if (value->buffer == nullptr
        || value->length == 0
        || value->length % sizeof(wchar_t) != 0) {
        return {};
    }

    const auto* storageStart =
        reinterpret_cast<const uchar*>(storage.constData());
    const auto* storageEnd = storageStart + storage.size();
    const auto* textStart =
        reinterpret_cast<const uchar*>(value->buffer);
    const auto* textEnd = textStart + value->length;
    if (textStart < storageStart || textEnd > storageEnd) {
        return {};
    }

    return QString::fromWCharArray(
        value->buffer,
        value->length / sizeof(wchar_t));
}

QVector<FollowerProcessEvidence> runningCodexProcesses()
{
    QVector<FollowerProcessEvidence> result;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<NativeQueryInformationProcess>(
              GetProcAddress(
                  ntdll,
                  "NtQueryInformationProcess"));

    NativeHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0));
    if (!snapshot.valid()) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return result;
    }

    do {
        if (!isCodexProcessName(entry.szExeFile)) {
            continue;
        }
        NativeHandle process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            entry.th32ProcessID));
        if (!process.valid()) {
            continue;
        }
        const QString imagePath =
            imagePathForProcess(process.get());
        if (imagePath.isEmpty()) {
            continue;
        }
        const QString commandLine =
            commandLineForProcess(process.get(), query);
        result.push_back({
            imagePath,
            commandLine,
        });
    } while (Process32NextW(snapshot.get(), &entry));

    return result;
}

QString normalizedPath(QString path)
{
    path = QDir::fromNativeSeparators(path.trimmed());
    if (path.isEmpty()) {
        return {};
    }
    path = QDir::cleanPath(path);
    if (path == QStringLiteral(".")) {
        return {};
    }
    while (path.size() > 3
           && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

bool isSamePath(
    const QString& candidate,
    const QString& expected)
{
    const QString normalizedCandidate =
        normalizedPath(candidate);
    const QString normalizedExpected =
        normalizedPath(expected);
    return !normalizedCandidate.isEmpty()
        && !normalizedExpected.isEmpty()
        && normalizedCandidate.compare(
               normalizedExpected,
               Qt::CaseInsensitive)
            == 0;
}

bool isWithinPath(
    const QString& candidate,
    const QString& root)
{
    const QString normalizedCandidate =
        normalizedPath(candidate);
    QString normalizedRoot = normalizedPath(root);
    if (normalizedCandidate.isEmpty()
        || normalizedRoot.isEmpty()) {
        return false;
    }
    normalizedRoot += QLatin1Char('/');
    return normalizedCandidate.startsWith(
        normalizedRoot,
        Qt::CaseInsensitive);
}

bool isWindowsAppsCodexPath(
    const QString& candidate,
    const QString& programFiles)
{
    const QString windowsAppsRoot = QDir(programFiles).filePath(
        QStringLiteral("WindowsApps"));
    if (!isWithinPath(candidate, windowsAppsRoot)) {
        return false;
    }

    const QString normalizedCandidate =
        normalizedPath(candidate);
    QString normalizedRoot =
        normalizedPath(windowsAppsRoot);
    normalizedRoot += QLatin1Char('/');
    const QString relative =
        normalizedCandidate.sliced(normalizedRoot.size());
    const qsizetype separator =
        relative.indexOf(QLatin1Char('/'));
    const QString packageDirectory =
        separator < 0 ? relative : relative.first(separator);
    return packageDirectory.startsWith(
               QStringLiteral("OpenAI.Codex_"),
               Qt::CaseInsensitive)
        && packageDirectory.endsWith(
            QStringLiteral(
                "__2p2nqsd0c76g0"),
            Qt::CaseInsensitive);
}

FollowerExecutableTrust executableTrustForPath(
    const QString& imagePath,
    const CodexEnvironment& environment,
    const QVector<QString>&
        protectedProgramFilesDirectories,
    const QVector<QString>&
        installedCodexPackageExecutables)
{
    if (isSamePath(
            imagePath,
            environment.configuredExecutable)) {
        return FollowerExecutableTrust::
            ConfiguredOverride;
    }

    const QString filename =
        QFileInfo(normalizedPath(imagePath)).fileName();
    if (filename.compare(
            QStringLiteral("codex.exe"),
            Qt::CaseInsensitive)
            != 0
        && filename.compare(
               QStringLiteral("ChatGPT.exe"),
               Qt::CaseInsensitive)
            != 0) {
        return FollowerExecutableTrust::Untrusted;
    }

    if (isWithinPath(
            imagePath,
            environment.codexBinRoot)) {
        return FollowerExecutableTrust::LocalRuntime;
    }

    if (!environment.localAppData.isEmpty()
        && isWithinPath(
            imagePath,
            QDir(environment.localAppData).filePath(
                QStringLiteral("OpenAI/Codex")))) {
        return FollowerExecutableTrust::LocalRuntime;
    }

    for (const QString& packageExecutable :
         installedCodexPackageExecutables) {
        if (isSamePath(
                imagePath,
                packageExecutable)) {
            return FollowerExecutableTrust::
                WindowsPackage;
        }
    }

    for (const QString& programFiles :
         protectedProgramFilesDirectories) {
        if (isWindowsAppsCodexPath(
                imagePath,
                programFiles)) {
            return FollowerExecutableTrust::
                WindowsPackage;
        }
    }

    return FollowerExecutableTrust::Untrusted;
}

void appendMetadataFile(
    QVector<QString>& output,
    const QString& path)
{
    const QString text = readSmallTextFile(path);
    if (!text.isEmpty()) {
        output.push_back(text);
    }
}

QVector<QString> installedRuntimeMetadata(
    const CodexEnvironment& environment,
    const QVector<FollowerProcessEvidence>& processes,
    const QVector<QString>&
        protectedProgramFilesDirectories,
    const QVector<QString>&
        installedCodexPackageExecutables)
{
    QVector<QString> result;
    appendMetadataFile(result, environment.configToml);

    const QString localRuntimeRoot = QDir(
        environment.localAppData)
        .filePath(QStringLiteral("OpenAI/Codex"));
    appendMetadataFile(
        result,
        QDir(localRuntimeRoot).filePath(
            QStringLiteral("owl-shell-runtime.json")));

    const QDir runtimeRoot(localRuntimeRoot);
    const QFileInfoList patches = runtimeRoot.entryInfoList(
        {QStringLiteral("patched-*")},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Time);
    for (const QFileInfo& patch : patches) {
        appendMetadataFile(
            result,
            QDir(patch.absoluteFilePath()).filePath(
                QStringLiteral("owl-shell-runtime.json")));
        appendMetadataFile(
            result,
            QDir(patch.absoluteFilePath()).filePath(
                QStringLiteral("app/owl-shell-runtime.json")));
    }

    for (const FollowerProcessEvidence& process :
         processes) {
        if (FollowerEndpointDiscovery::executableTrust(
                process.imagePath,
                environment,
                protectedProgramFilesDirectories,
                installedCodexPackageExecutables)
            == FollowerExecutableTrust::Untrusted) {
            continue;
        }
        const QFileInfo executable(process.imagePath);
        if (!executable.isAbsolute()) {
            continue;
        }
        const QDir executableDirectory =
            executable.absoluteDir();
        appendMetadataFile(
            result,
            executableDirectory.filePath(
                QStringLiteral("resources/owl-electron-app.json")));
        appendMetadataFile(
            result,
            executableDirectory.filePath(
                QStringLiteral("resources/owl-app.ini")));
        appendMetadataFile(
            result,
            executableDirectory.filePath(
                QStringLiteral("owl-shell-runtime.json")));
    }

    return result;
}

class SystemFollowerEndpointEvidenceSource final
    : public IFollowerEndpointEvidenceSource {
public:
    QString environmentValue(QStringView name) const override
    {
        return qEnvironmentVariable(name.toString().toUtf8());
    }

    QVector<QString>
    protectedProgramFilesDirectories() const override
    {
        return
            systemProtectedProgramFilesDirectories();
    }

    QVector<QString>
    installedCodexPackageExecutables() const override
    {
        return systemCodexDiscoverySource()
            .installedCodexPackageExecutables();
    }

    QVector<FollowerProcessEvidence>
    runningCodexProcesses() const override
    {
        return companion::runningCodexProcesses();
    }

    QVector<QString> installedRuntimeMetadata(
        const CodexEnvironment& environment) const override
    {
        const QVector<FollowerProcessEvidence> processes =
            companion::runningCodexProcesses();
        return companion::installedRuntimeMetadata(
            environment,
            processes,
            protectedProgramFilesDirectories(),
            installedCodexPackageExecutables());
    }
};

QString normalizedPipe(QString candidate)
{
    candidate = candidate.trimmed();
    while (candidate.size() >= 2
           && ((candidate.front() == QLatin1Char('"')
                && candidate.back() == QLatin1Char('"'))
               || (candidate.front() == QLatin1Char('\'')
                   && candidate.back() == QLatin1Char('\'')))) {
        candidate = candidate.sliced(1, candidate.size() - 2)
                        .trimmed();
    }

    if (!candidate.contains(QLatin1Char('\\'))
        && !candidate.contains(QLatin1Char('/'))) {
        candidate =
            QStringLiteral(R"(\\.\pipe\)") + candidate;
    }

    const QString prefix = QStringLiteral(R"(\\.\pipe\)");
    if (!candidate.startsWith(prefix, Qt::CaseInsensitive)) {
        return {};
    }

    const QString name = candidate.sliced(prefix.size());
    if (name.isEmpty()
        || name.contains(QStringLiteral(".."))
        || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char(':'))) {
        return {};
    }
    for (const QChar character : name) {
        if (character.unicode() < 0x20) {
            return {};
        }
    }
    return prefix + name;
}

void appendCandidate(
    QVector<QString>& output,
    QSet<QString>& seen,
    const QString& raw)
{
    const QString candidate = normalizedPipe(raw);
    if (candidate.isEmpty()) {
        return;
    }
    const QString key = candidate.toCaseFolded();
    if (seen.contains(key)) {
        return;
    }
    seen.insert(key);
    output.push_back(candidate);
}

void appendCommandLineEvidence(
    QVector<QString>& output,
    QSet<QString>& seen,
    const QString& commandLine)
{
    static const QRegularExpression expression(
        QStringLiteral(
            R"regex((?:^|\s)--follower-(?:pipe|endpoint)(?:=|\s+)(?:"([^"]+)"|'([^']+)'|([^\s]+)))regex"),
        QRegularExpression::CaseInsensitiveOption);
    auto iterator = expression.globalMatch(commandLine);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match =
            iterator.next();
        const QString value = !match.captured(1).isEmpty()
            ? match.captured(1)
            : (!match.captured(2).isEmpty()
                   ? match.captured(2)
                   : match.captured(3));
        appendCandidate(output, seen, value);
    }
}

bool isEndpointKey(QString key)
{
    key.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    key = key.toLower();
    return key == QStringLiteral("followerpipe")
        || key == QStringLiteral("followerendpoint");
}

void appendJsonEvidence(
    QVector<QString>& output,
    QSet<QString>& seen,
    const QJsonValue& value)
{
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            appendJsonEvidence(output, seen, item);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    for (auto iterator = object.constBegin();
         iterator != object.constEnd();
         ++iterator) {
        if (isEndpointKey(iterator.key())
            && iterator.value().isString()) {
            appendCandidate(
                output,
                seen,
                iterator.value().toString());
        }
        appendJsonEvidence(output, seen, iterator.value());
    }
}

void appendMetadataEvidence(
    QVector<QString>& output,
    QSet<QString>& seen,
    const QString& metadata)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        metadata.toUtf8(),
        &parseError);
    if (parseError.error == QJsonParseError::NoError) {
        appendJsonEvidence(
            output,
            seen,
            document.isObject()
                ? QJsonValue(document.object())
                : QJsonValue(document.array()));
    }

    static const QRegularExpression assignment(
        QStringLiteral(
            R"regex(follower[_-]?(?:pipe|endpoint)\s*[:=]\s*(?:"((?:\\.|[^"])*)"|'([^']*)'|([^\s,}]+)))regex"),
        QRegularExpression::CaseInsensitiveOption);
    auto iterator = assignment.globalMatch(metadata);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match =
            iterator.next();
        QString value = !match.captured(1).isEmpty()
            ? match.captured(1)
            : (!match.captured(2).isEmpty()
                   ? match.captured(2)
                   : match.captured(3));
        value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        appendCandidate(output, seen, value);
    }
}

} // namespace

const IFollowerEndpointEvidenceSource&
systemFollowerEndpointEvidenceSource()
{
    static const SystemFollowerEndpointEvidenceSource source;
    return source;
}

FollowerEndpointDiscovery::FollowerEndpointDiscovery(
    const IFollowerEndpointEvidenceSource& source)
    : source_(source)
{
}

FollowerExecutableTrust
FollowerEndpointDiscovery::executableTrust(
    const QString& imagePath,
    const CodexEnvironment& environment,
    const QVector<QString>&
        protectedProgramFilesDirectories,
    const QVector<QString>&
        installedCodexPackageExecutables)
{
    return executableTrustForPath(
        imagePath,
        environment,
        protectedProgramFilesDirectories,
        installedCodexPackageExecutables);
}

QVector<QString> FollowerEndpointDiscovery::candidates(
    const CodexEnvironment& environment) const
{
    QVector<QString> result;
    QSet<QString> seen;

    static constexpr std::array<QStringView, 3>
        environmentNames{
            u"CODEX_COMPANION_FOLLOWER_PIPE",
            u"CODEX_FOLLOWER_PIPE",
            u"CODEX_IPC_PIPE",
        };
    for (const QStringView name : environmentNames) {
        appendCandidate(
            result,
            seen,
            source_.environmentValue(name));
    }

    const QVector<QString> protectedProgramFiles =
        source_.protectedProgramFilesDirectories();
    const QVector<QString> packageExecutables =
        source_.installedCodexPackageExecutables();
    for (const FollowerProcessEvidence& process :
         source_.runningCodexProcesses()) {
        if (executableTrust(
                process.imagePath,
                environment,
                protectedProgramFiles,
                packageExecutables)
            == FollowerExecutableTrust::Untrusted) {
            continue;
        }
        appendCommandLineEvidence(
            result,
            seen,
            process.commandLine);
    }

    for (const QString& metadata :
         source_.installedRuntimeMetadata(environment)) {
        appendMetadataEvidence(result, seen, metadata);
    }

    appendCandidate(
        result,
        seen,
        verifiedWindowsEndpoint());
    return result;
}

QString FollowerEndpointDiscovery::
verifiedWindowsEndpoint()
{
    return QString::fromWCharArray(
        kVerifiedWindowsPipe);
}

bool FollowerEndpointDiscovery::
isOfficialWindowsPackageProcess(
    const QString& imagePath,
    const QString& packageFamilyName)
{
    if (packageFamilyName.compare(
            kOfficialCodexPackageFamily,
            Qt::CaseInsensitive)
        != 0) {
        return false;
    }
    const QString filename =
        QFileInfo(imagePath).fileName();
    return filename.compare(
               QStringLiteral("codex.exe"),
               Qt::CaseInsensitive)
            == 0
        || filename.compare(
               QStringLiteral("ChatGPT.exe"),
               Qt::CaseInsensitive)
            == 0;
}

} // namespace companion
