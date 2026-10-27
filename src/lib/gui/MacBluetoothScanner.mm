/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MacBluetoothScanner.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <QSet>
#include <QTimer>

@class DeskflowBtScannerDelegate;

//! Service UUID the Deskflow link advertises; see \c net/bluetooth/BtTypes.h.
static NSString *const kDeskflowServiceUuid = @"3F2A7C1E-9B64-4D58-8A11-6C0D5E7B9F20";

namespace deskflow::gui {

class MacBluetoothScanner::Private
{
public:
  MacBluetoothScanner *q = nullptr;
  CBCentralManager *manager = nil;
  DeskflowBtScannerDelegate *delegate = nil;
  QTimer *timeout = nullptr;
  QSet<QString> seen;
  bool wanted = false;
  bool scanning = false;
  bool failureReported = false;

  void applyState(CBManagerState state);
  void beginScanning();
  void addDevice(const QString &name, const QString &identifier);
  void reportFailure(const QString &reason);
  void endScan();
};

} // namespace deskflow::gui

//! Receives the CoreBluetooth callbacks and forwards them to the private state.
/*!
Declared after \c Private so the delegate can hold a typed pointer to it; the
delegate itself is never handed out, it only exists to keep \c CBCentralManager
alive with a real delegate.
*/
@interface DeskflowBtScannerDelegate : NSObject <CBCentralManagerDelegate>
@property(nonatomic, assign) deskflow::gui::MacBluetoothScanner::Private *owner;
@end

@implementation DeskflowBtScannerDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
  if (self.owner == nullptr) {
    return;
  }
  self.owner->applyState(central.state);
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
        advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                     RSSI:(NSNumber *)RSSI
{
  (void)central;
  (void)RSSI;
  if (self.owner == nullptr) {
    return;
  }

  // Only report peripherals that advertise the Deskflow service.  This keeps
  // the list to the computers running Deskflow (the ones that can share a
  // keyboard and mouse), instead of every Bluetooth device in range.
  id services = advertisementData[CBAdvertisementDataServiceUUIDsKey];
  if (![services isKindOfClass:[NSArray class]]) {
    return;
  }
  bool isDeskflowPeer = false;
  for (CBUUID *uuid in static_cast<NSArray *>(services)) {
    if ([uuid.UUIDString caseInsensitiveCompare:kDeskflowServiceUuid] == NSOrderedSame) {
      isDeskflowPeer = true;
      break;
    }
  }
  if (!isDeskflowPeer) {
    return;
  }

  NSString *name = peripheral.name;
  if (name.length == 0) {
    id advertised = advertisementData[CBAdvertisementDataLocalNameKey];
    if ([advertised isKindOfClass:[NSString class]]) {
      name = static_cast<NSString *>(advertised);
    }
  }
  if (name.length == 0) {
    name = @"Deskflow computer";
  }

  // Mark it clearly so the user can tell it apart from any classic Bluetooth
  // devices that might still be listed.
  name = [name stringByAppendingString:@" (Deskflow)"];

  self.owner->addDevice(QString::fromNSString(name), QString::fromNSString(peripheral.identifier.UUIDString));
}

@end

namespace deskflow::gui {

void MacBluetoothScanner::Private::reportFailure(const QString &reason)
{
  if (failureReported) {
    return;
  }
  failureReported = true;
  wanted = false;
  scanning = false;
  if (timeout != nullptr) {
    timeout->stop();
  }
  Q_EMIT q->failed(reason);
}

void MacBluetoothScanner::Private::endScan()
{
  wanted = false;
  if (!scanning) {
    return;
  }
  scanning = false;
  if (manager != nil) {
    [manager stopScan];
  }
  Q_EMIT q->finished();
}

void MacBluetoothScanner::Private::beginScanning()
{
  if (scanning || manager == nil) {
    return;
  }
  // Every service, so the list also shows devices that do not advertise the
  // Deskflow UUID yet (the peer may still be starting up).
  [manager scanForPeripheralsWithServices:nil
                                  options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @NO}];
  scanning = true;
  if (timeout != nullptr && timeout->interval() > 0) {
    timeout->start();
  }
}

void MacBluetoothScanner::Private::applyState(CBManagerState state)
{
  switch (state) {
  case CBManagerStatePoweredOn:
    if (wanted) {
      beginScanning();
    }
    break;

  case CBManagerStateUnauthorized:
    reportFailure(
        QStringLiteral("Bluetooth access is not allowed for Deskflow. Turn it on in System Settings > Privacy & "
                       "Security > Bluetooth.")
    );
    break;

  case CBManagerStateUnsupported:
    reportFailure(QStringLiteral("This Mac does not support Bluetooth LE."));
    break;

  case CBManagerStatePoweredOff:
    reportFailure(QStringLiteral("Bluetooth is turned off."));
    break;

  default:
    // Resetting/unknown: wait for the next state update.
    break;
  }
}

void MacBluetoothScanner::Private::addDevice(const QString &name, const QString &identifier)
{
  if (!wanted || identifier.isEmpty() || seen.contains(identifier)) {
    return;
  }
  seen.insert(identifier);
  Q_EMIT q->deviceFound(name, identifier);
}

MacBluetoothScanner::MacBluetoothScanner(QObject *parent) : QObject(parent), d(new Private)
{
  d->q = this;

  d->delegate = [[DeskflowBtScannerDelegate alloc] init];
  d->delegate.owner = d;

  d->timeout = new QTimer(this);
  d->timeout->setSingleShot(true);
  connect(d->timeout, &QTimer::timeout, this, [this] { d->endScan(); });

  // Creating the manager is what makes macOS ask for the Bluetooth grant; the
  // state callback arrives asynchronously, so start() only records the wish.
  d->manager = [[CBCentralManager alloc] initWithDelegate:d->delegate queue:dispatch_get_main_queue()];
}

MacBluetoothScanner::~MacBluetoothScanner()
{
  if (d->manager != nil) {
    [d->manager stopScan];
    d->manager = nil;
  }
  d->delegate.owner = nullptr;
  d->delegate = nil;
  delete d;
}

bool MacBluetoothScanner::isScanning() const
{
  return d->scanning;
}

void MacBluetoothScanner::start(int timeoutMs)
{
  d->seen.clear();
  d->failureReported = false;
  d->wanted = true;
  d->timeout->setInterval(timeoutMs > 0 ? timeoutMs : 0);

  if (d->manager.state == CBManagerStatePoweredOn) {
    d->beginScanning();
  }
  // Otherwise applyState() starts the scan once the manager is powered on.
}

void MacBluetoothScanner::stop()
{
  d->wanted = false;
  d->timeout->stop();
  if (d->manager != nil && d->scanning) {
    [d->manager stopScan];
  }
  d->scanning = false;
}

} // namespace deskflow::gui
