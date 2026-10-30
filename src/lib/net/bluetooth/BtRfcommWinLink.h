/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/BtTypes.h"
#include "net/bluetooth/IBtLink.h"

#include <QString>

namespace deskflow::net::bt {

//! Bluetooth link over a classic (BR/EDR) RFCOMM serial port on Windows.
/*!
Why this backend exists: Qt has no peripheral (advertising) role on Windows, so
the BLE transport can only ever work with a non-Windows server.  Classic
Bluetooth has the opposite split - Windows can publish an SPP server, macOS can
only act as the client - which makes this the transport to use when the server
runs on Windows.

The server registers a serial port service, waits for the paired peer to open
that channel and then hands the socket to the relay.  Chunks stay opaque; see
\c BtCodec for framing and flow control.
*/
class BtRfcommWinLink : public IBtLink
{
  Q_OBJECT
public:
  explicit BtRfcommWinLink(QObject *parent = nullptr);
  ~BtRfcommWinLink() override;

  void startServer(const Config &config) override;
  void startClient(const Config &config) override;
  void stop() override;

  bool isConnected() const override;
  int maxChunkSize() const override;
  bool sendChunk(const QByteArray &chunk) override;
  QString describe() const override;

private:
  class Private;
  Private *d = nullptr;
};

} // namespace deskflow::net::bt
