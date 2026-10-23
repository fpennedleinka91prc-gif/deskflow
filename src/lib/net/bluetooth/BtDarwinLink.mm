/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtDarwinLink.h"

#include "base/Log.h"
#include "net/bluetooth/BtCodec.h"

#include <QMetaObject>
#include <QTimer>

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <utility>

@class DeskflowBtCentralDelegate;
@class DeskflowBtPeripheralDelegate;

namespace deskflow::net::bt {

namespace {

//! ATT header plus the BtCodec chunk header: a chunk of N bytes occupies N + 3
//! bytes of the ATT payload, and the ATT payload is the MTU minus 3 bytes.
constexpr int kFrameOverhead = 3 + BtCodec::kHeaderSize;
//! Payload used before the link knows what the peer can carry.
constexpr int kDefaultChunkSize = 180;
//! Payload for the peripheral role. macOS negotiates a 185 byte ATT MTU for
//! connections it accepts, so 185 - 3 (ATT) - 3 (codec) = 179 and a little
//! slack keeps a full sized notification inside one PDU.
constexpr int kPeripheralChunkSize = 176;
constexpr int kDefaultScanTimeoutMs = 15000;
constexpr int kDefaultRetryMs = 2000;
//! A 128 bit service UUID leaves no room for a long name in the 31 byte
//! advertisement packet, so the name is kept short.
constexpr int kMaxAdvertisedNameLength = 20;

int clampChunkSize(int size)
{
  return std::clamp(size, BtCodec::kMinChunkSize, BtCodec::kMaxChunkSize);
}

QString nsString(NSString *value)
{
  return value != nil ? QString::fromNSString(value) : QString();
}

NSString *nsString(const QString &value)
{
  return value.toNSString();
}

CBUUID *uuidFromUtf8(const char *value)
{
  NSString *text = [NSString stringWithUTF8String:value];
  return text != nil ? [CBUUID UUIDWithString:text] : nil;
}

QString errorText(NSError *error)
{
  if (error == nil) {
    return QString();
  }
  const QString description = nsString(error.localizedDescription);
  return description.isEmpty() ? QStringLiteral("CoreBluetooth error %1").arg(error.code) : description;
}

} // namespace

//! State shared between the Qt wrapper and the CoreBluetooth delegates.
/*!
Every member is touched on the link thread only. The delegates run on the main
queue and re-post themselves through \c post() before calling into this class,
which keeps the Qt signals and the CoreBluetooth bookkeeping on one thread.
*/
class BtDarwinLinkPrivate
{
public:
  BtDarwinLink *q = nullptr;
  Mode mode = Mode::Server;
  Config config;

  bool running = false;
  bool connected = false;
  bool advertising = false;
  bool scanning = false;
  bool connecting = false;
  int chunkSize = kDefaultChunkSize;
  QString peerLabel;

  QTimer *retryTimer = nullptr;
  QTimer *attemptTimer = nullptr;

  CBCentralManager *central = nil;
  DeskflowBtCentralDelegate *centralDelegate = nil;
  CBPeripheral *peripheral = nil;
  CBCharacteristic *rxCharacteristic = nil;
  CBCharacteristic *txCharacteristic = nil;

  CBPeripheralManager *peripheralManager = nil;
  DeskflowBtPeripheralDelegate *peripheralDelegate = nil;
  CBMutableService *service = nil;
  CBMutableCharacteristic *rxCharacteristicData = nil;
  CBMutableCharacteristic *txCharacteristicData = nil;

  //! Hands a callback back to the thread that owns the link.
  template <typename F> void post(F &&fn)
  {
    QMetaObject::invokeMethod(q, std::forward<F>(fn), Qt::QueuedConnection);
  }

  void beginServer();
  void beginClient();
  void teardown();
  void startAdvertising();
  void addService();
  void beginScanning();
  void scheduleRetry();
  void markConnected();
  void markDisconnected(const QString &reason);
  void handleAttemptFailure(const QString &reason);
  void fail(const QString &message);
  bool sendChunk(const QByteArray &chunk);
  int computeChunkSize() const;
  QString stateLabel() const;
  bool matchesPeer(CBPeripheral *candidate, NSDictionary *advertisementData) const;

