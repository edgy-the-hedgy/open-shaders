find_program(REDSTONE_DXC NAMES dxc.exe dxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64"
          "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
set(REDSTONE_DXIL_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/RedstoneShadows")
set(REDSTONE_DXIL_FILES "")
set(REDSTONE_DXIL_CORE_COMMANDS "")
if(REDSTONE_DXC)
    set(_redstone_directional "${REDSTONE_DXIL_DIRECTORY}/directional.dxil")
    add_custom_command(OUTPUT "${_redstone_directional}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/features/Light Limit Fix/Shaders"
            -Fo "${_redstone_directional}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneShadows/Shaders/RedstoneShadows/directional.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneShadows/Shaders/RedstoneShadows/directional.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneShadows/Shaders/RedstoneShadows/materials.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneShadows/Shaders/RedstoneShadows/directLighting.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneShadows/Shaders/RedstoneShadows/indirectLighting.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneShadows/Shaders/RedstoneShadows/directContract.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Light Limit Fix/Shaders/LightLimitFix/Attenuation.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Light Limit Fix/Shaders/LightLimitFix/Common.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/Game.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PointLightFlags.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/ReceiverRoom.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PBRMath.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/LightingCommon.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/BRDF.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/Math.hlsli"
        VERBATIM)
    add_custom_target(RedstoneShadowDXIL DEPENDS "${_redstone_directional}")
    add_dependencies(${PROJECT_NAME} RedstoneShadowDXIL)
    list(APPEND REDSTONE_DXIL_FILES "${_redstone_directional}")
    register_feature_payload(RedstoneShadows FILES ${REDSTONE_DXIL_FILES} DESTINATION "Shaders/RedstoneShadows")
    list(APPEND REDSTONE_DXIL_CORE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Core/Shaders/RedstoneShadows"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${REDSTONE_DXIL_FILES} "${CMAKE_CURRENT_BINARY_DIR}/Core/Shaders/RedstoneShadows")
else()
    message(WARNING "DXC was not found: RedstoneShadows directional traversal bytecode will not be packaged; native shadows remain available.")
endif()
