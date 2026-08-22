#include "backend.h"

#include <utility>

ChatBackend::ChatBackend(ConnectionSettings settings, QObject *parent)
    : QObject(parent),
      m_settings(std::move(settings)),
      m_id(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

ChatBackend::~ChatBackend() = default;

void ChatBackend::setConnectionSettings(const ConnectionSettings &settings)
{
    m_settings = settings;
}

void ChatBackend::changePassword(const QString &, const QString &)
{
    emit backendError(QStringLiteral("Password change"),
                      QStringLiteral("This protocol does not support changing the account password here."));
}

void ChatBackend::sendRaw(const QString &, const QString &, const QString &)
{
    emit backendError(QStringLiteral("Raw protocol command"),
                      QStringLiteral("Raw protocol commands are not implemented by this backend."));
}

void ChatBackend::changeNickname(const QString &)
{
    emit backendError(QStringLiteral("Nickname change"),
                      QStringLiteral("Nickname changes are not implemented by this backend."));
}

void ChatBackend::addBuddy(const QString &)
{
    emit backendError(QStringLiteral("Buddy list"),
                      QStringLiteral("This protocol does not expose an AIM-style buddy list."));
}

void ChatBackend::removeBuddy(const QString &)
{
    emit backendError(QStringLiteral("Buddy list"),
                      QStringLiteral("This protocol does not expose an AIM-style buddy list."));
}

void ChatBackend::setTerminalSize(int, int)
{
}

void ChatBackend::sendTerminalInput(const QByteArray &)
{
    emit backendError(QStringLiteral("Terminal input"),
                      QStringLiteral("This backend does not accept raw terminal keystrokes."));
}
