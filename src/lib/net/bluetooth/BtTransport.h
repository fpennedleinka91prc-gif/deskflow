/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/BtTypes.h"

#include <QString>
#include <QtGlobal>

#include <memory>

class QThread;

namespace deskflow::net::bt {

class BtRelay;

//! Owns the Bluetooth relay thread and exposes the loopback endpoint to the apps.
/*!
Life cycle:

\code
  BtTransport transport(Mode::Server, BtTransport::configFromSettings());
  if (!transport.start(listenPort, &error)) { ... }
  ...
  transport.stop();
\endcode

In server mode \c start() only begins advertising; incoming peers are accepted
one at a time and dial the Deskflow loopback listener.  In client mode
\c start() listens on an ephemeral loopback port, reports it through
\c localPort() and then keeps trying to reach the configured peripheral.
*/
class BtTransport
{
public:
  BtTransport(Mode mode, Config config);
  ~BtTransport();

  BtTransport(const BtTransport &) = delete;
  BtTransport &operator=(const BtTransport &) = delete;

  //! Start the relay.  \param serverPort Deskflow listen port, server mode only.
  bool start(quint16 serverPort, QString *error = nullptr, int timeoutMs = 10000);

  //! Loopback port Deskflow should connect to (client mode).
  quint16 localPort() const;

  void stop();
  bool isRunning() const;

  //! True when the transport was requested on the command line or in settings.
  static bool enabledBySettings();

  //! Build a Config from settings, falling back to the computer name.
  static Config configFromSettings();

private:
  struct Impl;

  Mode m_mode;
  Config m_config;
  Impl *m_impl = nullptr;
  bool m_running = false;
};

} // namespace deskflow::net::bt