  void handlePeripheralState(CBManagerState state);
  void handleServiceAdded(NSError *error);
  void handleAdvertisingStarted(NSError *error);
  void handleSubscribed(CBCharacteristic *characteristic);
  void handleUnsubscribed(CBCharacteristic *characteristic);
  void handleWriteRequests(NSArray<CBATTRequest *> *requests);
  void handleReadRequest(CBATTRequest *request);
  void handleReadyToUpdate();

  void handleCentralState(CBManagerState state);
  void handleDiscovered(CBPeripheral *candidate, NSDictionary *advertisementData);
  void handleConnectedPeripheral(CBPeripheral *peer);
  void handleConnectFailed(NSError *error);
  void handleDisconnectedPeripheral(NSError *error);
  void handleServices(CBPeripheral *peer, NSError *error);
  void handleCharacteristics(CBPeripheral *peer, CBService *service, NSError *error);
  void handleNotifyState(CBCharacteristic *characteristic, NSError *error);
  void handleValue(CBCharacteristic *characteristic, NSError *error);
  void handleWriteResult(CBCharacteristic *characteristic, NSError *error);
  void handleReadyToWrite();
  void handleAttemptTimeout();
  void handleRetryTimeout();
};

} // namespace deskflow::net::bt

//! Receives the peripheral (server) callbacks and re-posts them.
@interface DeskflowBtPeripheralDelegate : NSObject <CBPeripheralManagerDelegate>
@property(nonatomic, assign) deskflow::net::bt::BtDarwinLinkPrivate *owner;
@end

//! Receives the central (client) callbacks and re-posts them.
@interface DeskflowBtCentralDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, assign) deskflow::net::bt::BtDarwinLinkPrivate *owner;
@end

@implementation DeskflowBtPeripheralDelegate

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)peripheral
{
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  const CBManagerState state = peripheral.state;
  owner->post([owner, state] { owner->handlePeripheralState(state); });
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral didAddService:(CBService *)service error:(NSError *)error
{
  (void)peripheral;
  (void)service;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, error] { owner->handleServiceAdded(error); });
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)peripheral error:(NSError *)error
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, error] { owner->handleAdvertisingStarted(error); });
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
                   central:(CBCentral *)central
    didSubscribeToCharacteristic:(CBCharacteristic *)characteristic
{
  (void)peripheral;
  (void)central;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, characteristic] { owner->handleSubscribed(characteristic); });
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
                   central:(CBCentral *)central
  didUnsubscribeFromCharacteristic:(CBCharacteristic *)characteristic
{
  (void)peripheral;
  (void)central;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, characteristic] { owner->handleUnsubscribed(characteristic); });
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, requests] { owner->handleWriteRequests(requests); });
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral didReceiveReadRequest:(CBATTRequest *)request
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, request] { owner->handleReadRequest(request); });
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager *)peripheral
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner] { owner->handleReadyToUpdate(); });
}

@end

@implementation DeskflowBtCentralDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  const CBManagerState state = central.state;
  owner->post([owner, state] { owner->handleCentralState(state); });
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
        advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                     RSSI:(NSNumber *)RSSI
{
  (void)central;
  (void)RSSI;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, peripheral, advertisementData] {
    owner->handleDiscovered(peripheral, advertisementData);
  });
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
  (void)central;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, peripheral] { owner->handleConnectedPeripheral(peripheral); });
}

- (void)centralManager:(CBCentralManager *)central
    didFailToConnectPeripheral:(CBPeripheral *)peripheral
                         error:(NSError *)error
{
  (void)central;
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, error] { owner->handleConnectFailed(error); });
}

- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)peripheral
                      error:(NSError *)error
{
  (void)central;
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, error] { owner->handleDisconnectedPeripheral(error); });
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, peripheral, error] { owner->handleServices(peripheral, error); });
}

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service
                                   error:(NSError *)error
{
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, peripheral, service, error] {
    owner->handleCharacteristics(peripheral, service, error);
  });
}

- (void)peripheral:(CBPeripheral *)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
                                          error:(NSError *)error
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, characteristic, error] { owner->handleNotifyState(characteristic, error); });
}

- (void)peripheral:(CBPeripheral *)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
                              error:(NSError *)error
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, characteristic, error] { owner->handleValue(characteristic, error); });
}

