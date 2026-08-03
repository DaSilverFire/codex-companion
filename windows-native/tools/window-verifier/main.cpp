#include "platform/windows/NativeWindowApi.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>

#include <cstdint>

namespace {

struct Arguments final {
    DWORD pid = 0;
    QString jsonPath;
};

QString hwndString(HWND hwnd)
{
    return QStringLiteral("0x%1")
        .arg(reinterpret_cast<quintptr>(hwnd), 16, 16, QLatin1Char('0'));
}

bool parseArguments(const QStringList& args, Arguments& parsed)
{
    if (args.size() != 5 || args.at(1) != QStringLiteral("--pid") ||
        args.at(3) != QStringLiteral("--json")) {
        return false;
    }

    bool ok = false;
    const qulonglong pid = args.at(2).toULongLong(&ok);
    if (!ok || pid == 0 || pid > std::numeric_limits<DWORD>::max()) {
        return false;
    }

    parsed.pid = static_cast<DWORD>(pid);
    parsed.jsonPath = args.at(4);
    return !parsed.jsonPath.isEmpty();
}

struct EnumContext final {
    DWORD pid = 0;
    QJsonArray windows;
    bool violation = false;
};

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM lParam)
{
    auto* context = reinterpret_cast<EnumContext*>(lParam);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != context->pid) {
        return TRUE;
    }

    const companion::NativeWindowInfo info = companion::nativeWindowInfo(hwnd);
    const bool toolWindow = (info.extendedStyle & WS_EX_TOOLWINDOW) != 0;
    const bool appWindow = (info.extendedStyle & WS_EX_APPWINDOW) != 0;
    const bool noActivate = (info.extendedStyle & WS_EX_NOACTIVATE) != 0;
    const companion::NativeWindowShellClassification classification =
        companion::nativeWindowShellClassification(hwnd);

    QJsonObject window;
    window.insert(QStringLiteral("hwnd"), hwndString(hwnd));
    window.insert(QStringLiteral("title"), info.title);
    window.insert(QStringLiteral("visible"), info.visible);
    window.insert(QStringLiteral("cloaked"), info.cloaked);
    window.insert(QStringLiteral("toolWindow"), toolWindow);
    window.insert(QStringLiteral("appWindow"), appWindow);
    window.insert(QStringLiteral("noActivate"), noActivate);
    window.insert(QStringLiteral("owner"), hwndString(info.owner));
    window.insert(
        QStringLiteral("taskbarCandidate"),
        classification.taskbarCandidate);
    window.insert(
        QStringLiteral("altTabRepresentative"),
        hwndString(
            classification.altTabRepresentative));
    window.insert(
        QStringLiteral("altTabCandidate"),
        classification.altTabCandidate);
    context->windows.append(window);

    if (classification.taskbarCandidate
        || classification.altTabCandidate) {
        context->violation = true;
    }

    return TRUE;
}

bool writeJson(const QString& path, const QJsonArray& windows)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(windows).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    Arguments arguments;
    if (!parseArguments(application.arguments(), arguments)) {
        QTextStream(stderr)
            << "Usage: companion-window-verifier --pid <pid> --json <path>\n";
        return 1;
    }

    EnumContext context;
    context.pid = arguments.pid;
    if (EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&context)) == FALSE) {
        return 1;
    }
    if (!writeJson(arguments.jsonPath, context.windows)) {
        return 1;
    }

    return context.violation ? 2 : 0;
}
