#
# This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
# information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
# or (at your option) any later version.
#
# Included (OPTIONAL) by the core's modules/CMakeLists.txt, in the scope that creates the module targets.
# MSVC only: the Boost.Beast and Boost.JSON translation units of the CoA brain need /bigobj. Other compilers:
# nothing. Idempotent: including it twice adds nothing twice.

if(MSVC)
  # The module is often a symlink under modules/: name the files through every path its sources may carry.
  get_filename_component(_coa_brain_real "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
  set(_coa_brain_roots
    "${CMAKE_CURRENT_LIST_DIR}"
    "${_coa_brain_real}"
    "${CMAKE_SOURCE_DIR}/modules/mod-playerbots")
  list(REMOVE_DUPLICATES _coa_brain_roots)

  foreach(_coa_brain_root IN LISTS _coa_brain_roots)
    foreach(_coa_brain_file BrainHttpClient.cpp BrainJson.cpp)
      set(_coa_brain_source "${_coa_brain_root}/src/Coa/Brain/Net/${_coa_brain_file}")
      get_source_file_property(_coa_brain_options "${_coa_brain_source}" COMPILE_OPTIONS)
      if(NOT _coa_brain_options)
        set(_coa_brain_options "")
      endif()
      if(NOT "/bigobj" IN_LIST _coa_brain_options)
        set_property(SOURCE "${_coa_brain_source}" APPEND PROPERTY COMPILE_OPTIONS "/bigobj")
      endif()
    endforeach()
  endforeach()

  unset(_coa_brain_real)
  unset(_coa_brain_roots)
  unset(_coa_brain_root)
  unset(_coa_brain_file)
  unset(_coa_brain_source)
  unset(_coa_brain_options)
endif()
