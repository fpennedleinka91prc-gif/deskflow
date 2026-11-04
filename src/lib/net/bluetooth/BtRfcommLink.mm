/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtRfcommLink.h"

#include "base/Log.h"
#include "net/bluetooth/BtTypes.h"

#include <QTimer>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <atomic>

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

//! How long a freshly opened channel may stay silent before it is considered a
//! stale record (the peer advertised it, but nothing is listening on it).
constexpr int kPeerDataTimeoutMs = 3500;

//! SDP runs in the background, so the Deskflow record can be missing on the first look.
constexpr int kSdpAttempts = 5;
constexpr int kSdpRetryMs = 1000;

//! Delay before offering the link writable() again after a rejected write.
/*!
IOBluetooth rejects a write with \c kIOReturnNotOpen while the channel is still
being opened by the remote stack, so a failed write is retried rather than
treated as a dead link.
*/
constexpr int kWriteRetryMs = 50;

//! Consecutive rejected writes (about kWriteRetryMs each) before the channel is
//! considered stale and the link is re-established.
constexpr int kWriteFailuresBeforeReconnect = 60;

//! Human readable name for the IOReturn values the write path can produce.
const char *iorReturnName(IOReturn rc)
{
  switch (rc) {
  case kIOReturnSuccess:
    return "kIOReturnSuccess";
  case kIOReturnError:
    return "kIOReturnError";
  case kIOReturnNoDevice:
    return "kIOReturnNoDevice";
  case kIOReturnBadArgument:
    return "kIOReturnBadArgument";
  case kIOReturnUnsupported:
    return "kIOReturnUnsupported";
  case kIOReturnIOError:
    return "kIOReturnIOError";
  case kIOReturnNotOpen:
    return "kIOReturnNotOpen";
  case kIOReturnNotReadable:
    return "kIOReturnNotReadable";
  case kIOReturnNotWritable:
    return "kIOReturnNotWritable";
  case kIOReturnAborted:
    return "kIOReturnAborted";
  case kIOReturnNotFound:
    return "kIOReturnNotFound";
  default:
    return "unknown IOReturn";
  }
}

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

- (void)sdpQueryComplete:(IOBluetoothDevice *)device status:(IOReturn)status
{
  (void)device;
  if (self.owner != nullptr) {
    static_cast<BtRfcommLink *>(self.owner)->handleSdpQueryComplete(static_cast<int>(status));
  }
}

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
    // Any inbound byte proves the advertised channel really reaches the peer.
    link->handlePeerData();
    const QByteArray chunk(static_cast<const char *>(dataPointer), static_cast<int>(dataLength));
    Q_EMIT link->chunkReceived(chunk);
  }
}

- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel *)channel
{
  (void)channel;
  if (self.owner != nullptr) {
    static_cast<BtRfcommLink *>(self.owner)->handleChannelClosed();
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
  //! Set from the SDP completion callback; the cache is stale until it fires.
  bool sdpQueryFinished = false;
  //! True between openRFCOMMChannelAsync and its completion callback.
  bool channelOpening = false;
  QString peerLabel;

  //! RFCOMM channels of every Deskflow record the peer advertises, newest first.
  /*!
  Windows keeps SDP records of earlier runs alive, so the peer can advertise
  several Deskflow records at different channels; one of them is stale.  They
  are tried in order until one of them actually answers.
  */
  QVector<int> candidates;
  int candidateIndex = -1;
  //! Channel of the open request currently in flight (the channel object does
  //! not expose its own channel id).
  int pendingChannel = -1;
  //! Set until the peer sends its first bytes on the channel that was just opened.
  std::atomic<bool> awaitingPeer{false};
  QTimer *peerDataTimer = nullptr;

  //! Last IOReturn a write was rejected with; a repeated value is logged once.
  IOReturn lastWriteError = kIOReturnSuccess;
  quint64 writeFailures = 0;
  bool writePending = false;

  IOBluetoothDevice *findPairedDevice(const QString &wanted);
  QVector<int> deskflowChannels();
  void connectPeer();
  void discoverAndOpen();
  void openChannel(BluetoothRFCOMMChannelID channelId);
  void openNextCandidate(const QString &reason);
  void onPeerSilent();
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
    d->openNextCandidate(QStringLiteral("the channel open timed out"));
  });

  d->peerDataTimer = new QTimer(this);
  d->peerDataTimer->setSingleShot(true);
  connect(d->peerDataTimer, &QTimer::timeout, this, [this] { d->onPeerSilent(); });
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
  d->awaitingPeer = false;
  d->candidates.clear();
  d->candidateIndex = -1;
  if (d->retryTimer != nullptr) {
    d->retryTimer->stop();
  }
  if (d->openTimer != nullptr) {
    d->openTimer->stop();
  }
  if (d->peerDataTimer != nullptr) {
    d->peerDataTimer->stop();
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
      // kIOReturnNotOpen means the channel exists but the open is not finished
      // yet (the remote stack accepts a few milliseconds later).  Handing the
      // bytes back and asking the relay to retry is correct; failing the whole
      // connection over one rejected write is not.  Log only when the error
      // changes so a spinning retry cannot flood the log.
      if (rc != d->lastWriteError) {
        d->lastWriteError = rc;
        LOG_WARN(
            "bluetooth/rfcomm: write deferred (%s 0x%x), retrying every %d ms (mtu %u, failures %llu)",
            deskflow::net::bt::iorReturnName(rc), static_cast<unsigned>(rc), kWriteRetryMs,
            [d->channel getMTU], d->writeFailures
        );
      } else {
        LOG_DEBUG(
            "bluetooth/rfcomm: write still deferred (%s 0x%x, failures %llu)",
            deskflow::net::bt::iorReturnName(rc), static_cast<unsigned>(rc), d->writeFailures
        );
      }
      if (++d->writeFailures >= kWriteFailuresBeforeReconnect) {
        // The channel has been unwritable for about 3 s: it is stale (the peer
        // restarted and IOBluetooth has not reported the close yet).  Tear it
        // down so the normal reconnect path runs.
        LOG_WARN("bluetooth/rfcomm: the channel stayed unwritable for %llu attempts, reconnecting",
                 d->writeFailures);
        d->writeFailures = 0;
        d->lastWriteError = kIOReturnSuccess;
        QTimer::singleShot(0, this, [this] { handleChannelClosed(); });
        return false;
      }
      if (!d->writePending) {
        d->writePending = true;
        QTimer::singleShot(kWriteRetryMs, this, [this] {
          d->writePending = false;
          if (d->connected) {
            Q_EMIT writable();
          }
        });
      }
      return false;
    }
    d->lastWriteError = kIOReturnSuccess;
    d->writeFailures = 0;
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

void BtRfcommLink::handleSdpQueryComplete(int status)
{
  LOG_DEBUG("bluetooth/rfcomm: the SDP query finished (0x%x)", static_cast<unsigned>(status));
  QTimer::singleShot(0, this, [this, status] {
    d->sdpQueryFinished = status == kIOReturnSuccess;
    @autoreleasepool {
      if (d->device != nil) {
        LOG_DEBUG(
            "bluetooth/rfcomm: SDP cache updated at %s", [[[d->device getLastServicesUpdate] description] UTF8String]
        );
      }
    }
    if (!d->running || d->connected || !d->connecting) {
      return;
    }
    d->discoverAndOpen();
  });
}

void BtRfcommLink::handlePeerData()
{
  // Runs on an IOBluetooth thread: only clear the flag here, the timer has to
  // be stopped on the event loop thread.
  d->awaitingPeer = false;
  QTimer::singleShot(0, this, [this] {
    if (d->peerDataTimer != nullptr) {
      d->peerDataTimer->stop();
    }
  });
}

