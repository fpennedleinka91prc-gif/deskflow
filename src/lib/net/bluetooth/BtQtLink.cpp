/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtQtLink.h"

#include "base/Log.h"
#include "net/bluetooth/BtCodec.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>
#include <QtGlobal>

#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0) && __has_include(<QBluetoothPermission>)
#include <QBluetoothPermission>
#define DESKFLOW_HAVE_BLUETOOTH_PERMISSION 1
#else
#define DESKFLOW_HAVE_BLUETOOTH_PERMISSION 0
#endif
#endif

namespace deskflow::net::bt {

#if defined(DESKFLOW_BLUETOOTH) && DESKFLOW_BLUETOOTH

namespace {

//! Chunk size reported before an MTU is known (and when the stack hides it).
constexpr int kDefaultChunkSize = 180;

//! ATT header: opcode + handle = the bytes GATT steals from every MTU.
constexpr int kAttHeaderSize = 3;

constexpr int kDefaultDiscoveryTimeoutMs = 15000;
constexpr int kDefaultRetryMs = 2000;

//! How long to wait for the CCCD write before assuming notifications work.
constexpr int kCccdWaitMs = 2000;

//! Delay before advertising again after a central went away.
constexpr int kAdvertisingRestartDelayMs = 250;

constexpr quint16 kAdvertisingIntervalMinMs = 100;
constexpr quint16 kAdvertisingIntervalMaxMs = 150;

//! A 128 bit service UUID leaves little room for the name in the adv packet,
//! so the name travels in the scan response and is kept short.
constexpr int kMaxAdvertisingNameChars = 20;

constexpr auto kPermissionDeniedText =
    "Bluetooth permission denied - allow Deskflow in System Settings > Privacy & Security > Bluetooth";

int clampChunkSize(int size)
{
  return qBound(BtCodec::kMinChunkSize, size, BtCodec::kMaxChunkSize);
}

QBluetoothUuid serviceUuid()
{
  return QBluetoothUuid(QString::fromLatin1(kServiceUuid));
}

QBluetoothUuid rxUuid()
{
  return QBluetoothUuid(QString::fromLatin1(kRxCharacteristicUuid));
}

QBluetoothUuid txUuid()
{
  return QBluetoothUuid(QString::fromLatin1(kTxCharacteristicUuid));
}

QBluetoothUuid cccdUuid()
{
  return QBluetoothUuid(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
}

//! Little endian CCCD value that enables notifications.
QByteArray cccdNotifyValue()
{
  QByteArray value(2, '\0');
  value[0] = 0x01;
  return value;
}

QString controllerErrorText(QLowEnergyController::Error error)
{
  switch (error) {
  case QLowEnergyController::NoError:
    return QStringLiteral("no error");
  case QLowEnergyController::UnknownError:
    return QStringLiteral("unknown Bluetooth error");
  case QLowEnergyController::UnknownRemoteDeviceError:
    return QStringLiteral("unknown remote device");
  case QLowEnergyController::NetworkError:
    return QStringLiteral("Bluetooth network error");
  case QLowEnergyController::InvalidBluetoothAdapterError:
    return QStringLiteral("no usable Bluetooth adapter");
  case QLowEnergyController::ConnectionError:
    return QStringLiteral("Bluetooth connection error");
  default:
    return QStringLiteral("Bluetooth error %1").arg(int(error));
  }
}

QString serviceErrorText(QLowEnergyService::ServiceError error)
{
  switch (error) {
  case QLowEnergyService::NoError:
    return QStringLiteral("no error");
  case QLowEnergyService::OperationError:
    return QStringLiteral("GATT operation error");
  case QLowEnergyService::CharacteristicReadError:
    return QStringLiteral("GATT characteristic read error");
  case QLowEnergyService::CharacteristicWriteError:
    return QStringLiteral("GATT characteristic write error");
  case QLowEnergyService::DescriptorReadError:
    return QStringLiteral("GATT descriptor read error");
  case QLowEnergyService::DescriptorWriteError:
    return QStringLiteral("GATT descriptor write error");
  case QLowEnergyService::UnknownError:
    return QStringLiteral("unknown GATT error");
  default:
    return QStringLiteral("GATT error %1").arg(int(error));
  }
}

} // namespace

BtQtLink::BtQtLink(QObject *parent) : IBtLink(parent)
{
  m_retryTimer = new QTimer(this);
  m_retryTimer->setSingleShot(true);
  connect(m_retryTimer, &QTimer::timeout, this, &BtQtLink::onRetryTimeout);

  m_connectTimer = new QTimer(this);
  m_connectTimer->setSingleShot(true);
  connect(m_connectTimer, &QTimer::timeout, this, &BtQtLink::onConnectTimeout);

  m_cccdTimer = new QTimer(this);
  m_cccdTimer->setSingleShot(true);
  connect(m_cccdTimer, &QTimer::timeout, this, &BtQtLink::onCccdTimeout);
}

BtQtLink::~BtQtLink()
{
  m_stopping = true;
  ++m_generation;

  if (m_agent != nullptr) {
    m_agent->disconnect(this);
    m_agent->stop();
  }
  if (m_controller != nullptr) {
    m_controller->disconnect(this);
    if (m_advertising) {
      m_controller->stopAdvertising();
    }
    m_controller->disconnectFromDevice();
  }
}

void BtQtLink::startServer(const Config &config)
{
  m_mode = Mode::Server;
  m_config = config;
  m_stopping = false;
  m_connected = false;
  m_peerLabel.clear();
  ++m_generation;

  LOG_DEBUG("bluetooth: starting BLE peripheral \"%s\"", qPrintable(m_config.localName));
  withBluetoothPermission([this] { beginPeripheral(); });
}

void BtQtLink::startClient(const Config &config)
{
  m_mode = Mode::Client;
  m_config = config;
  m_stopping = false;
  m_connected = false;
  m_peerLabel.clear();
  ++m_generation;

  LOG_DEBUG(
      "bluetooth: starting BLE central (peer \"%s\"/\"%s\")", qPrintable(m_config.peerAddress),
      qPrintable(m_config.peerName)
  );
  withBluetoothPermission([this] { beginCentral(); });
}

void BtQtLink::stop()
{
  m_stopping = true;
  ++m_generation;

  m_retryTimer->stop();
  m_connectTimer->stop();
  m_cccdTimer->stop();

  if (m_agent != nullptr) {
    m_agent->stop();
    m_agent->disconnect(this);
    m_agent->deleteLater();
    m_agent = nullptr;
  }

  teardownController();

  if (m_connected) {
    markDisconnected(QStringLiteral("stopped"));
  }
}

bool BtQtLink::isConnected() const
{
  return m_connected;
}

int BtQtLink::maxChunkSize() const
{
  if (m_config.chunkSize > 0) {
    return clampChunkSize(m_config.chunkSize);
  }
  // One GATT value carries a whole BtCodec chunk, i.e. BtCodec's own header plus
  // its payload, and ATT takes another kAttHeaderSize bytes out of the MTU.
  const int usable = m_mtu - kAttHeaderSize - BtCodec::kHeaderSize;
  if (usable > 0) {
    return clampChunkSize(usable);
  }
  return kDefaultChunkSize;
}

bool BtQtLink::sendChunk(const QByteArray &chunk)
{
  if (!m_connected || m_service == nullptr) {
    return false;
  }

  const QLowEnergyCharacteristic &target = (m_mode == Mode::Server) ? m_txCharacteristic : m_rxCharacteristic;
  if (!target.isValid()) {
    fail(QStringLiteral("BLE: the Deskflow link characteristic is not available"));
    return false;
  }

  const int limit = maxChunkSize() + BtCodec::kHeaderSize;
  if (chunk.size() > limit) {
    // BtCodec should never produce this; a warning beats silently truncated data.
    LOG_WARN("bluetooth: chunk of %d bytes exceeds the %d byte MTU budget", int(chunk.size()), limit);
  }

  // Peripheral role: this emits a notification.  Central role: a plain write.
  // Both are fire and forget (WriteWithoutResponse); errors surface through
  // QLowEnergyService::errorOccurred.
  m_service->writeCharacteristic(target, chunk, QLowEnergyService::WriteWithoutResponse);
  scheduleWritable();
  return true;
}

QString BtQtLink::describe() const
{
  if (m_mode == Mode::Server) {
    QString state = QStringLiteral("idle");
    if (m_connected) {
      state = QStringLiteral("connected");
    } else if (m_advertising) {
      state = QStringLiteral("advertising");
    }
    const QString name = m_config.localName.isEmpty() ? QStringLiteral("Deskflow") : m_config.localName;
    return QStringLiteral("BLE peripheral '%1' (%2)").arg(name, state);
  }

  QString peer = m_peerLabel;
  if (peer.isEmpty()) {
    peer = !m_config.peerAddress.isEmpty() ? m_config.peerAddress : m_config.peerName;
  }
  if (peer.isEmpty()) {
    peer = QStringLiteral("(any Deskflow peripheral)");
  }
  const QString state = m_connected ? QStringLiteral("connected") : QStringLiteral("scanning");
  return QStringLiteral("BLE central -> %1 (%2)").arg(peer, state);
}

void BtQtLink::withBluetoothPermission(const std::function<void()> &onGranted)
{
#if DESKFLOW_HAVE_BLUETOOTH_PERMISSION
  QCoreApplication *app = QCoreApplication::instance();
  if (app == nullptr) {
    // No application object (unit test driver): nothing can ask for a grant.
    onGranted();
    return;
  }

  QBluetoothPermission permission;
  // Access covers scanning and connecting, Advertise covers the peripheral role.
  permission.setCommunicationModes(QBluetoothPermission::Access | QBluetoothPermission::Advertise);

  const Qt::PermissionStatus status = app->checkPermission(permission);
  if (status == Qt::PermissionStatus::Granted) {
    onGranted();
    return;
  }
  if (status == Qt::PermissionStatus::Denied) {
    failPermission();
    return;
  }

  const quint64 generation = m_generation;
  app->requestPermission(permission, this, [this, generation, onGranted](const QPermission &result) {
    if (m_stopping || generation != m_generation) {
      return;
    }
    if (result.status() == Qt::PermissionStatus::Granted) {
      onGranted();
    } else {
      failPermission();
    }
  });
#else
  // Qt < 6.5: the OS either has no permission prompt or asks on first use.
  onGranted();
#endif
}

void BtQtLink::beginPeripheral()
{
  if (m_stopping) {
    return;
  }

  teardownController();

  m_controller = QLowEnergyController::createPeripheral(this);
  if (m_controller == nullptr) {
    fail(QStringLiteral("BLE: this platform has no peripheral (advertising) support"));
    return;
  }

  connect(m_controller, &QLowEnergyController::stateChanged, this, &BtQtLink::onControllerStateChanged);
  connect(m_controller, &QLowEnergyController::errorOccurred, this, &BtQtLink::onControllerError);
  connect(m_controller, &QLowEnergyController::mtuChanged, this, &BtQtLink::onMtuChanged);

  // A whole BtCodec chunk (header plus payload) travels as one GATT value, so the
  // attributes must accept the largest frame the codec can build; the negotiated
  // MTU, not this attribute length, is what really limits a single write.
  const int valueLength = BtCodec::kMaxChunkSize + BtCodec::kHeaderSize;

  QLowEnergyCharacteristicData rx;
  rx.setUuid(rxUuid());
  rx.setProperties(QLowEnergyCharacteristic::Write | QLowEnergyCharacteristic::WriteNoResponse);
  rx.setValueLength(1, valueLength);

  QLowEnergyCharacteristicData tx;
  tx.setUuid(txUuid());
  tx.setProperties(QLowEnergyCharacteristic::Notify);
  tx.setValueLength(1, valueLength);
  // Without the CCCD descriptor the central cannot subscribe to notifications.
  tx.addDescriptor(QLowEnergyDescriptorData(cccdUuid(), QByteArray(2, '\0')));

  QLowEnergyServiceData serviceData;
  serviceData.setType(QLowEnergyServiceData::ServiceTypePrimary);
  serviceData.setUuid(serviceUuid());
  serviceData.addCharacteristic(rx);
  serviceData.addCharacteristic(tx);

  m_service = m_controller->addService(serviceData, this);
  if (m_service == nullptr) {
    fail(QStringLiteral("BLE: could not register the Deskflow GATT service"));
    return;
  }

  connect(m_service, &QLowEnergyService::stateChanged, this, &BtQtLink::onServiceStateChanged);
  connect(m_service, &QLowEnergyService::errorOccurred, this, &BtQtLink::onServiceError);
  connect(m_service, &QLowEnergyService::characteristicChanged, this, &BtQtLink::onCharacteristicChanged);
  connect(m_service, &QLowEnergyService::characteristicWritten, this, &BtQtLink::onCharacteristicWritten);
  connect(m_service, &QLowEnergyService::descriptorWritten, this, &BtQtLink::onDescriptorWritten);

  m_rxCharacteristic = resolveCharacteristic(kRxCharacteristicUuid);
  m_txCharacteristic = resolveCharacteristic(kTxCharacteristicUuid);
  if (!m_rxCharacteristic.isValid() || !m_txCharacteristic.isValid()) {
    LOG_WARN("bluetooth: local characteristics are not resolvable yet");
  }

  startAdvertising();
}

void BtQtLink::startAdvertising()
{
  if (m_controller == nullptr || m_stopping || m_advertising) {
    return;
  }

  QLowEnergyAdvertisingParameters parameters;
  parameters.setMode(QLowEnergyAdvertisingParameters::AdvInd);
  parameters.setInterval(kAdvertisingIntervalMinMs, kAdvertisingIntervalMaxMs);

  // The service UUID is what the central filters on, so it belongs in the
  // advertisement; the local name goes into the scan response where there is
  // room for it next to a 128 bit UUID.
  QLowEnergyAdvertisingData advertising;
  advertising.setDiscoverability(QLowEnergyAdvertisingData::DiscoverabilityGeneral);
  advertising.setIncludePowerLevel(true);
  advertising.setServices({serviceUuid()});

  QString name = m_config.localName;
  if (name.isEmpty()) {
    name = QStringLiteral("Deskflow");
  }
  if (name.size() > kMaxAdvertisingNameChars) {
    name = name.left(kMaxAdvertisingNameChars);
  }

  QLowEnergyAdvertisingData scanResponse;
  scanResponse.setLocalName(name);

  m_controller->startAdvertising(parameters, advertising, scanResponse);
  m_advertising = true;
  LOG_DEBUG("bluetooth: advertising as \"%s\"", qPrintable(name));
}

void BtQtLink::beginCentral()
{
  if (m_stopping) {
    return;
  }

  teardownController();

  if (m_agent == nullptr) {
    m_agent = new QBluetoothDeviceDiscoveryAgent(this);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, &BtQtLink::onDeviceDiscovered);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished, this, &BtQtLink::onDiscoveryFinished);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::canceled, this, &BtQtLink::onDiscoveryFinished);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this, &BtQtLink::onDiscoveryError);
  }

  const int timeout = m_config.connectTimeoutMs > 0 ? m_config.connectTimeoutMs : kDefaultDiscoveryTimeoutMs;
  m_agent->setLowEnergyDiscoveryTimeout(timeout);

  if (m_agent->isActive()) {
    m_agent->stop();
  }

  LOG_DEBUG("bluetooth: scanning for a Deskflow peripheral (%d ms)", timeout);
  m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BtQtLink::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
  if (m_stopping || m_controller != nullptr) {
    return;
  }

  bool matches = info.serviceUuids().contains(serviceUuid());
  if (!matches && !m_config.peerAddress.isEmpty()) {
    matches = info.address().toString().compare(m_config.peerAddress, Qt::CaseInsensitive) == 0;
  }
  if (!matches && m_config.peerAddress.isEmpty() && !m_config.peerName.isEmpty()) {
    matches = info.name().compare(m_config.peerName, Qt::CaseInsensitive) == 0;
  }
  if (!matches) {
    return;
  }

  LOG_DEBUG("bluetooth: found peripheral \"%s\" (%s)", qPrintable(info.name()), qPrintable(info.address().toString()));

  if (m_agent != nullptr) {
    m_agent->stop();
  }
  connectToPeripheral(info);
}

