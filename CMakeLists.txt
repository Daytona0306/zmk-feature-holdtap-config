zephyr_include_directories(include)

if(CONFIG_ZMK_HOLDTAP_RUNTIME)
  zephyr_library()
  target_sources(app PRIVATE src/holdtap_runtime.c)
endif()
