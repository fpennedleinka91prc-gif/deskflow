/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtRfcommWinLink.h"

#include "base/Log.h"

#ifdef _WIN32

#include <QTimer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstring>

#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>

namespace deskflow::net::bt {

namespace {

//! The service GUID identifies the Deskflow serial port to SDP clients.
constexpr GUID kServiceGuid = {0x3f2a7c1e, 0x9b64, 0x4d58, {0x8a, 0x11, 0x6c, 0x0d, 0x5e, 0x7b, 0x9f, 0x20}};

//! Chunk size for RFCOMM payloads.
constexpr int kMaxChunk = 512;

//! Backlog for the listening socket.
constexpr int kListenBacklog = 2;

bool winsockReady()
{
  static bool ready = false;
  static bool attempted = false;
  if (attempted) {
    return ready;
  }
  attempted = true;

  WSADATA data{};
  const int rc = WSAStartup(MAKEWORD(2, 2), &data);
  ready = (rc == 0);
  if (!ready) {
    LOG_ERR("bluetooth/rfcomm: WSAStartup failed (rc=%d, error=0x%x)", rc, WSAGetLastError());
  } else {
    LOG_INFO("bluetooth/rfcomm: Winsock 2.2 initialized (version 0x%x, description=\"%s\")",
             data.wVersion, data.szDescription);
  }
  return ready;
}

void logBluetoothRadiosAndDevices()
{
  BLUETOOTH_FIND_RADIO_PARAMS radioParams{sizeof(BLUETOOTH_FIND_RADIO_PARAMS)};
  HANDLE hRadio = nullptr;
  HBLUETOOTH_RADIO_FIND hFindRadio = BluetoothFindFirstRadio(&radioParams, &hRadio);
  if (hFindRadio == nullptr) {
    const DWORD err = GetLastError();
    LOG_WARN("bluetooth/rfcomm: no local Bluetooth radio found (error 0x%lx / %lu). Ensure Bluetooth adapter is plugged in and turned on in Windows settings.", err, err);
  } else {
    int radioCount = 0;
    do {
      radioCount++;
      BLUETOOTH_RADIO_INFO radioInfo{sizeof(BLUETOOTH_RADIO_INFO)};
      if (BluetoothGetRadioInfo(hRadio, &radioInfo) == ERROR_SUCCESS) {
        const BTH_ADDR addr = radioInfo.address.ullLong;
        const bool connectable = BluetoothIsConnectable(hRadio);
        const bool discoverable = BluetoothIsDiscoverable(hRadio);
        const auto radioName = QString::fromWCharArray(radioInfo.szName);
        LOG_INFO("bluetooth/rfcomm: local radio #%d: name=\"%s\", addr=%02llx:%02llx:%02llx:%02llx:%02llx:%02llx, mfr=0x%04x, lmpSub=0x%04x, connectable=%d, discoverable=%d",
                 radioCount,
                 qPrintable(radioName),
                 (addr >> 40) & 0xff, (addr >> 32) & 0xff, (addr >> 24) & 0xff,
                 (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff,
                 radioInfo.manufacturer,
                 radioInfo.lmpSubversion,
                 connectable ? 1 : 0,
                 discoverable ? 1 : 0);
      } else {
        LOG_WARN("bluetooth/rfcomm: BluetoothGetRadioInfo failed for radio #%d (error 0x%lx)", radioCount, GetLastError());
      }
      CloseHandle(hRadio);
      hRadio = nullptr;
    } while (BluetoothFindNextRadio(hFindRadio, &hRadio));
    BluetoothFindRadioClose(hFindRadio);
  }

  BLUETOOTH_DEVICE_SEARCH_PARAMS devSearch{sizeof(BLUETOOTH_DEVICE_SEARCH_PARAMS)};
  devSearch.fReturnAuthenticated = TRUE; // paired
  devSearch.fReturnRemembered = TRUE;
  devSearch.fReturnUnknown = FALSE;
  devSearch.fReturnConnected = TRUE;
  devSearch.hRadio = nullptr;
  devSearch.fIssueInquiry = FALSE;
  BLUETOOTH_DEVICE_INFO devInfo{sizeof(BLUETOOTH_DEVICE_INFO)};
  HBLUETOOTH_DEVICE_FIND hFindDev = BluetoothFindFirstDevice(&devSearch, &devInfo);
  if (hFindDev == nullptr) {
    LOG_INFO("bluetooth/rfcomm: no paired Bluetooth devices found on this system");
  } else {
    int devCount = 0;
    do {
      devCount++;
      const BTH_ADDR addr = devInfo.Address.ullLong;
      const auto devName = QString::fromWCharArray(devInfo.szName);
      LOG_INFO("bluetooth/rfcomm: paired device #%d: name=\"%s\", addr=%02llx:%02llx:%02llx:%02llx:%02llx:%02llx, authenticated=%d, remembered=%d, connected=%d",
               devCount,
               qPrintable(devName),
               (addr >> 40) & 0xff, (addr >> 32) & 0xff, (addr >> 24) & 0xff,
               (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff,
               devInfo.fAuthenticated ? 1 : 0,
               devInfo.fRemembered ? 1 : 0,
               devInfo.fConnected ? 1 : 0);
    } while (BluetoothFindNextDevice(hFindDev, &devInfo));
    BluetoothFindDeviceClose(hFindDev);
  }
}

} // namespace

//! RFCOMM channels Deskflow listens on.
/*!
Windows keeps advertising SDP records left behind by an earlier run, and they
point at the channel that run happened to be bound to.  Such a record cannot be
deleted from another process (the only documented way to delete one is
\c RNRSERVICE_DELETE with the address tuple - protocol plus port - that the
publishing process used, see WSASetService), so instead of listening on one
dynamically assigned channel this link listens on the low static channels as
well.  A stale record then still lands on a live socket and the peer connects,
even when the record itself survives.  Windows hands out dynamic channels
starting at 1, so the low range covers the records earlier builds created.
*/
constexpr ULONG kPreferredChannels[] = {1, 2, 3, 4};

class BtRfcommWinLink::Private
{
public:
  BtRfcommWinLink *q = nullptr;
  Mode mode = Mode::Client;
  Config config;

  //! One listening socket per RFCOMM channel; each advertises its own SDP record.
  struct ListeningService
  {
    SOCKET socket = INVALID_SOCKET;
    SOCKADDR_BTH address{};
    bool advertised = false;
  };
  QVector<ListeningService> services;

  SOCKET dataSocket = INVALID_SOCKET;

  bool running = false;
  bool connected = false;
  //! Set once a low RFCOMM channel was unavailable, to print the cleanup hint.
  bool channelConflictSeen = false;
  QString peerLabel;

  // Set when ::send() reported WSAEWOULDBLOCK; readLoop() then keeps offering
  // writable() so the relay retries once the Bluetooth stack has drained.
  bool sendBlocked = false;

  QTimer *retryTimer = nullptr;
  QTimer *acceptTimer = nullptr;
  QTimer *readTimer = nullptr;
  QTimer *statusTimer = nullptr;

  bool publishService();
  bool bindChannel(ULONG channel, ListeningService &service, int &errorCode);
  bool advertiseService(ListeningService &service);
  bool removeServiceRecord(const SOCKADDR_BTH &address, bool verbose);
  void removeAllServiceRecords(bool verbose);
  void acceptLoop();
  void readLoop();
  void connectToServer();
  void markConnected();
  void markDisconnected(const QString &reason);
  void cleanup();
  BTH_ADDR parseAddress(const QString &text) const;
};

BtRfcommWinLink::BtRfcommWinLink(QObject *parent) : IBtLink(parent), d(new Private)
{
  d->q = this;
  d->retryTimer = new QTimer(this);
  d->retryTimer->setSingleShot(true);
  connect(d->retryTimer, &QTimer::timeout, this, [this] {
    if (!d->running || d->connected) {
      return;
    }
    if (d->mode == Mode::Server) {
      // A disconnect tears down the listening sockets (see cleanup()), so on
      // retry we must rebuild them from scratch: re-create the sockets, re-bind,
      // re-listen and re-register the SDP records.  Calling acceptLoop() alone
      // would return immediately because the service list is empty.
      if (!d->publishService()) {
        LOG_ERR("bluetooth/rfcomm: re-publishService failed on retry");
        Q_EMIT errorOccurred(QStringLiteral("Could not re-open the Bluetooth serial port on this computer"));
        return;
      }
      if (d->acceptTimer != nullptr) {
        d->acceptTimer->start();
      }
      if (d->statusTimer != nullptr) {
        d->statusTimer->start();
      }
    } else {
      d->connectToServer();
    }
  });

  d->acceptTimer = new QTimer(this);
  d->acceptTimer->setInterval(500);
  connect(d->acceptTimer, &QTimer::timeout, this, [this] { d->acceptLoop(); });

  d->readTimer = new QTimer(this);
  d->readTimer->setInterval(10);
  connect(d->readTimer, &QTimer::timeout, this, [this] { d->readLoop(); });

  // While waiting for the other computer, repeat which channels are really
  // bound and which ones the SDP records advertise.  If a record advertises a
  // different channel than the socket listening for it, the peer connects to a
  // channel nobody listens on, which fails silently on its side.
  d->statusTimer = new QTimer(this);
  d->statusTimer->setInterval(10000);
  connect(d->statusTimer, &QTimer::timeout, this, [this] {
    if (!d->running || d->connected || d->services.isEmpty()) {
      return;
    }

    QStringList bound;
    QStringList advertised;
    QStringList mismatched;

    for (const auto &service : d->services) {
      SOCKADDR_BTH live{};
      int length = sizeof(live);
      int liveChannel = -1;
      if (::getsockname(service.socket, reinterpret_cast<SOCKADDR *>(&live), &length) != SOCKET_ERROR) {
        liveChannel = static_cast<int>(live.port);
      }
      bound << QString::number(liveChannel);
      if (!service.advertised) {
        continue;
      }
      const int advertisedChannel = static_cast<int>(service.address.port);
      advertised << QString::number(advertisedChannel);
      if (liveChannel > 0 && liveChannel != advertisedChannel) {
        mismatched << QStringLiteral("%1 (advertised) != %2 (bound)")
                          .arg(advertisedChannel)
                          .arg(liveChannel);
      }
    }

    LOG_INFO(
        "bluetooth/rfcomm: still waiting for the paired computer (bound channel(s) %s, advertised channel(s) %s)%s",
        qPrintable(bound.join(QLatin1Char(','))), qPrintable(advertised.join(QLatin1Char(','))),
        mismatched.isEmpty()
            ? ""
            : qPrintable(QStringLiteral(" - MISMATCH: %1; the SDP record points at another channel")
                              .arg(mismatched.join(QLatin1Char(';'))))
    );
  });
}

BtRfcommWinLink::~BtRfcommWinLink()
{
  stop();
  delete d;
}

BTH_ADDR BtRfcommWinLink::Private::parseAddress(const QString &text) const
{
  if (text.isEmpty()) {
    return 0;
  }
  // Accept both "00:11:22:33:44:55" and "001122334455" forms.
  QString digits = text;
  digits.remove(QLatin1Char(':'));
  digits.remove(QLatin1Char('-'));
  bool ok = false;
  const auto value = digits.toULongLong(&ok, 16);
  return ok ? static_cast<BTH_ADDR>(value) : 0;
}

bool BtRfcommWinLink::Private::bindChannel(ULONG channel, ListeningService &service, int &errorCode)
{
  SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
  if (socket == INVALID_SOCKET) {
    errorCode = WSAGetLastError();
    LOG_WARN("bluetooth/rfcomm: cannot create an RFCOMM socket (error 0x%x - %d)", errorCode, errorCode);
    return false;
  }

  SOCKADDR_BTH local{};
  local.addressFamily = AF_BTH;
  local.btAddr = 0; // any local adapter
  local.port = channel;

  if (::bind(socket, reinterpret_cast<SOCKADDR *>(&local), sizeof(local)) == SOCKET_ERROR) {
    errorCode = WSAGetLastError();
    ::closesocket(socket);
    return false;
  }

  if (::listen(socket, kListenBacklog) == SOCKET_ERROR) {
    errorCode = WSAGetLastError();
    ::closesocket(socket);
    return false;
  }

  SOCKADDR_BTH bound{};
  int length = sizeof(bound);
  if (::getsockname(socket, reinterpret_cast<SOCKADDR *>(&bound), &length) == SOCKET_ERROR) {
    errorCode = WSAGetLastError();
    ::closesocket(socket);
    return false;
  }

  u_long nonBlocking = 1;
  ::ioctlsocket(socket, FIONBIO, &nonBlocking);

  service.socket = socket;
  service.address = bound;
  service.advertised = false;
  LOG_INFO(
      "bluetooth/rfcomm: listening on RFCOMM channel %d (requested %lu)", static_cast<int>(bound.port), channel
  );
  return true;
}

bool BtRfcommWinLink::Private::removeServiceRecord(const SOCKADDR_BTH &address, bool verbose)
{
  SOCKADDR_BTH target = address;
  CSADDR_INFO info{};
  info.LocalAddr.iSockaddrLength = sizeof(target);
  info.LocalAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&target);
  info.RemoteAddr.iSockaddrLength = sizeof(target);
  info.RemoteAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&target);
  info.iSocketType = SOCK_STREAM;
  info.iProtocol = BTHPROTO_RFCOMM;

