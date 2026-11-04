add_library(ScopeOneNIDaqmx MODULE
    NIDaqmxDriver.cpp
    driver.json
)
target_link_libraries(ScopeOneNIDaqmx PRIVATE scopeone::DriverSDK Qt6::Core)
scopeone_add_driver(ScopeOneNIDaqmx)