void BtQtLink::connectToPeripheral(const QBluetoothDeviceInfo &info)
{
  m_peer = info;
  m_peerLabel = info.name().isEmpty() ? info.address().toString()
                                      : QStringLiteral("%1 (%2)").arg(info.name(), info.address().toString());

  m_controller = QLowEnergyController::createCentral(info, this);
  if (m_controller == nullptr) {
    fail(QStringLiteral("BLE: cannot create a central for %1").arg(m_peerLabel));
    return;
  }

  connect(m_controller, &QLowEnergyController::connected, this, [this] {
    if (m_stopping || m_controller == nullptr) {
      return;
    }
    LOG_DEBUG("bluetooth: connected to %s, discovering services", qPrintable(m_peerLabel));
    m_controller->discoverServices();
  });

  connect(m_controller, &QLowEnergyController::serviceDiscovered, this, [this](const QBluetoothUuid &uuid) {
    if (m_stopping || m_controller == nullptr || m_service != nullptr) {
      return;
    }
    if (uuid != serviceUuid()) {
      return;
    }

    m_service = m_controller->createServiceObject(uuid, this);
    if (m_service == nullptr) {
      fail(QStringLiteral("BLE: cannot open the Deskflow GATT service"));
      return;
    }

    connect(m_service, &QLowEnergyService::stateChanged, this, &BtQtLink::onServiceStateChanged);
    connect(m_service, &QLowEnergyService::errorOccurred, this, &BtQtLink::onServiceError);
    connect(m_service, &QLowEnergyService::characteristicChanged, this, &BtQtLink::onCharacteristicChanged);
    connect(m_service, &QLowEnergyService::descriptorWritten, this, &BtQtLink::onDescriptorWritten);

    LOG_DEBUG("bluetooth: discovering characteristics");
    m_service->discoverDetails();
  });

  connect(m_controller, &QLowEnergyController::discoveryFinished, this, [this] {
    if (m_stopping || m_controller == nullptr || m_service != nullptr) {
      return;
    }
    LOG_WARN("bluetooth: the peer does not expose the Deskflow service");
    markDisconnected(QStringLiteral("peer has no Deskflow service"));
    scheduleReconnect();
  });

  connect(m_controller, &QLowEnergyController::stateChanged, this, &BtQtLink::onControllerStateChanged);
  connect(m_controller, &QLowEnergyController::errorOccurred, this, &BtQtLink::onControllerError);
  connect(m_controller, &QLowEnergyController::mtuChanged, this, &BtQtLink::onMtuChanged);

  m_connectTimer->start(m_config.connectTimeoutMs > 0 ? m_config.connectTimeoutMs : kDefaultDiscoveryTimeoutMs);
  m_controller->connectToDevice();
}

