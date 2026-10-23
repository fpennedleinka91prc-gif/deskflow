/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtLinkFactory.h"

#include "base/Log.h"
#include "common/Settings.h"
#include "net/bluetooth/BtLoopbackLink.h"
#include "net/bluetooth/IBtLink.h"

#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
#include "net/bluetooth/BtQtLink.h"
#endif

namespace deskflow::net::bt {

namespace {

QString requestedBackend()
{
  const QByteArray fromEnv = qgetenv("DESKFLOW_BT_BACKEND");
  if (!fromEnv.isEmpty()) {
    return QString::fromLatin1(fromEnv).trimmed().toLower();
  }
  return Settings::value(Settings::Bluetooth::Backend).toString().trimmed().toLower();
}

} // namespace

bool BtLinkFactory::hasBluetoothBackend()
{
#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
  return true;
#else
  return false;
#endif
}

QString BtLinkFactory::backendName()
{
  const QString requested = requestedBackend();
  if (requested == QLatin1String("loopback")) {
    return QStringLiteral("loopback");
  }
#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
  return QStringLiteral("qt-bluetooth");
#else
  return requested.isEmpty() ? QStringLiteral("none") : requested + QStringLiteral(" (not compiled in)");
#endif
}

IBtLink *BtLinkFactory::create(QObject *parent)
{
  const QString requested = requestedBackend();

  if (requested == QLatin1String("loopback")) {
    return new BtLoopbackLink(parent);
  }

#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
  if (
      requested.isEmpty() || requested == QLatin1String("auto") || requested == QLatin1String("qt") ||
      requested == QLatin1String("ble") || requested == QLatin1String("qtbluetooth")
  ) {
    return new BtQtLink(parent);
  }
#else
  if (requested.isEmpty() || requested == QLatin1String("auto")) {
    // Without Qt Bluetooth the only usable stand-in is the loopback backend,
    // which is what the end to end tests drive.
    if (qEnvironmentVariableIsSet("DESKFLOW_BT_LOOPBACK_PORT")) {
      return new BtLoopbackLink(parent);
    }
    LOG_WARN("bluetooth: this build has no Bluetooth backend (Qt Bluetooth was not found)");
    return nullptr;
  }
#endif

  LOG_WARN("bluetooth: unknown backend \"%s\"", requested.toUtf8().constData());
  return nullptr;
}

} // namespace deskflow::net::bt
