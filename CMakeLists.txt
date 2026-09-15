# ../../Plugin.cmake upstream: correct when the plugin sits at src/plugins/<name>.
# In this fork third-party plugins live one level up in plugins/, so the path needs
# the src/ segment -- same include the vcxproj->CMake converter emits for the other
# plugins/ dwellers (see plugins/MQ2EQBC/CMakeLists.txt).
include("../../src/Plugin.cmake")

find_package(fmt REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)

set(MQMCPServer_SOURCES "MQMCPServer.cpp")
source_groups("Source Files" ${MQMCPServer_SOURCES})

add_library(MQMCPServer SHARED ${MQMCPServer_SOURCES})

set_target_properties(MQMCPServer PROPERTIES FOLDER "plugins")

target_Plugin_props(MQMCPServer)

target_link_libraries(MQMCPServer PRIVATE
    fmt::fmt
    httplib::httplib
    nlohmann_json::nlohmann_json
    MQ2Main
)