void BtQtLink::enableTxNotifications()
{
  if (m_service == nullptr || !m_txCharacteristic.isValid()) {
    return;
  }

  const QLowEnergyDescriptor cccd = m_txCharacteristic.descriptor(cccdUuid());
  if (!cccd.isValid()) {
    // Some stacks hide the CCCD from the client API; notifications still work
    // once the peer accepts the subscription, so do not block the session.
    LOG_WARN("bluetooth: no CCCD on the Tx characteristic, continuing without an explicit subscribe");
    markConnected(QStringLiteral("tx characteristic ready"));
    return;
  }

  m_awaitingCccd = true;
  m_cccdTimer->start(kCccdWaitMs);
  m_service->writeDescriptor(cccd, cccdNotifyValue());
  LOG_DEBUG("bluetooth: subscribing to Tx notifications");
}

void BtQtLink::onControllerStateChanged(QLowEnergyController::ControllerState state)
{
  LOG_DEBUG("bluetooth: controller state %d (%s)", int(state), qPrintable(describe()));

  if (m_mode == Mode::Server) {
    switch (state) {
    case QLowEnergyController::ConnectedState:
      // A peripheral cannot see the central's CCCD write on every platform, so
      // "connected" means "the link is up and the local service is registered".
      // The local characteristics are only resolvable once the stack is up.
      if (!m_rxCharacteristic.isValid() || !m_txCharacteristic.isValid()) {
        m_rxCharacteristic = resolveCharacteristic(kRxCharacteristicUuid);
        m_txCharacteristic = resolveCharacteristic(kTxCharacteristicUuid);
      }
      if (!m_rxCharacteristic.isValid() || !m_txCharacteristic.isValid()) {
        fail(QStringLiteral("BLE: the local GATT characteristics could not be resolved"));
        return;
      }
      markConnected(QStringLiteral("central connected"));
      break;
    case QLowEnergyController::AdvertisingState:
      m_advertising = true;
      break;
    case QLowEnergyController::UnconnectedState:
      if (m_connected) {
        markDisconnected(QStringLiteral("central disconnected"));
        scheduleAdvertisingRestart();
      } else if (m_advertising) {
        // The stack dropped the advertisement without a central ever arriving.
        scheduleAdvertisingRestart();
      }
      break;
    default:
      break;
    }
    return;
  }

  if (state == QLowEnergyController::UnconnectedState && m_connected) {
    markDisconnected(QStringLiteral("peripheral disconnected"));
    scheduleReconnect();
  }
}

