download_ffx_runtime(
    amd_fidelityfx_denoiser_dx12.dll
    48F1E5888BA6A0A3D59A98B9751E37C392B0F7B8C223D0082D5C1F40642879D3
)

split_runtime_payload(FFX_RUNTIME)

register_feature_payload(
    Upscaling
    FILES "${FFX_RUNTIME_DIRECTORY}/amd_fidelityfx_denoiser_dx12.dll"
    DESTINATION "${FFX_RUNTIME_RELATIVE_DIRECTORY}"
)
