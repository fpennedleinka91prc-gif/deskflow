/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtRfcommLink.h"

#include "base/Log.h"

#include <QTimer>

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

namespace deskflow::net::bt {

namespace {

//! Channels tried when SDP does not name a serial port.
constexpr BluetoothRFCOMMChannelID kCandidateChannels[] = {1, 2, 3, 4, 5, 6};

//! Chunk size for RFCOMM payloads; the link itself can carry more.
constexpr int kRfcommMaxChunk = 512;

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

  bool running = false;
  bool connected = false;
  bool connecting = false;
  QString peerLabel;

  IOBluetoothDevice *findPairedDevice(const QString &wanted);
  void connectPeer();
  void openChannel(BluetoothRFCOMMChannelID channelId);
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

  @autoreleasepool {
    if (![device isConnected]) {
      LOG_DEBUG("bluetooth/rfcomm: opening the baseband connection");
      [device openConnection];
    }
  }

  // Let the baseband link come up, then look for a serial port channel.
  QTimer::singleShot(2000, q, [this] {
    if (!running || connected) {
      return;
    }
    if (device == nil || ![device isConnected]) {
      connecting = false;
      onAttemptFailed(QStringLiteral("The Bluetooth link to the other computer did not come up."));
      return;
    }

    BluetoothRFCOMMChannelID discovered = 0;
    @autoreleasepool {
      [device performSDPQuery:nil];
      for (IOBluetoothSDPServiceRecord *record in [device services]) {
        if ([record getRFCOMMChannelID:&discovered] == kIOReturnSuccess && discovered > 0) {
          break;
        }
      }
    }

    if (discovered > 0) {
      LOG_DEBUG("bluetooth/rfcomm: SDP reports channel %d", static_cast<int>(discovered));
      openChannel(discovered);
      if (connected) {
        return;
      }
    }

    for (BluetoothRFCOMMChannelID candidate : kCandidateChannels) {
      if (connected || !running) {
        break;
      }
      openChannel(candidate);
    }

    if (!connected) {
      connecting = false;
      onAttemptFailed(
          QStringLiteral("No Bluetooth data channel could be opened. The other computer must offer a serial port "
                         "service (Windows Deskflow does; macOS cannot).")
      );
    }
  });
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
    const IOReturn rc = [device openRFCOMMChannelSync:&opened withChannelID:channelId delegate:(id)delegate];
    if (rc == kIOReturnSuccess && opened != nil && [opened isOpen]) {
      LOG_INFO(
          "bluetooth/rfcomm: channel %d is open, the link to \"%s\" is up", static_cast<int>(channelId),
          qPrintable(peerLabel)
      );
      channel = opened;
      markConnected();
    } else {
      LOG_DEBUG("bluetooth/rfcomm: channel %d not usable (0x%x)", static_cast<int>(channelId), rc);
    }
  }
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
