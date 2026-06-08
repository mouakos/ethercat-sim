# Unit tests for the device models and EtherCAT core logic.

add_executable(test_ecat_frame test_ecat_frame.cpp)
target_link_libraries(test_ecat_frame PRIVATE ec-core-lib)
add_test(NAME ecat_frame COMMAND test_ecat_frame)

add_executable(test_station test_station.cpp)
target_link_libraries(test_station PRIVATE ec-core-lib)
add_test(NAME station COMMAND test_station)

add_executable(test_chain test_chain.cpp)
target_link_libraries(test_chain PRIVATE ec-core-lib)
add_test(NAME chain COMMAND test_chain)

add_executable(test_coe test_coe.cpp)
target_link_libraries(test_coe PRIVATE ec-core-lib)
add_test(NAME coe COMMAND test_coe)

add_executable(test_aoe test_aoe.cpp)
target_link_libraries(test_aoe PRIVATE ec-core-lib)
add_test(NAME aoe COMMAND test_aoe)

# TELLTALE: removable diagnostic instrumentation (grep TELLTALE to remove)
add_executable(test_telltale test_telltale.cpp)
target_link_libraries(test_telltale PRIVATE ec-core-lib)
add_test(NAME telltale COMMAND test_telltale)
