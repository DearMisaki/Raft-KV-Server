
set(SRC_LIST friendService.cpp ../friend.pb.cc)

add_executable(provider ${SRC_LIST})
target_link_libraries(provider rpc_lib protobuf::libprotobuf muduo_net muduo_base pthread )
