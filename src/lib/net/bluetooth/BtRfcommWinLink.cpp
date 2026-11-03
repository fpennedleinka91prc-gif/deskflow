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

class BtRfcommWinLink::Private
{
public:
  BtRfcommWinLink *q = nullptr;
  Mode mode = Mode::Client;
  Config config;

  SOCKET listenSocket = INVALID_SOCKET;
  SOCKET dataSocket = INVALID_SOCKET;
  CSADDR_INFO serviceInfo{};
  SOCKADDR_BTH serviceAddress{};

  bool running = false;
  bool connected = false;
  bool published = false;
  int publishedChannel = 0;
  QString peerLabel;

  QTimer *retryTimer = nullptr;
  QTimer *acceptTimer = nullptr;
  QTimer *readTimer = nullptr;
  QTimer *statusTimer = nullptr;

  bool publishService();
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
      d->acceptLoop();
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

  // While waiting for the other computer, repeat which channel is really bound.
  // If that ever differs from the channel the SDP record advertises, the peer
  // connects to a channel nobody listens on, which fails silently on its side.
  d->statusTimer = new QTimer(this);
  d->statusTimer->setInterval(10000);
  connect(d->statusTimer, &QTimer::timeout, this, [this] {
    if (!d->running || d->connected || d->listenSocket == INVALID_SOCKET) {
      return;
    }

    SOCKADDR_BTH live{};
    int length = sizeof(live);
    int liveChannel = -1;
    if (::getsockname(d->listenSocket, reinterpret_cast<SOCKADDR *>(&live), &length) != SOCKET_ERROR) {
      liveChannel = static_cast<int>(live.port);
    }

    LOG_INFO(
        "bluetooth/rfcomm: still waiting for the paired computer (bound channel %d, advertised channel %d)%s",
        liveChannel, d->publishedChannel,
        (liveChannel > 0 && d->publishedChannel > 0 && liveChannel != d->publishedChannel)
            ? " - MISMATCH: the SDP record points at another channel"
            : ""
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

bool BtRfcommWinLink::Private::publishService()
{
  LOG_INFO("bluetooth/rfcomm: creating RFCOMM socket (AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)...");
  SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
  if (socket == INVALID_SOCKET) {
    const int err = WSAGetLastError();
    LOG_ERR("bluetooth/rfcomm: cannot create an RFCOMM socket (error 0x%x - %d)", err, err);
    return false;
  }
  LOG_INFO("bluetooth/rfcomm: RFCOMM socket created successfully");

  SOCKADDR_BTH local{};
  local.addressFamily = AF_BTH;
  local.btAddr = 0; // any local adapter
  local.port = BT_PORT_ANY;

  LOG_INFO("bluetooth/rfcomm: binding RFCOMM socket to local AF_BTH (BT_PORT_ANY)...");
  if (::bind(socket, reinterpret_cast<SOCKADDR *>(&local), sizeof(local)) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    LOG_ERR("bluetooth/rfcomm: bind failed (error 0x%x - %d)", err, err);
    ::closesocket(socket);
    return false;
  }
  LOG_INFO("bluetooth/rfcomm: RFCOMM socket bind succeeded");

  // Ask the Bluetooth stack to accept incoming RFCOMM connections.
  LOG_INFO("bluetooth/rfcomm: listening with backlog %d...", kListenBacklog);
  if (::listen(socket, kListenBacklog) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    LOG_ERR("bluetooth/rfcomm: listen failed (error 0x%x - %d)", err, err);
    ::closesocket(socket);
    return false;
  }
  LOG_INFO("bluetooth/rfcomm: RFCOMM listen succeeded");
  listenSocket = socket;

  // Publish the service so the peer's SDP query finds it.
  SOCKADDR_BTH bound{};
  int length = sizeof(bound);
  if (::getsockname(socket, reinterpret_cast<SOCKADDR *>(&bound), &length) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    LOG_WARN("bluetooth/rfcomm: getsockname failed (error 0x%x - %d)", err, err);
  } else {
    LOG_INFO("bluetooth/rfcomm: assigned RFCOMM channel is %d", static_cast<int>(bound.port));
  }

  CSADDR_INFO info{};
  info.LocalAddr.iSockaddrLength = sizeof(bound);
  info.LocalAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&bound);
  info.RemoteAddr.iSockaddrLength = sizeof(bound);
  info.RemoteAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&bound);
  info.iSocketType = SOCK_STREAM;
  info.iProtocol = BTHPROTO_RFCOMM;
  serviceInfo = info;

  // Description and service name shown to the other computer.
  WSAQUERYSET query{};
  query.dwNameSpace = NS_BTH;
  query.dwSize = sizeof(query);
  query.lpszServiceInstanceName = const_cast<LPWSTR>(L"Deskflow");
  query.lpszComment = const_cast<LPWSTR>(L"Deskflow keyboard and mouse sharing");
  query.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
  query.dwNumberOfCsAddrs = 1;
  query.lpcsaBuffer = &serviceInfo;

  // A record left behind by an earlier run keeps its old channel and shadows the
  // live socket, so clear it before publishing the new one.
  WSAQUERYSET stale{};
  stale.dwNameSpace = NS_BTH;
  stale.dwSize = sizeof(stale);
  stale.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
  if (::WSASetService(&stale, RNRSERVICE_DELETE, 0) != SOCKET_ERROR) {
    LOG_INFO("bluetooth/rfcomm: removed a previously registered Deskflow SDP record");
  }

  LOG_INFO("bluetooth/rfcomm: registering Deskflow service via WSASetService (SDP)...");
  if (::WSASetService(&query, RNRSERVICE_REGISTER, 0) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    LOG_WARN("bluetooth/rfcomm: WSASetService failed (error 0x%x - %d). The socket will listen on channel %d, but SDP clients may not discover it automatically.",
             err, err, static_cast<int>(bound.port));
    // Not fatal: the socket still listens, the peer just will not see it in SDP.
  } else {
    published = true;
    publishedChannel = static_cast<int>(bound.port);
    LOG_INFO("bluetooth/rfcomm: SDP service successfully registered for Deskflow (GUID {3f2a7c1e-9b64-4d58-8a11-6c0d5e7b9f20}) on RFCOMM channel %d",
             static_cast<int>(bound.port));
  }

  u_long nonBlocking = 1;
  ::ioctlsocket(listenSocket, FIONBIO, &nonBlocking);
  LOG_INFO(
      "bluetooth/rfcomm: listening on RFCOMM channel %d, waiting for incoming connection from paired computer",
      static_cast<int>(bound.port)
  );
  if (statusTimer != nullptr) {
    statusTimer->start();
  }
  return true;
}

void BtRfcommWinLink::Private::acceptLoop()
{
  if (!running || connected || listenSocket == INVALID_SOCKET) {
    return;
  }

  SOCKADDR_BTH clientAddr{};
  int clientAddrLen = sizeof(clientAddr);
  SOCKET client = ::accept(listenSocket, reinterpret_cast<SOCKADDR *>(&clientAddr), &clientAddrLen);
  if (client == INVALID_SOCKET) {
    const int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) {
      LOG_WARN("bluetooth/rfcomm: accept error (0x%x - %d)", err, err);
    }
    return;
  }

  const BTH_ADDR addr = clientAddr.btAddr;
  LOG_INFO(
      "bluetooth/rfcomm: accepted connection from Bluetooth peer %02llx:%02llx:%02llx:%02llx:%02llx:%02llx on channel %d",
      (addr >> 40) & 0xff, (addr >> 32) & 0xff, (addr >> 24) & 0xff,
      (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff,
      static_cast<int>(clientAddr.port)
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
}

void BtRfcommWinLink::Private::readLoop()
{
  if (!connected || dataSocket == INVALID_SOCKET) {
    return;
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
  if (listenSocket != INVALID_SOCKET) {
    LOG_INFO("bluetooth/rfcomm: closing listen socket");
    ::closesocket(listenSocket);
    listenSocket = INVALID_SOCKET;
  }
  if (published) {
    LOG_INFO("bluetooth/rfcomm: unregistering SDP service");
    WSAQUERYSET query{};
    query.dwNameSpace = NS_BTH;
    query.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
    ::WSASetService(&query, RNRSERVICE_DELETE, 0);
    published = false;
  }
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
    LOG_WARN("bluetooth/rfcomm: send failed (error 0x%x - %d)", err, err);
    return false;
  }

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
