target_sources(ninfer_ops PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/tail_extract.cu"
  "${CMAKE_CURRENT_LIST_DIR}/prepare/prepare.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/prepare/launch.cu"
  "${CMAKE_CURRENT_LIST_DIR}/indexer/indexer.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/indexer/launch.cu"
  "${CMAKE_CURRENT_LIST_DIR}/sparse_gqa/sparse_gqa.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/sparse_gqa/launch.cu"
)
