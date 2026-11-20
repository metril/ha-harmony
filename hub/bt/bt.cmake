# Bluetooth HID helper: vendored BTstack (only what is compiled) + the helper sources.
set(_bts ${CMAKE_SOURCE_DIR}/vendor/btstack)
set(_bts_src
  src/btstack_linked_list.c src/btstack_memory.c src/btstack_memory_pool.c src/btstack_run_loop.c
  src/ad_parser.c src/btstack_hid_parser.c src/btstack_run_loop_base.c src/btstack_tlv.c src/btstack_util.c src/hci.c src/hci_cmd.c src/hci_dump.c
  src/hci_event.c src/hci_event_builder.c src/l2cap.c src/l2cap_signaling.c
  src/classic/sdp_server.c src/classic/sdp_util.c src/classic/device_id_server.c src/classic/hid_device.c
  src/classic/btstack_link_key_db_tlv.c
  platform/posix/btstack_run_loop_posix.c platform/posix/btstack_tlv_posix.c)
if(HH_BT_LE)
  list(APPEND _bts_src
    src/btstack_crypto.c 3rd-party/rijndael/rijndael.c 3rd-party/micro-ecc/uECC.c
    src/ble/att_db.c src/ble/att_dispatch.c src/ble/att_server.c src/ble/gatt_client.c
    src/ble/le_device_db_tlv.c src/ble/sm.c
    src/ble/gatt-service/hids_device.c src/ble/gatt-service/hids_host.c
    src/ble/gatt-service/battery_service_server.c src/ble/gatt-service/device_information_service_server.c)
endif()
list(TRANSFORM _bts_src PREPEND ${_bts}/)
add_library(btstack STATIC ${_bts_src})
target_include_directories(btstack SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/hub/bt
  ${_bts}/src ${_bts}/src/classic ${_bts}/src/ble ${_bts}/src/ble/gatt-service
  ${_bts}/platform/posix ${_bts}/3rd-party/rijndael ${_bts}/3rd-party/micro-ecc)
target_compile_definitions(btstack PRIVATE _GNU_SOURCE)
target_compile_options(btstack PRIVATE -w -Wno-error -ffunction-sections -fdata-sections)  # third-party, patched copy
if(HH_BT_LE)
  target_compile_definitions(btstack PUBLIC HH_BT_LE=1)
endif()

add_library(hp_bt STATIC
  hub/bt/bt_main.c hub/bt/bt_bonds.c hub/bt/bt_state.c hub/bt/bt_pairing.c hub/bt/bt_remote.c hub/bt/bt_hid.c hub/bt/bt_hid_queue.c
  hub/bt/bt_reconnect_queue.c hub/bt/hci_transport_linux.c hub/bt/bt_leaddr.c hub/bt/bt_trace.c hub/bt/bt_sc.c)
target_include_directories(hp_bt PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_definitions(hp_bt PRIVATE _GNU_SOURCE)
target_compile_options(hp_bt PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(hp_bt PUBLIC btstack)
