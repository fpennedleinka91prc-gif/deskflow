/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QString>

namespace deskflow::gui {

//! CoreBluetooth device scan for macOS.
/*!
Qt's macOS port has no handler for \c QBluetoothPermission, so
\c QBluetoothDeviceDiscoveryAgent fails with \c MissingPermissionsError on every
scan, no matter what the system privacy settings say.  This class talks to
\c CBCentralManager directly, which makes the system ask for the Bluetooth
grant the normal way and returns the devices it finds.
*/
class MacBluetoothScanner : public QObject
{
  Q_OBJECT
public:
  //! Implementation state; defined in the Objective-C++ translation unit.
  class Private;

  explicit MacBluetoothScanner(QObject *parent = nullptr);
  ~MacBluetoothScanner() override;

  bool isScanning() const;

  //! Start a scan; \c timeoutMs <= 0 keeps it running until \c stop().
  void start(int timeoutMs = 10000);

  //! Stop a running scan; no signal is emitted for a scan we stopped.
  void stop();

Q_SIGNALS:
  //! \c identifier is the peripheral UUID, which is what the link connects to.
  void deviceFound(const QString &name, const QString &identifier);

  //! The scan ran to its end (timeout) on its own.
  void finished();

  //! Bluetooth is off, unauthorised, or the manager could not start.
  void failed(const QString &reason);

private:
  Private *d = nullptr;
};

} // namespace deskflow::gui
