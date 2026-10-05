add_executable(headless-shot main.cpp)
add_dependencies(headless-shot ladybird_build_resource_files ${ladybird_helper_processes})
target_link_libraries(headless-shot PRIVATE AK LibCore LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
