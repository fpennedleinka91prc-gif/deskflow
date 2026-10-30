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

//! Bluetooth link over a classic (BR/EDR) RFCOMM serial port.
/*!
This is the transport to use when the two computers are already paired with
each other in the operating system.  It needs no scanning and no service
advertisement: the peer is whatever device is paired, the connection is the
normal Bluetooth pairing, and the data travels over an RFCOMM channel.

As with every backend the chunks are opaque; framing and flow control stay in
\c BtCodec.  RFCOMM is already a reliable, ordered serial link, so the credit
window rarely has to hold anything back, but keeping it means the relay, the
codec and the tests work unchanged.

Roles are the usual surroundings of Deskflow: the server (the computer that
owns the keyboard and mouse) opens a listening RFCOMM channel, the client opens
the paired device's channel.  The channel is discovered through SDP, falling
back to a small list of common channel numbers because not every peer publishes
its serial port service.
*/
class BtRfcommLink : public IBtLink
{
  Q_OBJECT
public:
  explicit BtRfcommLink(QObject *parent = nullptr);
  ~BtRfcommLink() override;

  // ISocketFactory style API used by BtRelay.
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
