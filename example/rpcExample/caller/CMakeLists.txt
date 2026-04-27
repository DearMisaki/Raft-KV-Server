# set(SRC_LIST calluserservice.cc ../user.pb.cc)
set(SRC_LIST callFriendService.cpp ../friend.pb.cc)
add_executable(consumer ${SRC_LIST})
target_link_libraries(consumer rpc_lib protobuf::libprotobuf absl_log_internal_check_op absl_log_internal_message absl_log_internal_nullguard)