void BtQtLink::onControllerError(QLowEnergyController::Error error)
{
  const QString text = controllerErrorText(error);
  LOG_WARN("bluetooth: %s (%s)", qPrintable(text), qPrintable(describe()));

  if (error == QLowEnergyController::InvalidBluetoothAdapterError) {
    // No adapter, no point in retrying: this is a fatal setup problem.
    fail(QStringLiteral("BLE: %1").arg(text));
    return;
  }

  if (m_stopping) {
    return;
  }

  markDisconnected(text);
  if (m_mode == Mode::Client) {
    scheduleReconnect();
  } else {
    scheduleAdvertisingRestart();
  }
}

void BtQtLink::onMtuChanged(int mtu)
{
  m_mtu = mtu;
  LOG_DEBUG("bluetooth: negotiated MTU %d, chunk size %d (%s)", mtu, maxChunkSize(), qPrintable(describe()));
  scheduleWritable();
}

void BtQtLink::onServiceStateChanged(QLowEnergyService::ServiceState state)
{
  LOG_DEBUG("bluetooth: service state %d (%s)", int(state), qPrintable(describe()));

  if (m_mode == Mode::Server) {
    if (state == QLowEnergyService::LocalService) {
      m_rxCharacteristic = resolveCharacteristic(kRxCharacteristicUuid);
      m_txCharacteristic = resolveCharacteristic(kTxCharacteristicUuid);
    }
    return;
  }

  switch (state) {
  case QLowEnergyService::RemoteServiceDiscovered:
    m_rxCharacteristic = resolveCharacteristic(kRxCharacteristicUuid);
    m_txCharacteristic = resolveCharacteristic(kTxCharacteristicUuid);
    if (!m_rxCharacteristic.isValid() || !m_txCharacteristic.isValid()) {
      fail(QStringLiteral("BLE: the peer's Deskflow service is incomplete"));
      return;
    }
    enableTxNotifications();
    break;
  case QLowEnergyService::InvalidService:
    LOG_WARN("bluetooth: the peer's Deskflow service became invalid");
    markDisconnected(QStringLiteral("service invalid"));
    scheduleReconnect();
    break;
  default:
    break;
  }
}

