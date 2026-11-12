add_library(ScopeOneExamplePmt MODULE
    ExamplePmtDriver.cpp
    driver.json
)
target_link_libraries(ScopeOneExamplePmt PRIVATE scopeone::DriverSDK Qt6::Core)
scopeone_add_driver(ScopeOneExamplePmt)
