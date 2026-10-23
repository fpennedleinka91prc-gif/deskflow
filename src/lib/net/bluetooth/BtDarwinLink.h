/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/IBtLink.h"

#include <memory>

namespace deskflow::net::bt {

class BtDarwinLinkPrivate;

//! Native CoreBluetooth (macOS) implementation of IBtLink.
/*!
Qt's macOS port has no bridge for \c QBluetoothPermission, so every Bluetooth
call made through QtBluetooth is refused by Qt itself before it ever reaches the
operating system. This backend therefore talks to CoreBluetooth directly and
relies on the usage descriptions in the application bundle for the privacy
grant.

The object lives in the Bluetooth worker thread of \c BtRelay, while
CoreBluetooth delivers its callbacks on the main queue; every callback is
re-posted to the worker thread before any signal is emitted or any state is
touched.
*/
class BtDarwinLink : public IBtLink
{
  Q_OBJECT

public:
  explicit BtDarwinLink(QObject *parent = nullptr);
  ~BtDarwinLink() override;

  void startServer(const Config &config) override;
  void startClient(const Config &config) override;
  void stop() override;

  bool isConnected() const override;
  int maxChunkSize() const override;
  bool sendChunk(const QByteArray &chunk) override;
  QString describe() const override;

private:
  friend class BtDarwinLinkPrivate;

  std::unique_ptr<BtDarwinLinkPrivate> d;
};

} // namespace deskflow::net::bt