void BtQtLink::onServiceError(QLowEnergyService::ServiceError error)
{
  if (error == QLowEnergyService::NoError) {
    return;
  }

  const QString text = serviceErrorText(error);
  LOG_WARN("bluetooth: %s (%s)", qPrintable(text), qPrintable(describe()));

  if (m_service == nullptr || m_service->state() == QLowEnergyService::InvalidService) {
    // The service object itself is unusable: rebuild everything.
    fail(QStringLiteral("BLE: %1").arg(text));
    return;
  }

  if (m_stopping) {
    return;
  }

  // A refused write or descriptor is a broken link rather than a broken setup,
  // so report it and let the reconnect / re-advertise path take over.
  Q_EMIT errorOccurred(QStringLiteral("BLE: %1").arg(text));
  markDisconnected(text);
  if (m_mode == Mode::Client) {
    scheduleReconnect();
  } else {
    scheduleAdvertisingRestart();
  }
}

void BtQtLink::onCharacteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &value)
{
  // Only the central receives notifications; the peripheral originates them.
  if (m_mode != Mode::Client) {
    return;
  }
  if (characteristic.uuid() != txUuid() || value.isEmpty()) {
    return;
  }
  Q_EMIT chunkReceived(value);
}

void BtQtLink::onCharacteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &value)
{
  // Only the peripheral sees writes from the peer (central -> peripheral).
  if (m_mode != Mode::Server) {
    return;
  }
  if (characteristic.uuid() != rxUuid() || value.isEmpty()) {
    return;
  }
  Q_EMIT chunkReceived(value);
}

