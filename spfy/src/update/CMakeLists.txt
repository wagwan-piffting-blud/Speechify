# Update checking, split so the SAPI DLL can take the cheap half and nothing
# else.
#
#   spfy_update_trigger  paths + state + spawn. Pure string and file work.
#                        spfy_sapi.dll links THIS and only this: it runs
#                        in-process inside Narrator, so it may not fetch, may
#                        not hash and may not open a window.
#
#   spfy_update_core     everything that talks to the network or reads a
#                        96 MB VDB. Linked by spfy_synth (whose check runs
#                        after the WAV is written) and by spfy_update.
#
# WinHTTP is deliberately NOT linked -- upd_fetch.c LoadLibrary's it. Putting
# winhttp.dll in spfy_synth.exe's import table would stop the i686 build from
# starting on the older Windows the x86 installer exists to serve.

add_library(spfy_update_trigger STATIC
    upd_paths.c
    upd_state.c
    upd_trigger.c
    mini_json.c
)
target_include_directories(spfy_update_trigger
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spfy_update_trigger PUBLIC spfy_common)

add_library(spfy_update_core STATIC
    sha256.c
    upd_fetch.c
    upd_stamp.c
    upd_check.c
    upd_notify.c
    upd_run.c
)
target_link_libraries(spfy_update_core PUBLIC spfy_update_trigger spfy_common)
if(WIN32)
    # shell32: Shell_NotifyIcon (the tray balloon) + ShellExecute (open the
    # release page). user32: the message-only window behind the balloon.
    # Both have existed since NT 4.0, so neither raises the floor the way a
    # winhttp import would.
    target_link_libraries(spfy_update_core PUBLIC shell32 user32)
endif()

add_executable(spfy_update main_update.c)
target_link_libraries(spfy_update PRIVATE spfy_update_core)
