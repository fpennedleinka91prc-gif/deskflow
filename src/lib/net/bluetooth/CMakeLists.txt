# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

set(lib_name bluetooth)

set(bluetooth_sources
  BtCodec.cpp
  BtCodec.h
  BtLinkFactory.cpp
  BtLinkFactory.h
  BtLoopbackLink.cpp
  BtLoopbackLink.h
  BtRelay.cpp
  BtRelay.h
  BtTransport.cpp
  BtTransport.h
  BtTypes.h
  IBtLink.h
)

if(DESKFLOW_ENABLE_BLUETOOTH)
  list(APPEND bluetooth_sources
    BtQtLink.cpp
    BtQtLink.h
  )
endif()

add_library(${lib_name} STATIC ${bluetooth_sources})

target_link_libraries(${lib_name}
  PUBLIC
    Qt6::Core
    Qt6::Network
    common
  PRIVATE
    base
    mt
)

if(DESKFLOW_ENABLE_BLUETOOTH)
  target_link_libraries(${lib_name} PUBLIC Qt6::Bluetooth)
  target_compile_definitions(${lib_name} PUBLIC DESKFLOW_BLUETOOTH=1)

  # Qt's macOS port has no bridge for QBluetoothPermission, so the native
  # CoreBluetooth backend is the only usable one there.
  if(APPLE)
    target_sources(${lib_name} PRIVATE
      BtDarwinLink.mm
      BtDarwinLink.h
    )
    set_source_files_properties(BtDarwinLink.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(${lib_name} PUBLIC "-framework CoreBluetooth" "-framework Foundation")
    target_compile_definitions(${lib_name} PUBLIC DESKFLOW_BLUETOOTH_DARWIN=1)
  endif()
endif()
