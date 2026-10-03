# Adds the guest app to Vanadium's build without editing Vanadium's CMake files. Configure Vanadium with
#   -DCMAKE_PROJECT_Vanadium_INCLUDE=<this file>
# and project(Vanadium) includes it. The engine's targets don't exist yet at that point, so the guest's target is
# defined once the top-level CMakeLists.txt has been read to its end. (A deferred call may not add a subdirectory,
# so the target is defined by an included file in the top-level directory instead.)
set(GTR_GUEST_CMAKE "${CMAKE_CURRENT_LIST_DIR}/../guest/Guest.cmake")
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL include "${GTR_GUEST_CMAKE}")
