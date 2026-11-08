# Lays out what the installers put in the Atlas folder
# (Documents/Blind Producers Plugins/Atlas): every library with all of its
# kind folders, empty where Atlas ships nothing, and the Factory banks from
# Factory/Banks. The folder names are those of src/common/library_layout.cpp
# (libraryName and kindFolderName); keep the two in step.
#
#   cmake -DDESTINATION=<folder> -P installer/stage_library.cmake
#
# DESTINATION is emptied first, so a preset taken out of Factory/Banks does
# not stay in the next installer.
cmake_minimum_required(VERSION 3.22)

if(NOT DESTINATION)
  message(FATAL_ERROR "Pass -DDESTINATION=<folder>")
endif()
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(destination "${DESTINATION}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(destination STREQUAL root OR destination STREQUAL "${root}/Factory")
  message(FATAL_ERROR "DESTINATION must not be the repository or its Factory folder")
endif()

set(libraries "Factory" "User" "Third Party" "Community")
set(kinds "Banks" "Wavetables" "Multisamples" "Samples" "LFO Presets" "FX Presets" "Impulse Responses")

file(REMOVE_RECURSE "${destination}")
foreach(library IN LISTS libraries)
  foreach(kind IN LISTS kinds)
    file(MAKE_DIRECTORY "${destination}/${library}/${kind}")
  endforeach()
endforeach()

file(GLOB banks LIST_DIRECTORIES true "${root}/Factory/Banks/*")
if(NOT banks)
  message(FATAL_ERROR "No banks in ${root}/Factory/Banks")
endif()
file(COPY "${root}/Factory/Banks/" DESTINATION "${destination}/Factory/Banks"
     PATTERN ".DS_Store" EXCLUDE)
file(GLOB_RECURSE presets "${destination}/Factory/Banks/*.atp")
list(LENGTH presets count)
message(STATUS "Staged ${count} Factory presets in ${destination}")
