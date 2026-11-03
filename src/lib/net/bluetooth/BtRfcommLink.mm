/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtRfcommLink.h"

#include "base/Log.h"
#include "net/bluetooth/BtTypes.h"

#include <QTimer>

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

namespace deskflow::net::bt {

namespace {

//! Raw bytes of \c kServiceUuid, the UUID Windows publishes its serial port under.
constexpr unsigned char kDeskflowServiceUuidBytes[16] = {0x3f, 0x2a, 0x7c, 0x1e, 0x9b, 0x64, 0x4d, 0x58,
                                                         0x8a, 0x11, 0x6c, 0x0d, 0x5e, 0x7b, 0x9f, 0x20};

//! Chunk size for RFCOMM payloads; the link itself can carry more.
constexpr int kRfcommMaxChunk = 512;

//! How long to wait for the channel open callback before reporting a failure.
constexpr int kChannelOpenTimeoutMs = 15000;

//! SDP runs in the background, so the Deskflow record can be missing on the first look.
constexpr int kSdpAttempts = 5;
constexpr int kSdpRetryMs = 1000;

} // namespace

} // namespace deskflow::net::bt

using deskflow::net::bt::BtRfcommLink;

//! Forwards IOBluetooth RFCOMM callbacks into the link's signals.
/*!
Objective-C declarations have to live at global scope, so this class sits
outside the Deskflow namespace and only refers to the link through the class
name brought in by the using-declaration above.
*/
@interface DeskflowRfcommDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate>
@property(nonatomic, assign) void *owner;
@end

@implementation DeskflowRfcommDelegate

- (void)rfcommChannelOpenComplete:(IOBluetoothRFCOMMChannel *)channel status:(IOReturn)error
{
  if (self.owner != nullptr) {
    // The channel is handed over already retained; the link adopts it only when
    // the open reports success, so a rejected attempt cannot leak it.
    static_cast<BtRfcommLink *>(self.owner)->handleChannelOpenStatus(
        static_cast<int>(error), (__bridge void *)channel
    );
  }
}

- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel *)channel data:(void *)dataPointer length:(size_t)dataLength
{
  (void)channel;
  if (self.owner != nullptr && dataPointer != nullptr && dataLength > 0) {
    auto *link = static_cast<BtRfcommLink *>(self.owner);
    const QByteArray chunk(static_cast<const char *>(dataPointer), static_cast<int>(dataLength));
    Q_EMIT link->chunkReceived(chunk);
  }
}

- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel *)channel
{
  (void)channel;
  if (self.owner != nullptr) {
    auto *link = static_cast<BtRfcommLink *>(self.owner);
    Q_EMIT link->disconnected(QStringLiteral("the Bluetooth data channel closed"));
  }
}

@end

namespace deskflow::net::bt {

class BtRfcommLink::Private
{
public:
  BtRfcommLink *q = nullptr;
  Mode mode = Mode::Client;
  Config config;

  IOBluetoothDevice *device = nil;
  IOBluetoothRFCOMMChannel *channel = nil;
  DeskflowRfcommDelegate *delegate = nil;
  QTimer *retryTimer = nullptr;
  QTimer *openTimer = nullptr;

  bool running = false;
  bool connected = false;
  bool connecting = false;
  int sdpAttempt = 0;
  QString peerLabel;

