# Included at the end of Vanadium's top-level CMakeLists.txt (see cmake/GtrInject.cmake), so CMAKE_SOURCE_DIR is Vanadium's
# and paths here are spelled out from this file's folder
set(GTR_GUEST_DIR "${CMAKE_CURRENT_LIST_DIR}")

add_executable(Gtr.Guest)
set_target_properties(Gtr.Guest PROPERTIES
    OUTPUT_NAME "gtr-guest"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/GtrGuest"
)
target_link_libraries(Gtr.Guest PRIVATE Vanadium.SDLApp Vanadium.Engine)
if (WIN32)
    target_link_libraries(Gtr.Guest PRIVATE ws2_32)
endif()
target_include_directories(Gtr.Guest PRIVATE "${GTR_GUEST_DIR}/../shared")
target_sources(Gtr.Guest PRIVATE
    "${GTR_GUEST_DIR}/main.cpp"
    "${GTR_GUEST_DIR}/Bodies.cpp"
    "${GTR_GUEST_DIR}/Bridge.cpp"
    "${GTR_GUEST_DIR}/FrameExporter.cpp"
    "${GTR_GUEST_DIR}/HostLink.cpp"
)
target_compile_features(Gtr.Guest PUBLIC cxx_std_23)

vanadium_embed_core_scripts(Gtr.Guest SET Game "${CMAKE_SOURCE_DIR}/Engine/Scripts/CoreScripts")
vanadium_embed_core_scripts(Gtr.Guest SET PlayerScripts "${CMAKE_SOURCE_DIR}/Engine/Scripts/PlayerScripts")
vanadium_copy_engine_content(Gtr.Guest)

# The scripts the guest runs in whatever place it plays (see Bridge::DressPlace), beside the executable
add_custom_command(TARGET Gtr.Guest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GTR_GUEST_DIR}/scripts" "$<TARGET_FILE_DIR:Gtr.Guest>/scripts"
    COMMENT "Copying the guest's place scripts")
# Models the place scripts load (game:GetObjects), in the guest's content where rbxasset://gtr/ finds them
add_custom_command(TARGET Gtr.Guest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GTR_GUEST_DIR}/models" "$<TARGET_FILE_DIR:Gtr.Guest>/content/gtr"
    COMMENT "Copying the guest's models")