void BtQtLink::onDescriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &value)
{
  Q_UNUSED(descriptor)
  // The only descriptor we expose is the Tx CCCD, so any descriptor write is
  // the central subscribing (0x0001) or unsubscribing.
  const bool enabled = !value.isEmpty() && (value.at(0) & 0x01) != 0;
  LOG_DEBUG("bluetooth: Tx notifications %s", enabled ? "enabled" : "disabled");

  if (m_mode == Mode::Server) {
    if (enabled && m_connected) {
      scheduleWritable();
    }
    return;
  }

  if (m_awaitingCccd && enabled) {
    m_awaitingCccd = false;
    m_cccdTimer->stop();
    markConnected(QStringLiteral("subscribed to Tx notifications"));
  }
}

void BtQtLink::onDiscoveryFinished()
{
  if (m_stopping || m_controller != nullptr) {
    return;
  }
  LOG_DEBUG("bluetooth: scan finished without a matching peripheral");
  scheduleReconnect();
}

void BtQtLink::onDiscoveryError(QBluetoothDeviceDiscoveryAgent::Error error)
{
  if (m_stopping) {
    return;
  }

  if (error == QBluetoothDeviceDiscoveryAgent::InvalidBluetoothAdapterError) {
    fail(QStringLiteral("BLE: no usable Bluetooth adapter"));
    return;
  }

  LOG_WARN("bluetooth: discovery error %d", int(error));
  scheduleReconnect();
}