  // RNRSERVICE_DELETE indexes the record by the address tuple it was
  // registered with (protocol and port, see "Bluetooth and WSAQUERYSET for
  // Set Service"), so these members must match the registration call.  A query
  // carrying only the service GUID is rejected with WSAEINVAL (10022).
  // A record published by another process cannot be removed here at all; it
  // disappears when that process exits.
  WSAQUERYSET query{};
  query.dwSize = sizeof(query);
  query.dwNameSpace = NS_BTH;
  query.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
  query.lpszServiceInstanceName = const_cast<LPWSTR>(L"Deskflow");
  query.lpszComment = const_cast<LPWSTR>(L"Deskflow keyboard and mouse sharing");
  query.dwNumberOfCsAddrs = 1;
  query.lpcsaBuffer = &info;

  if (::WSASetService(&query, RNRSERVICE_DELETE, 0) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    if (verbose) {
      LOG_WARN(
          "bluetooth/rfcomm: could not remove the Deskflow SDP record for RFCOMM channel %d (error 0x%x - %d)",
          static_cast<int>(address.port), err, err
      );
    } else {
      LOG_DEBUG(
          "bluetooth/rfcomm: no SDP record for RFCOMM channel %d is registered by this process (error 0x%x - %d)",
          static_cast<int>(address.port), err, err
      );
    }
    return false;
  }
  LOG_INFO("bluetooth/rfcomm: removed the Deskflow SDP record for RFCOMM channel %d", static_cast<int>(address.port));
  return true;
}

void BtRfcommWinLink::Private::removeAllServiceRecords(bool verbose)
{
  for (const auto &service : services) {
    if (service.advertised) {
      removeServiceRecord(service.address, verbose);
    }
  }
}

bool BtRfcommWinLink::Private::advertiseService(ListeningService &service)
{
  CSADDR_INFO info{};
  info.LocalAddr.iSockaddrLength = sizeof(service.address);
  info.LocalAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&service.address);
  info.RemoteAddr.iSockaddrLength = sizeof(service.address);
  info.RemoteAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&service.address);
  info.iSocketType = SOCK_STREAM;
  info.iProtocol = BTHPROTO_RFCOMM;

  // Description and service name shown to the other computer.
  WSAQUERYSET query{};
  query.dwNameSpace = NS_BTH;
  query.dwSize = sizeof(query);
  query.lpszServiceInstanceName = const_cast<LPWSTR>(L"Deskflow");
  query.lpszComment = const_cast<LPWSTR>(L"Deskflow keyboard and mouse sharing");
  query.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
  query.dwNumberOfCsAddrs = 1;
  query.lpcsaBuffer = &info;

  // The same address tuple may only be registered once, so release a record
  // this process left on this channel before publishing again.
  removeServiceRecord(service.address, false);

  if (::WSASetService(&query, RNRSERVICE_REGISTER, 0) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    LOG_WARN(
        "bluetooth/rfcomm: WSASetService could not register RFCOMM channel %d (error 0x%x - %d). The socket still "
        "listens, but the peer will not discover this channel through SDP.",
        static_cast<int>(service.address.port), err, err
    );
    return false;
  }

  service.advertised = true;
  LOG_INFO(
      "bluetooth/rfcomm: SDP service successfully registered for Deskflow (GUID "
      "{3f2a7c1e-9b64-4d58-8a11-6c0d5e7b9f20}) on RFCOMM channel %d",
      static_cast<int>(service.address.port)
  );
  return true;
}

