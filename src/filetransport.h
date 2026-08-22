#pragma once

#include "filetransfer.h"

#include <QByteArray>
#include <QString>

namespace WaffleFileTransport {

inline QString unsecuredPrefix()
{
    return QString(QChar(0x1e)) + QStringLiteral("WHFILE1|");
}

inline QString wrapUnsecured(const QString &filePayload)
{
    const QByteArray encoded = filePayload.toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return unsecuredPrefix() + QString::fromLatin1(encoded);
}

inline bool unwrapUnsecured(const QString &wireText, QString &filePayload)
{
    filePayload.clear();
    const QString prefix = unsecuredPrefix();
    if (!wireText.startsWith(prefix)) return false;
    const QByteArray decoded = QByteArray::fromBase64(
        wireText.mid(prefix.size()).toLatin1(), QByteArray::Base64UrlEncoding);
    const QString candidate = QString::fromUtf8(decoded);
    if (!CpxFileTransferManager::looksLikeMessage(candidate)) return false;
    filePayload = candidate;
    return true;
}

inline QString transferId(const QString &filePayload)
{
    CpxFileTransferManager::Message message;
    if (!CpxFileTransferManager::parseMessage(filePayload, message, nullptr)) return {};
    return message.id;
}

} // namespace WaffleFileTransport