void BtQtLink::onConnectTimeout()
{
  if (m_stopping) {
    return;
  }

  LOG_WARN("bluetooth: connection attempt timed out (%s)", qPrintable(describe()));
  markDisconnected(QStringLiteral("connect timeout"));
  if (m_mode == Mode::Client) {
    scheduleReconnect();
  } else {
    scheduleAdvertisingRestart();
  }
}

void BtQtLink::onCccdTimeout()
{
  if (m_stopping || !m_awaitingCccd) {
    return;
  }
  m_awaitingCccd = false;
  LOG_WARN("bluetooth: no CCCD confirmation, assuming Tx notifications are enabled");
  markConnected(QStringLiteral("cccd confirmation timeout"));
}

void BtQtLink::onRetryTimeout()
{
  if (m_stopping) {
    return;
  }

  if (m_mode == Mode::Server) {
    if (m_controller == nullptr) {
      beginPeripheral();
    } else {
      startAdvertising();
    }
    return;
  }

  beginCentral();
}

void BtQtLink::scheduleReconnect()
{
  if (m_stopping) {
    return;
  }

  teardownController();
  if (m_agent != nullptr) {
    m_agent->stop();
  }

  const int delay = m_config.retryIntervalMs > 0 ? m_config.retryIntervalMs : kDefaultRetryMs;
  LOG_DEBUG("bluetooth: retrying in %d ms", delay);
  m_retryTimer->start(delay);
}

void BtQtLink::scheduleAdvertisingRestart()
{
  if (m_stopping) {
    return;
  }
  m_advertising = false;
  m_retryTimer->start(kAdvertisingRestartDelayMs);
}

void BtQtLink::scheduleWritable()
{
  // A request made while writable() is being delivered comes from the relay
  // flushing its backlog, so re-scheduling it would spin the event loop.
  if (m_writableScheduled || m_emittingWritable) {
    return;
  }
  m_writableScheduled = true;
  // Queued: sendChunk() is called from the relay's flush loop, and emitting
  // writable() synchronously would re-enter that loop.
  QMetaObject::invokeMethod(
      this,
      [this] {
        m_writableScheduled = false;
        if (!m_connected) {
          return;
        }
        m_emittingWritable = true;
        Q_EMIT writable();
        m_emittingWritable = false;
      },
      Qt::QueuedConnection
  );
}