bool BtRfcommWinLink::Private::publishService()
{
  services.clear();
  channelConflictSeen = false;

  // A record left behind by an earlier run keeps advertising its old channel
  // and cannot be deleted from here (see removeServiceRecord), so listen on the
  // low channels as well: the stale record then still reaches a live socket.
  for (const ULONG channel : kPreferredChannels) {
    ListeningService service;
    int err = 0;
    if (!bindChannel(channel, service, err)) {
      // WSAEADDRINUSE means another local service holds the channel: a Windows
      // Bluetooth profile, or a Deskflow instance that is still running.
      channelConflictSeen = true;
      LOG_INFO("bluetooth/rfcomm: RFCOMM channel %lu is not available (error 0x%x - %d)", channel, err, err);
      continue;
    }
    services.append(service);
  }

  if (services.isEmpty()) {
    // No low channel is free; let the stack pick one.
    ListeningService service;
    int err = 0;
    if (!bindChannel(BT_PORT_ANY, service, err)) {
      LOG_ERR("bluetooth/rfcomm: cannot bind an RFCOMM listening socket (error 0x%x - %d)", err, err);
      return false;
    }
    services.append(service);
  }

  if (channelConflictSeen) {
    LOG_INFO(
        "bluetooth/rfcomm: a low RFCOMM channel was already in use; if the peer keeps discovering a channel "
        "Deskflow no longer listens on, close a leftover deskflow-core.exe (tasklist | findstr deskflow) or restart "
        "the Bluetooth Support Service (net stop bthserv && net start bthserv)."
    );
  }

  int advertisedCount = 0;
  for (auto &service : services) {
    if (advertiseService(service)) {
      ++advertisedCount;
    }
  }
  LOG_INFO(
      "bluetooth/rfcomm: %d of %d listening RFCOMM channel(s) are advertised over SDP, waiting for the paired computer",
      advertisedCount, static_cast<int>(services.size())
  );

  if (statusTimer != nullptr) {
    statusTimer->start();
  }
  return true;
}

