# ../../Plugin.cmake upstream: correct when the plugin sits at src/plugins/<name>.
# In this fork third-party plugins live one level up in plugins/, so the path needs
# the src/ segment -- same include the vcxproj->CMake converter emits for the other
# plugins/ dwellers (see plugins/MQ2EQBC/CMakeLists.txt).
include("../../src/Plugin.cmake")

find_package(fmt REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)

set(MQMCP_SOURCES "MQMCP.cpp")
source_groups("Source Files" ${MQMCP_SOURCES})

add_library(MQMCP SHARED ${MQMCP_SOURCES})

set_target_properties(MQMCP PROPERTIES FOLDER "plugins")

target_Plugin_props(MQMCP)

target_link_libraries(MQMCP PRIVATE
    fmt::fmt
    httplib::httplib
    nlohmann_json::nlohmann_json
    MQ2Main
)
