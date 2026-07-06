# ==============================================================================
# IP address validation utility for CMake cache variables.
#
# Provides:
#   validate_ip_address(address variable_name)
#
# Callers use it to ensure a cache variable is a valid IPv4 or IPv6 address
# at configure time, e.g.:
#
#   include(ValidateIpAddress)
#   validate_ip_address("${MY_VAR}" "MY_VAR")
#
# On failure a FATAL_ERROR is raised with the given variable_name in the
# message.
# ==============================================================================

function(validate_ip_address _addr _var_name)
  # --- IPv4 ---
  if ("${_addr}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    foreach (_i RANGE 1 4)
      if (${CMAKE_MATCH_${_i}} LESS 0 OR ${CMAKE_MATCH_${_i}} GREATER 255)
        message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
      endif ()
    endforeach ()
    return()
  endif ()

  # --- IPv6 (basic structural check) ---
  string(REGEX MATCHALL ":" _colons "${_addr}")
  list(LENGTH _colons _colon_count)
  string(FIND "${_addr}" "::" _has_dd)

  # Check for characters other than hex digits and colons
  if (NOT "${_addr}" MATCHES "^[0-9a-fA-F:]+$")
    message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
  endif ()

  # Check lone leading/trailing colon
  if ("${_addr}" MATCHES "^:[^:]" OR "${_addr}" MATCHES "[^:]:$")
    message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
  endif ()

  # Check colon/group count
  if (_has_dd EQUAL -1)
    if (NOT _colon_count EQUAL 7)
      message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
    endif ()
  elseif (_colon_count GREATER 7)
    message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
  endif ()

  # Check each hex group
  string(REGEX MATCHALL "[0-9a-fA-F]+" _groups "${_addr}")
  list(LENGTH _groups _group_count)
  if (_group_count GREATER 8)
    message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
  endif ()
  foreach (_g ${_groups})
    string(LENGTH "${_g}" _len)
    if (_len GREATER 4)
      message(FATAL_ERROR "${_var_name}: invalid IP address '${_addr}'")
    endif ()
  endforeach ()
endfunction()