void BtRfcommWinLink::Private::acceptLoop()
{
  if (!running || connected || services.isEmpty()) {
    return;
  }

  for (const auto &service : services) {
    SOCKADDR_BTH clientAddr{};
    int clientAddrLen = sizeof(clientAddr);
    SOCKET client = ::accept(service.socket, reinterpret_cast<SOCKADDR *>(&clientAddr), &clientAddrLen);
    if (client == INVALID_SOCKET) {
      const int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK) {
        LOG_WARN("bluetooth/rfcomm: accept error on channel %d (0x%x - %d)", static_cast<int>(service.address.port), err, err);
      }
      continue;
    }

    const BTH_ADDR addr = clientAddr.btAddr;
    LOG_INFO(
        "bluetooth/rfcomm: accepted connection from Bluetooth peer %02llx:%02llx:%02llx:%02llx:%02llx:%02llx on "
        "channel %d",
        (addr >> 40) & 0xff, (addr >> 32) & 0xff, (addr >> 24) & 0xff, (addr >> 16) & 0xff, (addr >> 8) & 0xff,
        addr & 0xff, static_cast<int>(clientAddr.port)
    );

    u_long nonBlocking = 1;
    ::ioctlsocket(client, FIONBIO, &nonBlocking);

    dataSocket = client;
    if (acceptTimer != nullptr) {
      acceptTimer->stop();
    }
    if (statusTimer != nullptr) {
      statusTimer->stop();
    }
    markConnected();
    return;
  }
}