void BtQtLink::markConnected(const QString &why)
{
  if (m_connected) {
    return;
  }

  m_connected = true;
  m_awaitingCccd = false;
  if (m_mode == Mode::Server) {
    // The stack stops advertising as soon as a central connects.
    m_advertising = false;
  }
  m_connectTimer->stop();
  m_cccdTimer->stop();
  LOG_INFO("bluetooth: link up (%s): %s", qPrintable(why), qPrintable(describe()));
  Q_EMIT connected();
  scheduleWritable();
}

void BtQtLink::markDisconnected(const QString &reason)
{
  if (!m_connected) {
    return;
  }

  m_connected = false;
  m_awaitingCccd = false;
  LOG_INFO("bluetooth: link down: %s", qPrintable(reason));
  Q_EMIT disconnected(reason);
}

void BtQtLink::teardownController()
{
  if (m_service != nullptr) {
    QLowEnergyService *service = m_service;
    m_service = nullptr;
    service->disconnect(this);
    if (m_mode == Mode::Client) {
      // A central's service object belongs to the caller (createServiceObject).
      service->deleteLater();
    }
    // A peripheral's service object belongs to the controller (addService),
    // which releases it together with the controller below. It is also
    // parented to this link, so nothing is left dangling either way.
  }

  if (m_controller != nullptr) {
    QLowEnergyController *controller = m_controller;
    m_controller = nullptr;
    controller->disconnect(this);
    if (m_advertising) {
      controller->stopAdvertising();
    }
    if (controller->state() != QLowEnergyController::UnconnectedState) {
      controller->disconnectFromDevice();
    }
    controller->deleteLater();
  }

  m_rxCharacteristic = QLowEnergyCharacteristic();
  m_txCharacteristic = QLowEnergyCharacteristic();
  m_advertising = false;
  m_awaitingCccd = false;
  m_mtu = 0;
}

QLowEnergyCharacteristic BtQtLink::resolveCharacteristic(const QString &uuid) const
{
  if (m_service == nullptr) {
    return QLowEnergyCharacteristic();
  }

  const QBluetoothUuid target(uuid);
  const QLowEnergyCharacteristic direct = m_service->characteristic(target);
  if (direct.isValid()) {
    return direct;
  }

  const QList<QLowEnergyCharacteristic> all = m_service->characteristics();
  for (const QLowEnergyCharacteristic &candidate : all) {
    if (candidate.uuid() == target) {
      return candidate;
    }
  }
  return QLowEnergyCharacteristic();
}

void BtQtLink::fail(const QString &message)
{
  LOG_ERR("bluetooth: %s", qPrintable(message));
  Q_EMIT errorOccurred(message);
  stop();
}

void BtQtLink::failPermission()
{
  fail(QString::fromUtf8(kPermissionDeniedText));
}

#else // !DESKFLOW_BLUETOOTH

namespace {

constexpr auto kNotCompiledText = "Bluetooth support was not compiled in";

} // namespace

BtQtLink::BtQtLink(QObject *parent) : IBtLink(parent)
{
}

BtQtLink::~BtQtLink() = default;

void BtQtLink::startServer(const Config &config)
{
  Q_UNUSED(config)
  m_mode = Mode::Server;
  LOG_ERR("bluetooth: %s", kNotCompiledText);
  Q_EMIT errorOccurred(QString::fromUtf8(kNotCompiledText));
}

void BtQtLink::startClient(const Config &config)
{
  Q_UNUSED(config)
  m_mode = Mode::Client;
  LOG_ERR("bluetooth: %s", kNotCompiledText);
  Q_EMIT errorOccurred(QString::fromUtf8(kNotCompiledText));
}

void BtQtLink::stop()
{
}

bool BtQtLink::isConnected() const
{
  return false;
}

int BtQtLink::maxChunkSize() const
{
  return 180;
}

bool BtQtLink::sendChunk(const QByteArray &chunk)
{
  Q_UNUSED(chunk)
  return false;
}

QString BtQtLink::describe() const
{
  return QStringLiteral("BLE (Bluetooth support was not compiled in)");
}

#endif // DESKFLOW_BLUETOOTH

} // namespace deskflow::net::bt
