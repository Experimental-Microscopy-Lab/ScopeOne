# The serial driver runs the description files in profiles, and it is the only driver that links
# Qt SerialPort
find_package(Qt6 ${SCOPEONE_QT_MINIMUM_VERSION} REQUIRED COMPONENTS Core SerialPort)

add_library(ScopeOneSerial MODULE
    SerialDriver.cpp
    SerialDevice.cpp
    SerialDevice.h
    driver.json
)
target_link_libraries(ScopeOneSerial PRIVATE scopeone::DriverSDK Qt6::Core Qt6::SerialPort)
scopeone_add_driver(ScopeOneSerial)

# The description files sit beside the libraries, and DriverManifest lists them as drivers
install(FILES
    profiles/cobolt-05-01.json
    DESTINATION "drivers"
)