void BtRfcommLink::handleChannelClosed()
{
  if (!d->running || !d->connected) {
    return;
  }
  // Drop the dead channel and clear the state so a later startClient() (the
  // relay schedules one on disconnect) can dial the peer again; leaving
  // `connected` set would make connectPeer() return early forever.
  d->connected = false;
  d->connecting = false;
  d->channelOpening = false;
  d->awaitingPeer = false;
  d->channel = nil;
  d->device = nil;
  LOG_INFO("bluetooth/rfcomm: the data channel closed");
  Q_EMIT disconnected(QStringLiteral("the Bluetooth data channel closed"));
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

QVector<int> BtRfcommLink::Private::deskflowChannels()
{
  QVector<int> channels;
  if (device == nil) {
    return channels;
  }

  @autoreleasepool {
    // The peer can advertise more than one Deskflow record: Windows keeps a
    // record of an earlier run alive, and it points at the channel that run was
    // bound to.  Collect every channel instead of trusting one record, so a
    // stale entry can be skipped when it does not answer.
    struct Found
    {
      BluetoothSDPServiceRecordHandle handle;
      int channel;
    };
    QVector<Found> records;

    for (IOBluetoothSDPServiceRecord *candidate in [device services]) {
      BluetoothSDPServiceRecordHandle candidateHandle = 0;
      BluetoothRFCOMMChannelID candidateChannel = 0;
      [candidate getServiceRecordHandle:&candidateHandle];
      const IOReturn rc = [candidate getRFCOMMChannelID:&candidateChannel];
      NSString *candidateName = [candidate getServiceName];
      LOG_DEBUG(
          "bluetooth/rfcomm: SDP service \"%s\" handle=%u rfcomm=%d (0x%x)",
          candidateName != nil ? [candidateName UTF8String] : "", static_cast<unsigned>(candidateHandle),
          static_cast<int>(candidateChannel), static_cast<unsigned>(rc)
      );
      if (![candidateName isEqualToString:@"Deskflow"] || rc != kIOReturnSuccess || candidateChannel == 0) {
        continue;
      }
      records.append({candidateHandle, static_cast<int>(candidateChannel)});
    }

    // The SDP server hands out increasing handles, so the highest one is the
    // record the peer registered last - try it first, then the older ones.
    std::sort(records.begin(), records.end(), [](const Found &a, const Found &b) { return a.handle > b.handle; });
    for (const auto &record : records) {
      if (!channels.contains(record.channel)) {
        channels.append(record.channel);
      }
    }

    if (channels.isEmpty()) {
      // A record published by an older build may carry no name.
      IOBluetoothSDPUUID *uuid =
          [IOBluetoothSDPUUID uuidWithBytes:kDeskflowServiceUuidBytes length:sizeof(kDeskflowServiceUuidBytes)];
      IOBluetoothSDPServiceRecord *record = [device getServiceRecordForUUID:uuid];
      BluetoothRFCOMMChannelID channelId = 0;
      if (record != nil && [record getRFCOMMChannelID:&channelId] == kIOReturnSuccess && channelId != 0) {
        channels.append(static_cast<int>(channelId));
      }
    }
  }

  return channels;
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
  sdpQueryFinished = false;
  channelOpening = false;
  candidates.clear();
  candidateIndex = -1;

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
  if (!running || connected || device == nil || channelOpening) {
    return;
  }

  @autoreleasepool {
    if (delegate == nil) {
      delegate = [[DeskflowRfcommDelegate alloc] init];
      delegate.owner = q;
    }

    if (sdpAttempt == 0) {
      // The SDP cache still holds the records of the previous run, which is how a
      // stale channel gets picked (Windows advertised channel 3 while the cache
      // said 1). Query again and wait for the callback before trusting the cache.
      sdpQueryFinished = false;
      [device performSDPQuery:(id)delegate];
    }

    if (!sdpQueryFinished && ++sdpAttempt < kSdpAttempts) {
      LOG_DEBUG("bluetooth/rfcomm: waiting for the SDP query to finish (attempt %d)", sdpAttempt);
      QTimer::singleShot(kSdpRetryMs, q, [this] { discoverAndOpen(); });
      return;
    }
    if (!sdpQueryFinished) {
      LOG_WARN("bluetooth/rfcomm: the SDP query did not finish, falling back to the cached records");
    }

    candidates = deskflowChannels();
    if (candidates.isEmpty() && ++sdpAttempt < kSdpAttempts) {
      LOG_DEBUG("bluetooth/rfcomm: the Deskflow service is not in the SDP cache yet (attempt %d)", sdpAttempt);
      QTimer::singleShot(kSdpRetryMs, q, [this] { discoverAndOpen(); });
      return;
    }

    if (candidates.isEmpty()) {
      connecting = false;
      LOG_WARN("bluetooth/rfcomm: \"%s\" publishes no Deskflow service", qPrintable(peerLabel));
      onAttemptFailed(
          QStringLiteral("The other computer does not publish a Deskflow Bluetooth service. Start Deskflow there "
                         "with the Bluetooth transport enabled.")
      );
      return;
    }

    QStringList channels;
    for (const int candidate : candidates) {
      channels << QString::number(candidate);
    }
    LOG_INFO(
        "bluetooth/rfcomm: \"%s\" advertises %d Deskflow RFCOMM channel(s): %s", qPrintable(peerLabel),
        static_cast<int>(candidates.size()), qPrintable(channels.join(QLatin1Char(',')))
    );

    // Try every advertised channel in turn: one of them can belong to a stale
    // SDP record of an earlier run, which accepts nothing.
    candidateIndex = 0;
    openChannel(candidates.at(candidateIndex));
  }
}

void BtRfcommLink::Private::openChannel(BluetoothRFCOMMChannelID channelId)
{
  if (!running || connected || device == nil) {
    return;
  }

  channelOpening = true;
  pendingChannel = static_cast<int>(channelId);
  @autoreleasepool {
    if (delegate == nil) {
      delegate = [[DeskflowRfcommDelegate alloc] init];
      delegate.owner = q;
    }

    IOBluetoothRFCOMMChannel *opened = nil;
    const IOReturn rc = [device openRFCOMMChannelAsync:&opened withChannelID:channelId delegate:(id)delegate];
    if (rc != kIOReturnSuccess) {
      LOG_WARN("bluetooth/rfcomm: cannot start opening channel %d (0x%x)", static_cast<int>(channelId), rc);
      openNextCandidate(QStringLiteral("channel %1 could not be opened (0x%2)")
                            .arg(static_cast<int>(channelId))
                            .arg(static_cast<unsigned>(rc), 0, 16));
      return;
    }

    if (openTimer != nullptr) {
      openTimer->start(kChannelOpenTimeoutMs);
    }
  }
}

void BtRfcommLink::Private::openNextCandidate(const QString &reason)
{
  channelOpening = false;
  if (openTimer != nullptr) {
    openTimer->stop();
  }

  if (channel != nil) {
    @autoreleasepool {
      [channel closeChannel];
    }
    channel = nil;
  }

  ++candidateIndex;
  if (!running || candidateIndex < 0 || candidateIndex >= static_cast<int>(candidates.size())) {
    const int tried = static_cast<int>(candidates.size());
    candidates.clear();
    candidateIndex = -1;
    connecting = false;
    LOG_WARN("bluetooth/rfcomm: none of the %d advertised Deskflow channel(s) answered (last: %s)", tried, qPrintable(reason));
    onAttemptFailed(
        QStringLiteral("None of the Bluetooth serial channels advertised by the other computer accepted the "
                       "connection. Restart Deskflow there so it re-registers its Bluetooth service.")
    );
    return;
  }

  const int next = candidates.at(candidateIndex);
  LOG_INFO(
      "bluetooth/rfcomm: %s, trying the next advertised channel %d (%d of %d)", qPrintable(reason), next,
      candidateIndex + 1, static_cast<int>(candidates.size())
  );
  openChannel(static_cast<BluetoothRFCOMMChannelID>(next));
}

void BtRfcommLink::Private::onPeerSilent()
{
  if (!running || !connected || !awaitingPeer) {
    return;
  }
  awaitingPeer = false;

  int channelId = -1;
  @autoreleasepool {
    if (channel != nil) {
      channelId = pendingChannel;
    }
  }
  LOG_WARN(
      "bluetooth/rfcomm: channel %d on \"%s\" accepted the connection but sent nothing within %d ms; it belongs to a "
      "stale SDP record, trying the next channel",
      channelId, qPrintable(peerLabel), kPeerDataTimeoutMs
  );

  // The channel is dead, but the peer is not: keep the attempt alive and move on
  // to the next advertised channel instead of failing the whole connection.
  connected = false;
  connecting = true;
  @autoreleasepool {
    if (channel != nil) {
      [channel closeChannel];
    }
  }
  channel = nil;
  Q_EMIT q->disconnected(QStringLiteral("no data on the advertised Bluetooth channel"));
  openNextCandidate(QStringLiteral("channel %1 stayed silent").arg(channelId));
}

void BtRfcommLink::Private::onChannelOpenFinished(int status, void *openedChannel)
{
  channelOpening = false;
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
    LOG_INFO(
        "bluetooth/rfcomm: opened channel %d of \"%s\" (MTU %u), verifying it with the peer's first bytes", pendingChannel,
        qPrintable(peerLabel), [channel getMTU]
    );
    connecting = false;
    awaitingPeer = true;
    markConnected();
    return;
  }

  LOG_WARN("bluetooth/rfcomm: the channel open failed (0x%x)", static_cast<unsigned>(status));
  openNextCandidate(
      QStringLiteral("the channel open was refused (0x%1)").arg(static_cast<unsigned>(status), 0, 16)
  );
}

void BtRfcommLink::Private::markConnected()
{
  if (connected) {
    return;
  }
  connected = true;
  connecting = false;
  // Report the link from the event loop, never from inside IOBluetooth's
  // rfcommChannelOpenComplete callback: the relay writes the protocol hello
  // synchronously on connected(), and IOBluetooth rejects that write with
  // kIOReturnNotOpen (0xe00002cd) while the callback is still on the stack.
  // The timers are touched here too, again because the callback runs on an
  // IOBluetooth thread.
  QTimer::singleShot(0, q, [this] {
    if (retryTimer != nullptr) {
      retryTimer->stop();
    }
    if (!connected) {
      return;
    }
    if (awaitingPeer && peerDataTimer != nullptr) {
      // A channel that opens but never carries a byte belongs to a stale SDP
      // record; the timer is what catches that.
      peerDataTimer->start(kPeerDataTimeoutMs);
    }
    LOG_DEBUG("bluetooth/rfcomm: channel ready, reporting the link to the relay (mtu %u)", [channel getMTU]);
    Q_EMIT q->connected();
    Q_EMIT q->writable();
  });
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
