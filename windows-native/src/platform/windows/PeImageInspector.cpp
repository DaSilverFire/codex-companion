#include "platform/windows/PeImageInspector.h"

#include <utility>

#include <QFile>
#include <QVariantMap>
#include <QtEndian>

#define NOMINMAX
#include <windows.h>

namespace companion {
namespace {

CompanionError peError(
    QString code,
    QString message,
    QStringView path)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {
                QStringLiteral("path"),
                path.toString(),
            },
        },
    };
}

} // namespace

Result<PeMachine>
PeImageInspector::machine(
    QStringView path) const
{
    QFile file(path.toString());
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<PeMachine>::failure(
            peError(
                QStringLiteral(
                    "update.artifact_open_failed"),
                QStringLiteral(
                    "The update installer could not be opened."),
                path));
    }

    constexpr qsizetype minimumDosHeaderSize =
        64;
    const QByteArray dosHeader =
        file.read(minimumDosHeaderSize);
    if (dosHeader.size()
            != minimumDosHeaderSize
        || dosHeader.at(0) != 'M'
        || dosHeader.at(1) != 'Z') {
        return Result<PeMachine>::failure(
            peError(
                QStringLiteral("update.invalid_pe"),
                QStringLiteral(
                    "The update installer has an invalid DOS header."),
                path));
    }

    constexpr qsizetype peOffsetField = 0x3c;
    const auto* peOffsetBytes =
        reinterpret_cast<const uchar*>(
            dosHeader.constData()
            + peOffsetField);
    const quint32 peOffset =
        qFromLittleEndian<quint32>(
            peOffsetBytes);
    constexpr qint64 signatureAndMachineBytes =
        6;
    if (peOffset
            < quint32(minimumDosHeaderSize)
        || qint64(peOffset)
            > file.size()
                - signatureAndMachineBytes
        || !file.seek(qint64(peOffset))) {
        return Result<PeMachine>::failure(
            peError(
                QStringLiteral("update.invalid_pe"),
                QStringLiteral(
                    "The update installer has an invalid PE offset."),
                path));
    }

    const QByteArray peHeader =
        file.read(signatureAndMachineBytes);
    if (peHeader.size()
            != signatureAndMachineBytes
        || peHeader.first(4)
            != QByteArrayLiteral("PE\0\0")) {
        return Result<PeMachine>::failure(
            peError(
                QStringLiteral("update.invalid_pe"),
                QStringLiteral(
                    "The update installer has an invalid PE signature."),
                path));
    }

    const auto* machineBytes =
        reinterpret_cast<const uchar*>(
            peHeader.constData() + 4);
    const quint16 machine =
        qFromLittleEndian<quint16>(
            machineBytes);
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386:
        return Result<PeMachine>::success(
            PeMachine::X86);
    case IMAGE_FILE_MACHINE_AMD64:
        return Result<PeMachine>::success(
            PeMachine::X64);
    case IMAGE_FILE_MACHINE_ARM64:
        return Result<PeMachine>::success(
            PeMachine::Arm64);
    default:
        return Result<PeMachine>::success(
            PeMachine::Unknown);
    }
}

} // namespace companion
