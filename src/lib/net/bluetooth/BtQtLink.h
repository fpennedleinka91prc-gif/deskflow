/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/BtTypes.h"
#include "net/bluetooth/IBtLink.h"

#include <QByteArray>
#include <QString>

#include <functional>

#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
// The module wide header keeps this file independent of Qt's per class header
// layout (QLowEnergyDescriptorData and friends moved around between versions).
#include <QtBluetooth/QtBluetooth>
#endif

class QTimer;

namespace deskflow::net::bt {

//! BLE (GATT) link backend on top of Qt Bluetooth.
/*!
The Deskflow transport uses one GATT service with two characteristics (see
\c BtTypes.h):

- \c Rx (Write / WriteWithoutResponse): central -> peripheral, i.e. bytes the
  peer sends us;
- \c Tx (Notify): peripheral -> central, i.e. bytes we send to the peer.

Roles:

- \c startServer() is the \e peripheral: it registers the service and
  advertises, accepts one central at a time, and goes back to advertising when
  the central leaves so a reconnect needs no restart.
- \c startClient() is the \e central: it scans for a device advertising
  \c kServiceUuid (or matching \c Config::peerAddress / \c Config::peerName),
  connects, discovers the service, subscribes to \c Tx and then reports
  \c connected().  Failed attempts are retried every
  \c Config::retryIntervalMs until \c stop().

Both directions move opaque chunks; framing and flow control stay in
\c BtCodec.

Known limitations: no bonding/pairing PIN handling, no encryption requirement,
one central at a time, and (because Qt exposes no local CCCD callback on every
platform) "connected" implies "writable" for the peripheral.

\warning The size of this class depends on \c DESKFLOW_BLUETOOTH, so every
translation unit that includes this header must see the same value.  The
bluetooth CMake target sets the macro \c PUBLIC; include the header through
that target rather than compiling against a bare \c Qt6::Core.
*/
class BtQtLink : public IBtLink
{
  Q_OBJECT
public:
  explicit BtQtLink(QObject *parent = nullptr);
  ~BtQtLink() override;

  BtQtLink(const BtQtLink &) = delete;
  BtQtLink &operator=(const BtQtLink &) = delete;

  // IBtLink overrides
  void startServer(const Config &config) override;
  void startClient(const Config &config) override;
  void stop() override;
  bool isConnected() const override;
  int maxChunkSize() const override;
  bool sendChunk(const QByteArray &chunk) override;
  QString describe() const override;

private:
#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
  //! Run \c onGranted now, or after the OS granted the Bluetooth permission.
  void withBluetoothPermission(const std::function<void()> &onGranted);

  void beginPeripheral();
  void beginCentral();
  void startAdvertising();
  void connectToPeripheral(const QBluetoothDeviceInfo &info);
  void enableTxNotifications();
  void teardownController();
  void scheduleReconnect();
  void scheduleAdvertisingRestart();
  void scheduleWritable();

  void markConnected(const QString &why);
  void markDisconnected(const QString &reason);
  void fail(const QString &message);
  void failPermission();

  QLowEnergyCharacteristic resolveCharacteristic(const QString &uuid) const;

  void onControllerStateChanged(QLowEnergyController::ControllerState state);
  void onControllerError(QLowEnergyController::Error error);
  void onMtuChanged(int mtu);
  void onServiceStateChanged(QLowEnergyService::ServiceState state);
  void onServiceError(QLowEnergyService::ServiceError error);
  void onCharacteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &value);
  void onCharacteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &value);
  void onDescriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &value);
  void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
  void onDiscoveryFinished();
  void onDiscoveryError(QBluetoothDeviceDiscoveryAgent::Error error);
  void onConnectTimeout();
  void onCccdTimeout();
  void onRetryTimeout();

  QLowEnergyController *m_controller = nullptr;
  QLowEnergyService *m_service = nullptr;
  QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
  QTimer *m_retryTimer = nullptr;
  QTimer *m_connectTimer = nullptr;
  QTimer *m_cccdTimer = nullptr;
  QLowEnergyCharacteristic m_rxCharacteristic;
  QLowEnergyCharacteristic m_txCharacteristic;
  QBluetoothDeviceInfo m_peer;
  QString m_peerLabel;
  quint64 m_generation = 0;
  int m_mtu = 0;
  bool m_connected = false;
  bool m_stopping = false;
  bool m_advertising = false;
  bool m_awaitingCccd = false;
  bool m_writableScheduled = false;
  bool m_emittingWritable = false;
#endif

  Mode m_mode = Mode::Server;
  Config m_config;
};

} // namespace deskflow::net::bt