void BtRfcommWinLink::Private::readLoop()
{
  if (!connected || dataSocket == INVALID_SOCKET) {
    return;
  }

  // A previous send filled the RFCOMM buffer; give the relay another chance to
  // flush, because without a socket event there is nothing else that would.
  if (sendBlocked) {
    sendBlocked = false;
    Q_EMIT q->writable();
  }

  char buffer[4096];
  const int received = ::recv(dataSocket, buffer, sizeof(buffer), 0);
  if (received <= 0) {
    if (received == 0) {
      LOG_INFO("bluetooth/rfcomm: remote peer closed the Bluetooth link");
      markDisconnected(QStringLiteral("the other computer closed the Bluetooth link"));
    } else {
      const int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK) {
        LOG_WARN("bluetooth/rfcomm: recv failed (error 0x%x - %d)", err, err);
        markDisconnected(QStringLiteral("the Bluetooth link failed (%1)").arg(err));
      }
    }
    return;
  }

  static quint64 totalReceived = 0;
  totalReceived += received;
  if (totalReceived == static_cast<quint64>(received)) {
    LOG_INFO("bluetooth/rfcomm: first chunk received (%d bytes)", received);
  } else {
    LOG_DEBUG("bluetooth/rfcomm: received %d bytes (total: %llu)", received, totalReceived);
  }

  const QByteArray chunk(buffer, received);
  Q_EMIT q->chunkReceived(chunk);
}

