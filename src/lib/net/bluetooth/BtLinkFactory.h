/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

class QObject;

namespace deskflow::net::bt {

class IBtLink;

//! Creates the link backend requested by the environment.
/*!
Backends:

- \c qt (default, when built with Qt Bluetooth): BLE GATT peripheral/central.
- \c loopback: an in-process/TCP stand-in that keeps the chunking and credit
  behaviour of a real link without any radio.  Used by the end to end tests
  (\c DESKFLOW_BT_BACKEND=loopback, \c DESKFLOW_BT_LOOPBACK_PORT=<port>).

Selection order: \c DESKFLOW_BT_BACKEND environment variable, then settings
key \c bluetooth/backend, then \c auto.
*/
class BtLinkFactory
{
public:
  //! Create a link; returns nullptr when no usable backend was compiled in.
  static IBtLink *create(QObject *parent = nullptr);

  //! Name of the backend that \c create() would use, for logging.
  static QString backendName();

  //! True when this build has a real Bluetooth backend.
  static bool hasBluetoothBackend();
};

} // namespace deskflow::net::bt
