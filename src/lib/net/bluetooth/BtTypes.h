/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace deskflow::net::bt {

//! Bluetooth LE GATT service used by the Deskflow transport.
/*!
The service exposes two characteristics:

- \c Rx : central (client) -> peripheral (server), Write / WriteWithoutResponse.
- \c Tx : peripheral (server) -> central (client), Notify.

The link is a plain, reliable, ordered byte pipe.  Framing and flow control
are handled above the link by \c BtCodec so that a GATT message can never be
mistaken for a stream boundary.
*/
inline constexpr char kServiceUuid[] = "3f2a7c1e-9b64-4d58-8a11-6c0d5e7b9f20";
inline constexpr char kRxCharacteristicUuid[] = "3f2a7c1e-9b64-4d58-8a11-6c0d5e7b9f21";
inline constexpr char kTxCharacteristicUuid[] = "3f2a7c1e-9b64-4d58-8a11-6c0d5e7b9f22";

//! Which side of the Bluetooth link this process implements.
enum class Mode
{
  Server, //!< Peripheral: advertises and accepts a central (the Deskflow server).
  Client  //!< Central: scans for and connects to a peripheral (the Deskflow client).
};

//! Link configuration shared by every backend.
struct Config
{
  //! Name advertised by the peripheral.  Defaults to the computer name.
  QString localName;

  //! Peer address (macOS: CoreBluetooth peripheral UUID) for the central role.
  QString peerAddress;

  //! Peer name used when \c peerAddress is empty (matched against advertised name).
  QString peerName;

  //! Requested payload bytes per GATT chunk; 0 lets the backend pick from the MTU.
  int chunkSize = 0;

  //! Number of chunks the peer may send before it must wait for credit.
  int initialCredits = 32;

  //! Seconds between connection attempts in the central role.
  int retryIntervalMs = 2000;

  //! Overall connect timeout for the central role.
  int connectTimeoutMs = 30000;
};

} // namespace deskflow::net::bt