void BtRfcommWinLink::Private::connectToServer()
{
  if (!running || connected) {
    return;
  }

  const BTH_ADDR target = parseAddress(config.peerAddress);
  if (target == 0) {
    LOG_WARN("bluetooth/rfcomm: no peer address configured (got: \"%s\"), expected format AA:BB:CC:DD:EE:FF",
             qPrintable(config.peerAddress));
    if (retryTimer != nullptr) {
      retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
    }
    return;
  }

  LOG_INFO("bluetooth/rfcomm: connecting to peer %02llx:%02llx:%02llx:%02llx:%02llx:%02llx...",
           (target >> 40) & 0xff, (target >> 32) & 0xff, (target >> 24) & 0xff,
           (target >> 16) & 0xff, (target >> 8) & 0xff, target & 0xff);

  SOCKADDR_BTH remote{};
  remote.addressFamily = AF_BTH;
  remote.btAddr = target;
  remote.port = 0; // let SDP resolve the serial port channel
  remote.serviceClassId = kServiceGuid;

  SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
  if (socket == INVALID_SOCKET) {
    const int err = WSAGetLastError();
    LOG_ERR("bluetooth/rfcomm: cannot create an RFCOMM socket (0x%x - %d)", err, err);
    if (retryTimer != nullptr) {
      retryTimer->start(2000);
    }
    return;
  }

  if (::connect(socket, reinterpret_cast<SOCKADDR *>(&remote), sizeof(remote)) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    LOG_WARN("bluetooth/rfcomm: connect to peer failed (0x%x - %d)", err, err);
    ::closesocket(socket);
    if (retryTimer != nullptr) {
      retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
    }
    return;
  }

  LOG_INFO("bluetooth/rfcomm: connect to peer succeeded");
  dataSocket = socket;
  u_long nonBlocking = 1;
  ::ioctlsocket(dataSocket, FIONBIO, &nonBlocking);
  markConnected();
}

void BtRfcommWinLink::Private::markConnected()
{
  if (connected) {
    return;
  }
  connected = true;
  if (retryTimer != nullptr) {
    retryTimer->stop();
  }
  if (readTimer != nullptr) {
    readTimer->start();
  }
  LOG_INFO("bluetooth/rfcomm: the Bluetooth link is UP");
  Q_EMIT q->connected();
  Q_EMIT q->writable();
}

