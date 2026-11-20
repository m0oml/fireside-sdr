add_executable(fireside_tests main.cpp test_protocol.cpp test_link_safety.cpp test_eeprom_startup.cpp test_pal.cpp test_startup.cpp test_dsp.cpp test_cat.cpp test_rigctl.cpp test_nr2.cpp test_display.cpp test_capture.cpp test_tune.cpp test_cli.cpp test_engine.cpp test_session.cpp test_resample.cpp test_decode_cw.cpp test_decode_psk.cpp test_decode_rtty.cpp test_decode_ft8.cpp test_playback.cpp test_logbook.cpp test_bandplan.cpp test_tx_ssb.cpp test_tx_roundtrip.cpp test_tx_mic.cpp test_tx_cw.cpp test_tx_imd.cpp test_tx_slots.cpp test_tx_render.cpp test_preamp.cpp test_fw_iso.cpp test_ft8_osd.cpp test_sam.cpp test_dxcc.cpp test_rxant.cpp test_recorder.cpp test_if_offset.cpp test_atu_tx.cpp test_qrz.cpp test_pskr.cpp test_qso_seq.cpp test_fw_iso_tx.cpp test_tx_voice.cpp test_vectors.cpp)
target_link_libraries(fireside_tests PRIVATE fireside_core util pthread)
target_compile_definitions(fireside_tests PRIVATE FIRESIDETX_PATH="$<TARGET_FILE:firesidetx>" FIRESIDE_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/vectors" FIRESIDE_FT8_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ft8" FIRESIDE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(fireside_tests PRIVATE -Wall -Wextra)
add_test(NAME fireside_core COMMAND fireside_tests)

add_dependencies(fireside_tests firesidetx)

# Transmit isolation (plan step 8): the TX DSP and firesidetx can never include or link radio code. See tx_isolation.cmake.
set(TXI ${CMAKE_CURRENT_SOURCE_DIR}/tx_isolation.cmake)
set(TXI_FIX ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tx_isolation)
set(TX_ROOTS "${CMAKE_SOURCE_DIR}/core/include/fireside/tx_dsp.h;${CMAKE_SOURCE_DIR}/core/src/tx_dsp.cpp;${CMAKE_SOURCE_DIR}/core/include/fireside/tx_render.h;${CMAKE_SOURCE_DIR}/core/src/tx_render.cpp;${CMAKE_SOURCE_DIR}/tools/firesidetx.cpp")
add_test(NAME tx_isolation_source COMMAND ${CMAKE_COMMAND} -DMODE=source "-DROOTS=${TX_ROOTS}" -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/core/include -P ${TXI})
add_test(NAME tx_isolation_binary COMMAND ${CMAKE_COMMAND} -DMODE=binary -DBINARY=$<TARGET_FILE:firesidetx> -DARCHIVE=$<TARGET_FILE:fireside_core> -DNM=${CMAKE_NM} -P ${TXI})
# Self-tests: each must be caught, and for the stated reason (not a script error).
add_test(NAME tx_isolation_catches_transitive_include COMMAND ${CMAKE_COMMAND} -DMODE=source -DROOTS=${TXI_FIX}/root_ok_header.cpp -DINCLUDE_DIR=${TXI_FIX}/include -P ${TXI})
set_tests_properties(tx_isolation_catches_transitive_include PROPERTIES PASS_REGULAR_EXPRESSION "TX ISOLATION VIOLATION: .*dsp\\.h includes fireside/link\\.h")
add_test(NAME tx_isolation_catches_device_access COMMAND ${CMAKE_COMMAND} -DMODE=source -DROOTS=${TXI_FIX}/root_device.cpp -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/core/include -P ${TXI})
set_tests_properties(tx_isolation_catches_device_access PROPERTIES PASS_REGULAR_EXPRESSION "TX ISOLATION VIOLATION: .*mentions .*/dev/")
add_test(NAME tx_isolation_catches_system_library COMMAND ${CMAKE_COMMAND} -DMODE=source -DROOTS=${TXI_FIX}/root_alsa.cpp -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/core/include -P ${TXI})
set_tests_properties(tx_isolation_catches_system_library PROPERTIES PASS_REGULAR_EXPRESSION "TX ISOLATION VIOLATION: .*<alsa/asoundlib\\.h>")
add_test(NAME tx_isolation_catches_radio_code_in_a_binary COMMAND ${CMAKE_COMMAND} -DMODE=binary -DBINARY=$<TARGET_FILE:firesidecli> -DARCHIVE=$<TARGET_FILE:fireside_core> -DNM=${CMAKE_NM} -P ${TXI})
set_tests_properties(tx_isolation_catches_radio_code_in_a_binary PROPERTIES PASS_REGULAR_EXPRESSION "TX ISOLATION VIOLATION: .*firesidecli contains .* from [a-z_]+\\.cpp\\.o")

# The transmit tab of the GUI (offline lab): same rule, plus no transmit key / session / sound / network words, and only TxPanel.h and Theme.h as local includes.
set(TX_GUI_ROOTS "${CMAKE_SOURCE_DIR}/gui/TxPanel.h;${CMAKE_SOURCE_DIR}/gui/TxPanel.cpp")
add_test(NAME tx_isolation_gui_source COMMAND ${CMAKE_COMMAND} -DMODE=source -DGUI=ON -DLOCAL_DIR=${CMAKE_SOURCE_DIR}/gui "-DROOTS=${TX_GUI_ROOTS}" -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/core/include -P ${TXI})
foreach(case key:mentions.*PTT network:mentions.*QTcp other_panel:includes.*LogPanel\\.h slash_include:includes.*QtSql/QSqlDatabase sound:mentions.*QAudio)
  string(REPLACE ":" ";" parts "${case}")
  list(GET parts 0 name)
  list(GET parts 1 rx)
  add_test(NAME tx_isolation_gui_catches_${name} COMMAND ${CMAKE_COMMAND} -DMODE=source -DGUI=ON -DLOCAL_DIR=${CMAKE_SOURCE_DIR}/gui -DROOTS=${TXI_FIX}/gui/gui_${name}.cpp -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/core/include -P ${TXI})
  set_tests_properties(tx_isolation_gui_catches_${name} PROPERTIES PASS_REGULAR_EXPRESSION "TX ISOLATION VIOLATION: .*${rx}")
endforeach()
