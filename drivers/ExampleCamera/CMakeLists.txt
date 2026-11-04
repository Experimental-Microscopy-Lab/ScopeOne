add_library(ScopeOneExampleCamera MODULE
    ExampleCameraDriver.cpp
    TestPatterns.cpp
    TestPatterns.h
    driver.json
)
target_link_libraries(ScopeOneExampleCamera PRIVATE scopeone::DriverSDK Qt6::Core)
scopeone_add_driver(ScopeOneExampleCamera)
