# Test tools, see docs/testing.md

if (WIVRN_BUILD_SERVER)
	add_executable(wivrn-virtual-headset virtual_headset/main.cpp)
	target_link_libraries(wivrn-virtual-headset PRIVATE xrt-external-openxr wivrn-common CLI11::CLI11 nlohmann_json::nlohmann_json)
endif()

if (WIVRN_BUILD_SERVER)
	add_executable(wivrn-xr-tests xr/wivrn_xr_tests.cpp)
	target_link_libraries(wivrn-xr-tests PRIVATE xrt-external-openxr wivrn-common Vulkan::Vulkan CLI11::CLI11 nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
endif()
