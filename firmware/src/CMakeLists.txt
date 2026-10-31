idf_component_register(SRCS "main.cpp"
    INCLUDE_DIRS "../include" "../../common"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer esp_driver_gpio json)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
