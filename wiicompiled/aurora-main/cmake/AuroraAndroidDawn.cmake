# Static Android packages retain a CI-local NDK liblog path. Rebind only the
# imported consumer target, preserving the hash-verified package on disk.
function(aurora_rebind_android_dawn)
  get_target_property(_dawn_links dawn::webgpu_dawn INTERFACE_LINK_LIBRARIES)
  string(REGEX REPLACE "/[^;]*android[^;]*/liblog\\.so" "log" _dawn_links "${_dawn_links}")
  set_property(TARGET dawn::webgpu_dawn PROPERTY INTERFACE_LINK_LIBRARIES "${_dawn_links}")
  if(_dawn_links MATCHES "/usr/local/lib/android")
    message(FATAL_ERROR "Unrebound CI NDK dependency: ${_dawn_links}")
  endif()
endfunction()
