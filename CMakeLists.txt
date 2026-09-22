zephyr_include_directories(include)

if(CONFIG_ZMK_HOLDTAP_RUNTIME)
  zephyr_library()
  target_sources(app PRIVATE src/holdtap_runtime.c)
  if(CONFIG_ZMK_HOLDTAP_RUNTIME_STUDIO_RPC)
    target_sources(app PRIVATE src/studio/holdtap_rpc_stub.c)
  endif()
endif()