- (void)peripheral:(CBPeripheral *)peripheral
    didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
                             error:(NSError *)error
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner, characteristic, error] { owner->handleWriteResult(characteristic, error); });
}

- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral *)peripheral
{
  (void)peripheral;
  auto *owner = self.owner;
  if (owner == nullptr) {
    return;
  }
  owner->post([owner] { owner->handleReadyToWrite(); });
}

@end

namespace deskflow::net::bt {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void BtDarwinLinkPrivate::beginServer()
{
  if (peripheralDelegate == nil) {
    peripheralDelegate = [[DeskflowBtPeripheralDelegate alloc] init];
    peripheralDelegate.owner = this;
  }
  if (peripheralManager == nil) {
    // Creating the manager is what makes macOS evaluate the privacy grant.
    peripheralManager =
        [[CBPeripheralManager alloc] initWithDelegate:peripheralDelegate queue:dispatch_get_main_queue()];
  }
  if (peripheralManager.state == CBManagerStatePoweredOn) {
    if (service == nil) {
      addService();
    } else if (!advertising && !connected) {
      startAdvertising();
    }
  }
}

void BtDarwinLinkPrivate::beginClient()
{
  if (centralDelegate == nil) {
    centralDelegate = [[DeskflowBtCentralDelegate alloc] init];
    centralDelegate.owner = this;
  }
  if (central == nil) {
    central = [[CBCentralManager alloc] initWithDelegate:centralDelegate queue:dispatch_get_main_queue()];
  }
  if (central.state == CBManagerStatePoweredOn) {
    beginScanning();
  }
}

void BtDarwinLinkPrivate::teardown()
{
  if (retryTimer != nullptr) {
    retryTimer->stop();
  }
  if (attemptTimer != nullptr) {
    attemptTimer->stop();
  }

  if (central != nil) {
    [central stopScan];
    if (peripheral != nil) {
      peripheral.delegate = nil;
      [central cancelPeripheralConnection:peripheral];
    }
    central.delegate = nil;
    central = nil;
  }
  if (centralDelegate != nil) {
    centralDelegate.owner = nullptr;
    centralDelegate = nil;
  }

  if (peripheralManager != nil) {
    [peripheralManager stopAdvertising];
    [peripheralManager removeAllServices];
    peripheralManager.delegate = nil;
    peripheralManager = nil;
  }
  if (peripheralDelegate != nil) {
    peripheralDelegate.owner = nullptr;
    peripheralDelegate = nil;
  }

  peripheral = nil;
  rxCharacteristic = nil;
  txCharacteristic = nil;
  service = nil;
  rxCharacteristicData = nil;
  txCharacteristicData = nil;
  scanning = false;
  connecting = false;
  advertising = false;
  connected = false;
  chunkSize = kDefaultChunkSize;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void BtDarwinLinkPrivate::markConnected()
{
  if (!running) {
    return;
  }
  if (attemptTimer != nullptr) {
    attemptTimer->stop();
  }
  if (retryTimer != nullptr) {
    retryTimer->stop();
  }
  const bool fresh = !connected;
  connected = true;
  chunkSize = computeChunkSize();
  if (fresh) {
    LOG_INFO(
        "bluetooth: %s connected (chunk %d)", qPrintable(q->describe()), chunkSize
    );
    Q_EMIT q->connected();
  }
  Q_EMIT q->writable();
}

void BtDarwinLinkPrivate::markDisconnected(const QString &reason)
{
  if (!running) {
    return;
  }
  const bool wasConnected = connected;
  connected = false;
  chunkSize = kDefaultChunkSize;
  if (wasConnected) {
    LOG_INFO("bluetooth: link down: %s", qPrintable(reason));
  } else {
    LOG_DEBUG("bluetooth: link attempt failed: %s", qPrintable(reason));
  }
  Q_EMIT q->disconnected(reason);
}

void BtDarwinLinkPrivate::handleAttemptFailure(const QString &reason)
{
  if (!running) {
    return;
  }
  connecting = false;
  if (scanning) {
    scanning = false;
    if (central != nil) {
      [central stopScan];
    }
  }
  if (peripheral != nil) {
    peripheral.delegate = nil;
    if (central != nil) {
      [central cancelPeripheralConnection:peripheral];
    }
    peripheral = nil;
  }
  rxCharacteristic = nil;
  txCharacteristic = nil;
  markDisconnected(reason);
  scheduleRetry();
}

void BtDarwinLinkPrivate::fail(const QString &message)
{
  LOG_ERR("bluetooth: %s", qPrintable(message));
  running = false;
  if (retryTimer != nullptr) {
    retryTimer->stop();
  }
  if (attemptTimer != nullptr) {
    attemptTimer->stop();
  }
  Q_EMIT q->errorOccurred(message);
}

void BtDarwinLinkPrivate::scheduleRetry()
{
  if (!running || connected || retryTimer == nullptr) {
    return;
  }
  const int delay = config.retryIntervalMs > 0 ? config.retryIntervalMs : kDefaultRetryMs;
  LOG_DEBUG("bluetooth: retrying the link in %d ms", delay);
  retryTimer->start(delay);
}

int BtDarwinLinkPrivate::computeChunkSize() const
{
  if (config.chunkSize > 0) {
    return clampChunkSize(config.chunkSize);
  }
  if (mode == Mode::Client) {
    if (connected && peripheral != nil) {
      const NSUInteger maximum = [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
      const int usable = static_cast<int>(maximum) - kFrameOverhead;
      if (usable > 0) {
        return clampChunkSize(usable);
      }
    }
    return kDefaultChunkSize;
  }
  return connected ? kPeripheralChunkSize : kDefaultChunkSize;
}

QString BtDarwinLinkPrivate::stateLabel() const
{
  if (connected) {
    return QStringLiteral("connected");
  }
  if (advertising) {
    return QStringLiteral("advertising");
  }
  if (scanning) {
    return QStringLiteral("scanning");
  }
  if (connecting) {
    return QStringLiteral("connecting");
  }
  return QStringLiteral("idle");
}

bool BtDarwinLinkPrivate::sendChunk(const QByteArray &chunk)
{
  if (!running || !connected || chunk.isEmpty()) {
    return false;
  }

  @autoreleasepool {
    NSData *data = chunk.toNSData();
    if (data == nil) {
      return false;
    }

    if (mode == Mode::Server) {
      if (peripheralManager == nil || txCharacteristicData == nil) {
        return false;
      }
      // NO means the transmit queue is full; the relay keeps the chunk and
      // waits for peripheralManagerIsReadyToUpdateSubscribers.
      return [peripheralManager updateValue:data forCharacteristic:txCharacteristicData onSubscribedCentrals:nil] ==
             YES;
    }

    if (peripheral == nil || rxCharacteristic == nil) {
      return false;
    }

    const NSUInteger maximum = [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
    const CBCharacteristicWriteType type = maximum >= static_cast<NSUInteger>(chunk.size())
                                               ? CBCharacteristicWriteWithoutResponse
                                               : CBCharacteristicWriteWithResponse;
    if (type == CBCharacteristicWriteWithoutResponse && !peripheral.canSendWriteWithoutResponse) {
      return false;
    }
    [peripheral writeValue:data forCharacteristic:rxCharacteristic type:type];
    return true;
  }
}

// ---------------------------------------------------------------------------
// Peripheral (server) role
// ---------------------------------------------------------------------------

void BtDarwinLinkPrivate::addService()
{
  if (peripheralManager == nil || service != nil) {
    return;
  }

  rxCharacteristicData = [[CBMutableCharacteristic alloc]
      initWithType:uuidFromUtf8(kRxCharacteristicUuid)
        properties:CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse
             value:nil
       permissions:CBAttributePermissionsWriteable];
  txCharacteristicData = [[CBMutableCharacteristic alloc]
      initWithType:uuidFromUtf8(kTxCharacteristicUuid)
        properties:CBCharacteristicPropertyNotify
             value:nil
       permissions:CBAttributePermissionsReadable];
  service = [[CBMutableService alloc] initWithType:uuidFromUtf8(kServiceUuid) primary:YES];
  service.characteristics = @[ rxCharacteristicData, txCharacteristicData ];
  [peripheralManager addService:service];
  LOG_DEBUG("bluetooth: publishing the Deskflow GATT service");
}

void BtDarwinLinkPrivate::startAdvertising()
{
  if (peripheralManager == nil || advertising) {
    return;
  }

  NSMutableDictionary<NSString *, id> *data = [NSMutableDictionary dictionary];
  CBUUID *serviceUuid = uuidFromUtf8(kServiceUuid);
  if (serviceUuid != nil) {
    data[CBAdvertisementDataServiceUUIDsKey] = @[ serviceUuid ];
  }

  NSString *local = nsString(config.localName.trimmed());
  if (local.length > kMaxAdvertisedNameLength) {
    local = [local substringToIndex:kMaxAdvertisedNameLength];
  }
  if (local.length > 0) {
    data[CBAdvertisementDataLocalNameKey] = local;
  }

  [peripheralManager startAdvertising:data];
  advertising = true;
  LOG_INFO("bluetooth: advertising \"%s\"", qPrintable(config.localName));
}

void BtDarwinLinkPrivate::handlePeripheralState(CBManagerState state)
{
  if (!running) {
    return;
  }

  switch (state) {
  case CBManagerStatePoweredOn:
    if (service == nil) {
      addService();
    } else if (!advertising && !connected) {
      startAdvertising();
    }
    break;

  case CBManagerStateUnauthorized:
    fail(
        QStringLiteral("Bluetooth access is not allowed for Deskflow. Turn it on in System Settings > Privacy & "
                       "Security > Bluetooth.")
    );
    break;

  case CBManagerStateUnsupported:
    fail(QStringLiteral("This Mac does not support Bluetooth Low Energy."));
    break;

  case CBManagerStatePoweredOff:
    fail(QStringLiteral("Bluetooth is turned off. Turn it on to use the Bluetooth transport."));
    break;

  default:
    // Resetting/unknown: wait for the next state update.
    break;
  }
}

void BtDarwinLinkPrivate::handleServiceAdded(NSError *error)
{
  if (!running) {
    return;
  }
  if (error != nil) {
    fail(QStringLiteral("Unable to publish the Bluetooth service: %1").arg(errorText(error)));
    return;
  }
  startAdvertising();
}

void BtDarwinLinkPrivate::handleAdvertisingStarted(NSError *error)
{
  if (!running) {
    return;
  }
  if (error != nil) {
    advertising = false;
    fail(QStringLiteral("Unable to advertise over Bluetooth: %1").arg(errorText(error)));
    return;
  }
  LOG_INFO("bluetooth: advertising as \"%s\" (%s)", qPrintable(config.localName), qPrintable(q->describe()));
}

void BtDarwinLinkPrivate::handleSubscribed(CBCharacteristic *characteristic)
{
  if (!running || characteristic != txCharacteristicData) {
    return;
  }
  // A subscribed central is the only "link is up" signal the peripheral role
  // gets from CoreBluetooth.
  markConnected();
}

void BtDarwinLinkPrivate::handleUnsubscribed(CBCharacteristic *characteristic)
{
  if (!running || characteristic != txCharacteristicData) {
    return;
  }
  markDisconnected(QStringLiteral("the peer stopped listening"));
}

void BtDarwinLinkPrivate::handleWriteRequests(NSArray<CBATTRequest *> *requests)
{
  if (peripheralManager == nil) {
    return;
  }

  for (CBATTRequest *request in requests) {
    if (request.value != nil && request.value.length > 0) {
      const QByteArray chunk = QByteArray::fromNSData(request.value);
      if (!chunk.isEmpty() && running) {
        Q_EMIT q->chunkReceived(chunk);
      }
    }
    [peripheralManager respondToRequest:request withResult:CBATTErrorSuccess];
  }

  // Handling a request drains the transmit queue, so more data may fit now.
  if (running) {
    Q_EMIT q->writable();
  }
}

void BtDarwinLinkPrivate::handleReadRequest(CBATTRequest *request)
{
  if (peripheralManager == nil) {
    return;
  }
  request.value = [NSData data];
  [peripheralManager respondToRequest:request withResult:CBATTErrorSuccess];
}

void BtDarwinLinkPrivate::handleReadyToUpdate()
{
  if (!running) {
    return;
  }
  Q_EMIT q->writable();
}

// ---------------------------------------------------------------------------
// Central (client) role
// ---------------------------------------------------------------------------

void BtDarwinLinkPrivate::beginScanning()
{
  if (central == nil || scanning || connected) {
    return;
  }

  [central scanForPeripheralsWithServices:nil
                                  options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @NO}];
  scanning = true;
  const int timeout = config.connectTimeoutMs > 0 ? config.connectTimeoutMs : kDefaultScanTimeoutMs;
  if (attemptTimer != nullptr) {
    attemptTimer->start(timeout);
  }
  LOG_INFO("bluetooth: scanning for the Deskflow Bluetooth service");
}

bool BtDarwinLinkPrivate::matchesPeer(CBPeripheral *candidate, NSDictionary *advertisementData) const
{
  if (candidate == nil) {
    return false;
  }

  const QString wantedAddress = config.peerAddress.trimmed();
  if (!wantedAddress.isEmpty()) {
    const QString address = nsString(candidate.identifier.UUIDString);
    if (address.compare(wantedAddress, Qt::CaseInsensitive) == 0) {
      return true;
    }
  }

  const QString wantedName = config.peerName.trimmed();
  if (!wantedName.isEmpty()) {
    QString name = nsString(candidate.name);
    if (name.isEmpty() && advertisementData != nil) {
      id advertised = advertisementData[CBAdvertisementDataLocalNameKey];
      if ([advertised isKindOfClass:[NSString class]]) {
        name = nsString(static_cast<NSString *>(advertised));
      }
    }
    if (name.compare(wantedName, Qt::CaseInsensitive) == 0) {
      return true;
    }
  }

  // Nothing to match against: the scan filter already narrowed the field to
  // peripherals that advertise the Deskflow service.
  return wantedAddress.isEmpty() && wantedName.isEmpty();
}

void BtDarwinLinkPrivate::handleCentralState(CBManagerState state)
{
  if (!running) {
    return;
  }

  switch (state) {
  case CBManagerStatePoweredOn:
    if (!connected) {
      beginScanning();
    }
    break;

  case CBManagerStateUnauthorized:
    fail(
        QStringLiteral("Bluetooth access is not allowed for Deskflow. Turn it on in System Settings > Privacy & "
                       "Security > Bluetooth.")
    );
    break;

  case CBManagerStateUnsupported:
    fail(QStringLiteral("This Mac does not support Bluetooth Low Energy."));
    break;

  case CBManagerStatePoweredOff:
    fail(QStringLiteral("Bluetooth is turned off. Turn it on to use the Bluetooth transport."));
    break;

  default:
    break;
  }
}

void BtDarwinLinkPrivate::handleDiscovered(CBPeripheral *candidate, NSDictionary *advertisementData)
{
  if (!running || connected || connecting || candidate == nil) {
    return;
  }

  {
    QStringList services;
    id advertised = advertisementData[CBAdvertisementDataServiceUUIDsKey];
    if ([advertised isKindOfClass:[NSArray class]]) {
      for (CBUUID *uuid in static_cast<NSArray *>(advertised)) {
        services.append(nsString(uuid.UUIDString));
      }
    }
    LOG_DEBUG(
        "bluetooth: saw \"%s\" [%s] services=%s", qPrintable(nsString(candidate.name)),
        qPrintable(nsString(candidate.identifier.UUIDString)), qPrintable(services.join(QLatin1Char(',')))
    );
  }

  if (!matchesPeer(candidate, advertisementData)) {
    return;
  }

  connecting = true;
  if (scanning) {
    scanning = false;
    if (central != nil) {
      [central stopScan];
    }
  }

  QString name = nsString(candidate.name);
  if (name.isEmpty() && advertisementData != nil) {
    id advertised = advertisementData[CBAdvertisementDataLocalNameKey];
    if ([advertised isKindOfClass:[NSString class]]) {
      name = nsString(static_cast<NSString *>(advertised));
    }
  }
  peerLabel = name.isEmpty() ? nsString(candidate.identifier.UUIDString) : name;

  LOG_INFO("bluetooth: connecting to \"%s\"", qPrintable(peerLabel));
  if (attemptTimer != nullptr) {
    attemptTimer->start(config.connectTimeoutMs > 0 ? config.connectTimeoutMs : kDefaultScanTimeoutMs);
  }
  if (central != nil) {
    [central connectPeripheral:candidate options:nil];
  }
}

void BtDarwinLinkPrivate::handleConnectedPeripheral(CBPeripheral *peer)
{
  if (!running || peer == nil) {
    if (central != nil && peer != nil) {
      [central cancelPeripheralConnection:peer];
    }
    return;
  }

  peripheral = peer;
  peripheral.delegate = centralDelegate;
  connecting = false;
  if (attemptTimer != nullptr) {
    attemptTimer->start(config.connectTimeoutMs > 0 ? config.connectTimeoutMs : kDefaultScanTimeoutMs);
  }

  CBUUID *serviceUuid = uuidFromUtf8(kServiceUuid);
  [peripheral discoverServices:serviceUuid != nil ? @[ serviceUuid ] : nil];
  LOG_DEBUG("bluetooth: connected to the peer, discovering services");
}

void BtDarwinLinkPrivate::handleConnectFailed(NSError *error)
{
  if (!running) {
    return;
  }
  connecting = false;
  handleAttemptFailure(QStringLiteral("Unable to connect to the Bluetooth peer: %1").arg(errorText(error)));
}

void BtDarwinLinkPrivate::handleDisconnectedPeripheral(NSError *error)
{
  if (!running) {
    return;
  }
  peripheral = nil;
  rxCharacteristic = nil;
  txCharacteristic = nil;
  connecting = false;

  const QString reason =
      error != nil ? QStringLiteral("the peer disconnected: %1").arg(errorText(error))
                   : QStringLiteral("the peer disconnected");
  markDisconnected(reason);
  scheduleRetry();
}

void BtDarwinLinkPrivate::handleServices(CBPeripheral *peer, NSError *error)
{
  if (!running || peer != peripheral) {
    return;
  }
  if (error != nil) {
    handleAttemptFailure(QStringLiteral("Unable to discover the Bluetooth service: %1").arg(errorText(error)));
    return;
  }

  CBUUID *wanted = uuidFromUtf8(kServiceUuid);
  CBService *match = nil;
  for (CBService *candidate in peer.services) {
    if (wanted != nil && [candidate.UUID isEqual:wanted]) {
      match = candidate;
      break;
    }
  }
  if (match == nil) {
    handleAttemptFailure(QStringLiteral("The Bluetooth peer does not expose the Deskflow service"));
    return;
  }
  [peripheral discoverCharacteristics:nil forService:match];
}

void BtDarwinLinkPrivate::handleCharacteristics(CBPeripheral *peer, CBService *discovered, NSError *error)
{
  if (!running || peer != peripheral) {
    return;
  }
  if (error != nil) {
    handleAttemptFailure(QStringLiteral("Unable to discover the Bluetooth characteristics: %1").arg(errorText(error)));
    return;
  }

  CBUUID *rxUuid = uuidFromUtf8(kRxCharacteristicUuid);
  CBUUID *txUuid = uuidFromUtf8(kTxCharacteristicUuid);
  for (CBCharacteristic *candidate in discovered.characteristics) {
    if (txUuid != nil && [candidate.UUID isEqual:txUuid]) {
      txCharacteristic = candidate;
    } else if (rxUuid != nil && [candidate.UUID isEqual:rxUuid]) {
      rxCharacteristic = candidate;
    }
  }

  if (txCharacteristic == nil || rxCharacteristic == nil) {
    handleAttemptFailure(QStringLiteral("The Bluetooth peer is missing a Deskflow characteristic"));
    return;
  }

  [peripheral setNotifyValue:YES forCharacteristic:txCharacteristic];
}

void BtDarwinLinkPrivate::handleNotifyState(CBCharacteristic *characteristic, NSError *error)
{
  if (!running || characteristic != txCharacteristic) {
    return;
  }
  if (error != nil) {
    handleAttemptFailure(QStringLiteral("Unable to subscribe to the Bluetooth peer: %1").arg(errorText(error)));
    return;
  }
  if (characteristic.isNotifying) {
    markConnected();
  } else {
    markDisconnected(QStringLiteral("the peer stopped notifying"));
    scheduleRetry();
  }
}

void BtDarwinLinkPrivate::handleValue(CBCharacteristic *characteristic, NSError *error)
{
  if (!running || characteristic != txCharacteristic) {
    return;
  }
  if (error != nil) {
    LOG_WARN("bluetooth: notification failed: %s", qPrintable(errorText(error)));
    return;
  }
  if (characteristic.value == nil || characteristic.value.length == 0) {
    return;
  }
  Q_EMIT q->chunkReceived(QByteArray::fromNSData(characteristic.value));
}

void BtDarwinLinkPrivate::handleWriteResult(CBCharacteristic *characteristic, NSError *error)
{
  if (!running || characteristic != rxCharacteristic) {
    return;
  }
  if (error != nil) {
    LOG_WARN("bluetooth: write to the peer failed: %s", qPrintable(errorText(error)));
  }
  Q_EMIT q->writable();
}

void BtDarwinLinkPrivate::handleReadyToWrite()
{
  if (!running) {
    return;
  }
  Q_EMIT q->writable();
}

void BtDarwinLinkPrivate::handleAttemptTimeout()
{
  if (!running || connected) {
    return;
  }
  const QString reason = scanning ? QStringLiteral("no Bluetooth peer was found")
                                  : QStringLiteral("the Bluetooth peer did not answer in time");
  handleAttemptFailure(reason);
}

void BtDarwinLinkPrivate::handleRetryTimeout()
{
  if (!running || connected) {
    return;
  }

  if (mode == Mode::Client) {
    if (central == nil) {
      beginClient();
      return;
    }
    if (central.state == CBManagerStatePoweredOn) {
      connecting = false;
      beginScanning();
    }
    return;
  }

  if (peripheralManager != nil && peripheralManager.state == CBManagerStatePoweredOn && !advertising) {
    if (service == nil) {
      addService();
    } else {
      startAdvertising();
    }
  }
}

// ---------------------------------------------------------------------------
// Qt wrapper
// ---------------------------------------------------------------------------

BtDarwinLink::BtDarwinLink(QObject *parent) : IBtLink(parent), d(new BtDarwinLinkPrivate)
{
  d->q = this;

  d->retryTimer = new QTimer(this);
  d->retryTimer->setSingleShot(true);
  connect(d->retryTimer, &QTimer::timeout, this, [this] { d->handleRetryTimeout(); });

  d->attemptTimer = new QTimer(this);
  d->attemptTimer->setSingleShot(true);
  connect(d->attemptTimer, &QTimer::timeout, this, [this] { d->handleAttemptTimeout(); });
}

BtDarwinLink::~BtDarwinLink()
{
  d->running = false;
  d->teardown();
}

void BtDarwinLink::startServer(const Config &config)
{
  // A restart (the relay drives those) must never look like a link failure.
  d->running = false;
  d->teardown();

  d->config = config;
  d->mode = Mode::Server;
  d->running = true;

  LOG_INFO("bluetooth: starting the CoreBluetooth peripheral \"%s\"", qPrintable(config.localName));
  d->beginServer();
}

void BtDarwinLink::startClient(const Config &config)
{
  d->running = false;
  d->teardown();

  d->config = config;
  d->mode = Mode::Client;
  d->running = true;

  LOG_INFO(
      "bluetooth: starting the CoreBluetooth central (peer \"%s\")",
      qPrintable(config.peerName.isEmpty() ? config.peerAddress : config.peerName)
  );
  d->beginClient();
}

void BtDarwinLink::stop()
{
  d->running = false;
  d->teardown();
}

bool BtDarwinLink::isConnected() const
{
  return d->connected;
}

int BtDarwinLink::maxChunkSize() const
{
  return d->computeChunkSize();
}

bool BtDarwinLink::sendChunk(const QByteArray &chunk)
{
  return d->sendChunk(chunk);
}

QString BtDarwinLink::describe() const
{
  if (d->mode == Mode::Server) {
    const QString name = d->config.localName.trimmed();
    return QStringLiteral("BLE darwin peripheral \"%1\" (%2)")
        .arg(name.isEmpty() ? QStringLiteral("Deskflow") : name, d->stateLabel());
  }

  QString peer = d->peerLabel;
  if (peer.isEmpty()) {
    peer = d->config.peerName.trimmed();
  }
  if (peer.isEmpty()) {
    peer = d->config.peerAddress.trimmed();
  }
  return QStringLiteral("BLE darwin central -> %1 (%2)")
      .arg(peer.isEmpty() ? QStringLiteral("any peer") : peer, d->stateLabel());
}

} // namespace deskflow::net::bt
