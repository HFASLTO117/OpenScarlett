# OpenScarlett — Minecraft Preview Routing & API-Set Contracts Tests

openscarlett_add_test(test_mine_routing_apisets tests/test_mine_routing_apisets.cpp)
add_dependencies(test_mine_routing_apisets openscarlett)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_mine_routing_apisets)
endif()
