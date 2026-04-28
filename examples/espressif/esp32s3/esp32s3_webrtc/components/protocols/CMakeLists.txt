set(COMPONENT_ADD_INCLUDEDIRS include)

register_component(INCLUDE_DIRS "." "include")

target_link_libraries(${COMPONENT_TARGET} INTERFACE "-L ${CMAKE_CURRENT_SOURCE_DIR}/libs")

target_link_libraries(${COMPONENT_TARGET} INTERFACE br_ai_voice_rtc)