  IOBluetoothDevice *findPairedDevice(const QString &wanted);
  IOBluetoothSDPServiceRecord *findDeskflowRecord();
  void connectPeer();
  void discoverAndOpen();
  void openChannel(BluetoothRFCOMMChannelID channelId);
  void onChannelOpenFinished(int status, void *openedChannel);
  void markConnected();
  void onAttemptFailed(const QString &reason);
};

#else // !__APPLE__

namespace deskflow::net::bt {

class BtRfcommLink::Private
{
public:
  BtRfcommLink *q = nullptr;
};

#endif

BtRfcommLink::BtRfcommLink(QObject *parent) : IBtLink(parent), d(new Private)
{
  d->q = this;
#ifdef __APPLE__
  d->retryTimer = new QTimer(this);
  d->retryTimer->setSingleShot(true);
  connect(d->retryTimer, &QTimer::timeout, this, [this] {
    if (d->running && !d->connected && d->mode == Mode::Client) {
      d->connectPeer();
    }
  });

  d->openTimer = new QTimer(this);
  d->openTimer->setSingleShot(true);
  connect(d->openTimer, &QTimer::timeout, this, [this] {
    if (!d->running || d->connected) {
      return;
    }
    // macOS can accept the open request and never call back, so the only way
    // out of "connecting" is this timer.
    LOG_WARN("bluetooth/rfcomm: the channel open timed out after %d ms", kChannelOpenTimeoutMs);
    d->connecting = false;
    d->onAttemptFailed(QStringLiteral("The other computer did not accept the Bluetooth connection."));
  });
#endif
}

BtRfcommLink::~BtRfcommLink()
{
  stop();
  delete d;
}

void BtRfcommLink::startServer(const Config &config)
{
  Q_UNUSED(config);
#ifdef __APPLE__
  // macOS does not let an application register an RFCOMM/SPP server: the
  // listening side of IOBluetooth was dropped from the 64-bit framework.  So on
  // this platform the classic transport can only be the client; the other
  // computer (Windows Deskflow can listen) has to be the server.
  Q_EMIT errorOccurred(
      QStringLiteral("This Mac cannot listen on classic Bluetooth. Use it as the client, or use the Bluetooth "
                     "(BLE) transport for the server side.")
  );
#else
  Q_EMIT errorOccurred(QStringLiteral("Classic Bluetooth is not available in this build"));
#endif
}

void BtRfcommLink::startClient(const Config &config)
{
#ifdef __APPLE__
  d->mode = Mode::Client;
  d->config = config;
  d->running = true;
  if (d->retryTimer != nullptr) {
    d->retryTimer->stop();
  }
  d->connectPeer();
#else
  Q_UNUSED(config);
  Q_EMIT errorOccurred(QStringLiteral("Classic Bluetooth is not available in this build"));
#endif
}

void BtRfcommLink::stop()
{
#ifdef __APPLE__
  d->running = false;
  d->connecting = false;
  if (d->retryTimer != nullptr) {
    d->retryTimer->stop();
  }
  if (d->openTimer != nullptr) {
    d->openTimer->stop();
  }
  if (d->channel != nil) {
    @autoreleasepool {
      [d->channel closeChannel];
    }
    d->channel = nil;
  }
  d->device = nil;
  d->connected = false;
#endif
}

bool BtRfcommLink::isConnected() const
{
  return d->connected;
}

int BtRfcommLink::maxChunkSize() const
{
#ifdef __APPLE__
  return kRfcommMaxChunk;
#else
  return 180;
#endif
}

bool BtRfcommLink::sendChunk(const QByteArray &chunk)
{
#ifdef __APPLE__
  if (!d->connected || d->channel == nil || chunk.isEmpty()) {
    return false;
  }
  @autoreleasepool {
    const IOReturn rc = [d->channel writeAsync:const_cast<char *>(chunk.constData())
                                        length:static_cast<NSUInteger>(chunk.size())
                                        refcon:nullptr];
    if (rc != kIOReturnSuccess) {
      LOG_WARN("bluetooth/rfcomm: write failed (0x%x)", rc);
      return false;
    }
    // RFCOMM carries its own credits, so an accepted write is the flow control.
    QTimer::singleShot(0, this, [this] { Q_EMIT writable(); });
    return true;
  }
#else
  Q_UNUSED(chunk);
  return false;
#endif
}

QString BtRfcommLink::describe() const
{
#ifdef __APPLE__
  const QString state = d->connected ? QStringLiteral("connected")
                                     : (d->connecting ? QStringLiteral("connecting") : QStringLiteral("idle"));
  return QStringLiteral("classic Bluetooth -> %1 (%2)")
      .arg(d->peerLabel.isEmpty() ? QStringLiteral("paired computer") : d->peerLabel, state);
#else
  return QStringLiteral("classic Bluetooth (unavailable)");
#endif
}

#ifdef __APPLE__

void BtRfcommLink::handleChannelOpenStatus(int status, void *openedChannel)
{
  d->onChannelOpenFinished(status, openedChannel);
}

IOBluetoothDevice *BtRfcommLink::Private::findPairedDevice(const QString &wanted)
{
  @autoreleasepool {
    if (!wanted.isEmpty()) {
      NSString *wantedNs = [NSString stringWithUTF8String:wanted.toUtf8().constData()];
      IOBluetoothDevice *byAddress = [IOBluetoothDevice deviceWithAddressString:wantedNs];
      if (byAddress != nil && [byAddress isPaired]) {
        return byAddress;
      }
      for (IOBluetoothDevice *candidate in [IOBluetoothDevice pairedDevices]) {
        const QString name = QString::fromNSString([candidate nameOrAddress]);
        const QString address = QString::fromNSString([candidate addressString]);
        if (name.compare(wanted, Qt::CaseInsensitive) == 0 || address.compare(wanted, Qt::CaseInsensitive) == 0) {
          return candidate;
        }
      }
      return nil;
    }

    // Nothing configured: take the first paired device that is not a peripheral.
    for (IOBluetoothDevice *candidate in [IOBluetoothDevice pairedDevices]) {
      if (![candidate isPaired]) {
        continue;
      }
      const BluetoothDeviceClassMinor minor = [candidate deviceClassMinor];
      if (minor == kBluetoothDeviceClassMinorPeripheral1Keyboard ||
          minor == kBluetoothDeviceClassMinorPeripheral1Pointing) {
        continue;
      }
      return candidate;
    }
    return nil;
  }
}

IOBluetoothSDPServiceRecord *BtRfcommLink::Private::findDeskflowRecord()
{
  if (device == nil) {
    return nil;
  }

  @autoreleasepool {
    // Match on the service class UUID Windows publishes.  Taking "the first
    // record that has an RFCOMM channel" picks one of the computer's own
    // services (audio, HID, ...) and fails on a channel that is not ours.
    IOBluetoothSDPUUID *uuid =
        [IOBluetoothSDPUUID uuidWithBytes:kDeskflowServiceUuidBytes length:sizeof(kDeskflowServiceUuidBytes)];
    IOBluetoothSDPServiceRecord *record = [device getServiceRecordForUUID:uuid];
    if (record != nil) {
      return record;
    }

    // A record published by an older build may carry only the service name.
    for (IOBluetoothSDPServiceRecord *candidate in [device services]) {
      if ([[candidate getServiceName] isEqualToString:@"Deskflow"]) {
        return candidate;
      }
    }
    return nil;
  }
}

void BtRfcommLink::Private::connectPeer()
{
  if (!running || connected || connecting) {
    return;
  }

  QString wanted = config.peerAddress.trimmed();
  if (wanted.isEmpty()) {
    wanted = config.peerName.trimmed();
  }

  device = findPairedDevice(wanted);
  if (device == nil) {
    LOG_WARN("bluetooth/rfcomm: no paired computer found (looked for \"%s\")", qPrintable(wanted));
    onAttemptFailed(
        QStringLiteral("No paired Bluetooth computer was found. Pair this Mac with the other computer in System "
                       "Settings first.")
    );
    return;
  }

  peerLabel = QString::fromNSString([device nameOrAddress]);
  LOG_INFO("bluetooth/rfcomm: using paired device \"%s\"", qPrintable(peerLabel));

  connecting = true;
  sdpAttempt = 0;

  @autoreleasepool {
    if (![device isConnected]) {
      LOG_DEBUG("bluetooth/rfcomm: opening the baseband connection");
      [device openConnection];
    }
  }

  // Let the baseband link come up, then look for the Deskflow service.
  QTimer::singleShot(2000, q, [this] {
    if (!running || connected) {
      return;
    }
    if (device == nil || ![device isConnected]) {
      connecting = false;
      onAttemptFailed(QStringLiteral("The Bluetooth link to the other computer did not come up."));
      return;
    }
    discoverAndOpen();
  });
}

void BtRfcommLink::Private::discoverAndOpen()
{
  if (!running || connected || device == nil) {
    return;
  }

  @autoreleasepool {
    if (sdpAttempt == 0) {
      // Starts the exchange; the records land in the cache asynchronously, so the
      // lookups below are repeated on a timer rather than back to back.
      [device performSDPQuery:nil];
    }

    IOBluetoothSDPServiceRecord *record = findDeskflowRecord();
    if (record == nil && ++sdpAttempt < kSdpAttempts) {
      LOG_DEBUG("bluetooth/rfcomm: the Deskflow service is not in the SDP cache yet (attempt %d)", sdpAttempt);
      QTimer::singleShot(kSdpRetryMs, q, [this] { discoverAndOpen(); });
      return;
    }

    if (record == nil) {
      connecting = false;
      LOG_WARN("bluetooth/rfcomm: \"%s\" publishes no Deskflow service", qPrintable(peerLabel));
      onAttemptFailed(
          QStringLiteral("The other computer does not publish a Deskflow Bluetooth service. Start Deskflow there "
                         "with the Bluetooth transport enabled.")
      );
      return;
    }

    BluetoothRFCOMMChannelID channelId = 0;
    const IOReturn rc = [record getRFCOMMChannelID:&channelId];
    if (rc != kIOReturnSuccess || channelId == 0) {
      connecting = false;
      LOG_WARN("bluetooth/rfcomm: the Deskflow record carries no RFCOMM channel (0x%x)", rc);
      onAttemptFailed(QStringLiteral("The other computer's Bluetooth service exposes no serial channel."));
      return;
    }

    LOG_INFO("bluetooth/rfcomm: the Deskflow service is on channel %d, opening it", static_cast<int>(channelId));
    openChannel(channelId);
  }
}

void BtRfcommLink::Private::openChannel(BluetoothRFCOMMChannelID channelId)
{
  if (!running || connected || device == nil) {
    return;
  }

  @autoreleasepool {
    if (delegate == nil) {
      delegate = [[DeskflowRfcommDelegate alloc] init];
      delegate.owner = q;
    }

    IOBluetoothRFCOMMChannel *opened = nil;
    const IOReturn rc = [device openRFCOMMChannelAsync:&opened withChannelID:channelId delegate:(id)delegate];
    if (rc != kIOReturnSuccess) {
      connecting = false;
      LOG_WARN("bluetooth/rfcomm: cannot start opening channel %d (0x%x)", static_cast<int>(channelId), rc);
      onAttemptFailed(
          QStringLiteral("The Bluetooth serial channel could not be opened (error 0x%1).").arg(rc, 0, 16)
      );
      return;
    }

    if (openTimer != nullptr) {
      openTimer->start(kChannelOpenTimeoutMs);
    }
  }
}

void BtRfcommLink::Private::onChannelOpenFinished(int status, void *openedChannel)
{
  if (openTimer != nullptr) {
    openTimer->stop();
  }
  if (!running || connected) {
    return;
  }

  if (status == kIOReturnSuccess && openedChannel != nullptr) {
    // Assigning to the strong member adopts the channel, so the completion
    // callback can let it go.
    channel = (__bridge IOBluetoothRFCOMMChannel *)openedChannel;
    LOG_INFO("bluetooth/rfcomm: the link to \"%s\" is up (MTU %u)", qPrintable(peerLabel), [channel getMTU]);
    connecting = false;
    markConnected();
    return;
  }

  connecting = false;
  LOG_WARN("bluetooth/rfcomm: the channel open failed (0x%x)", static_cast<unsigned>(status));
  onAttemptFailed(
      QStringLiteral("The Bluetooth serial channel was refused (error 0x%1).").arg(static_cast<unsigned>(status), 0, 16)
  );
}

void BtRfcommLink::Private::markConnected()
{
  if (connected) {
    return;
  }
  connected = true;
  connecting = false;
  if (retryTimer != nullptr) {
    retryTimer->stop();
  }
  Q_EMIT q->connected();
  Q_EMIT q->writable();
}

void BtRfcommLink::Private::onAttemptFailed(const QString &reason)
{
  Q_EMIT q->errorOccurred(reason);
  if (!running || retryTimer == nullptr) {
    return;
  }
  retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
}

#endif // __APPLE__

} // namespace deskflow::net::bt