void BtRfcommWinLink::Private::markDisconnected(const QString &reason)
{
  if (!connected) {
    return;
  }
  connected = false;
  if (readTimer != nullptr) {
    readTimer->stop();
  }
  LOG_INFO("bluetooth/rfcomm: the Bluetooth link is DOWN: %s", qPrintable(reason));
  Q_EMIT q->disconnected(reason);
  cleanup();
  if (running && retryTimer != nullptr) {
    LOG_INFO("bluetooth/rfcomm: retry timer scheduled (%d ms)", config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
    retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
  }
}

void BtRfcommWinLink::Private::cleanup()
{
  if (statusTimer != nullptr) {
    statusTimer->stop();
  }
  if (dataSocket != INVALID_SOCKET) {
    LOG_INFO("bluetooth/rfcomm: closing data socket");
    ::closesocket(dataSocket);
    dataSocket = INVALID_SOCKET;
  }
  // Unregister before closing the sockets: the record is indexed by the address
  // the socket was bound to.  A failure here is harmless - Windows drops the
  // records of a process when it exits - so it is logged at debug level.
  removeAllServiceRecords(false);
  for (const auto &service : services) {
    LOG_INFO("bluetooth/rfcomm: closing listen socket on RFCOMM channel %d", static_cast<int>(service.address.port));
    ::closesocket(service.socket);
  }
  services.clear();
}

void BtRfcommWinLink::startServer(const Config &config)
{
  LOG_INFO("bluetooth/rfcomm: startServer requested (localName=\"%s\")", qPrintable(config.localName));
  if (!winsockReady()) {
    LOG_ERR("bluetooth/rfcomm: Winsock is not ready, cannot start server");
    Q_EMIT errorOccurred(QStringLiteral("Winsock could not be started"));
    return;
  }

  logBluetoothRadiosAndDevices();

  d->mode = Mode::Server;
  d->config = config;
  d->running = true;

  if (!d->publishService()) {
    LOG_ERR("bluetooth/rfcomm: publishService failed");
    Q_EMIT errorOccurred(QStringLiteral("Could not open the Bluetooth serial port on this computer"));
    return;
  }

  d->acceptTimer->start();
  LOG_INFO("bluetooth/rfcomm: server accept timer started (500ms interval)");
}

void BtRfcommWinLink::startClient(const Config &config)
{
  LOG_INFO("bluetooth/rfcomm: startClient requested (peerAddress=\"%s\", peerName=\"%s\")",
           qPrintable(config.peerAddress), qPrintable(config.peerName));
  if (!winsockReady()) {
    LOG_ERR("bluetooth/rfcomm: Winsock is not ready, cannot start client");
    Q_EMIT errorOccurred(QStringLiteral("Winsock could not be started"));
    return;
  }

  logBluetoothRadiosAndDevices();

  d->mode = Mode::Client;
  d->config = config;
  d->running = true;
  d->peerLabel = config.peerAddress;
  d->connectToServer();
}

void BtRfcommWinLink::stop()
{
  d->running = false;
  d->connected = false;
  if (d->retryTimer != nullptr) {
    d->retryTimer->stop();
  }
  if (d->acceptTimer != nullptr) {
    d->acceptTimer->stop();
  }
  if (d->readTimer != nullptr) {
    d->readTimer->stop();
  }
  d->cleanup();
}

bool BtRfcommWinLink::isConnected() const
{
  return d->connected;
}

int BtRfcommWinLink::maxChunkSize() const
{
  return kMaxChunk;
}

bool BtRfcommWinLink::sendChunk(const QByteArray &chunk)
{
  if (!d->connected || d->dataSocket == INVALID_SOCKET || chunk.isEmpty()) {
    return false;
  }

  const int sent = ::send(d->dataSocket, chunk.constData(), chunk.size(), 0);
  if (sent <= 0) {
    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      // The RFCOMM buffer is full; readLoop() retries through writable().
      d->sendBlocked = true;
      return false;
    }
    LOG_WARN("bluetooth/rfcomm: send failed (error 0x%x - %d)", err, err);
    return false;
  }
  d->sendBlocked = false;

  static quint64 totalSent = 0;
  totalSent += sent;
  if (totalSent == static_cast<quint64>(sent)) {
    LOG_INFO("bluetooth/rfcomm: first chunk sent (%d bytes)", sent);
  } else {
    LOG_DEBUG("bluetooth/rfcomm: sent %d bytes (total: %llu)", sent, totalSent);
  }

  QTimer::singleShot(0, this, [this] { Q_EMIT writable(); });
  return true;
}

QString BtRfcommWinLink::describe() const
{
  const QString state = d->connected ? QStringLiteral("connected") : QStringLiteral("waiting");
  if (d->mode == Mode::Server) {
    return QStringLiteral("classic Bluetooth server (%1)").arg(state);
  }
  return QStringLiteral("classic Bluetooth -> %1 (%2)")
      .arg(d->peerLabel.isEmpty() ? QStringLiteral("paired computer") : d->peerLabel, state);
}

#else // !_WIN32

namespace deskflow::net::bt {

class BtRfcommWinLink::Private
{
public:
  BtRfcommWinLink *q = nullptr;
};

BtRfcommWinLink::BtRfcommWinLink(QObject *parent) : IBtLink(parent), d(new Private)
{
  d->q = this;
}

BtRfcommWinLink::~BtRfcommWinLink()
{
  delete d;
}

void BtRfcommWinLink::startServer(const Config &config)
{
  Q_UNUSED(config);
  Q_EMIT errorOccurred(QStringLiteral("Classic Bluetooth on Windows is only available in a Windows build"));
}

void BtRfcommWinLink::startClient(const Config &config)
{
  Q_UNUSED(config);
  Q_EMIT errorOccurred(QStringLiteral("Classic Bluetooth on Windows is only available in a Windows build"));
}

void BtRfcommWinLink::stop()
{
}

bool BtRfcommWinLink::isConnected() const
{
  return false;
}

int BtRfcommWinLink::maxChunkSize() const
{
  return 180;
}

bool BtRfcommWinLink::sendChunk(const QByteArray &chunk)
{
  Q_UNUSED(chunk);
  return false;
}

QString BtRfcommWinLink::describe() const
{
  return QStringLiteral("classic Bluetooth (unavailable)");
}

#endif // _WIN32

} // namespace deskflow::net::bt
