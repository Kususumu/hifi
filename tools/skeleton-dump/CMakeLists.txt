set(TARGET_NAME skeleton-dump)
setup_hifi_project(Core)
setup_memory_debugger()
link_hifi_libraries(shared fbx hfm graphics gpu gl animation)
