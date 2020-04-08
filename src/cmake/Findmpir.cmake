find_library(MPIR_LIBRARY mpir)
find_library(MPIR_CXX_LIBRARY mpirxx)

message("-- Found mpir library at ${MPIR_LIBRARY}")
message("-- Found mpirxx library at ${MPIR_CXX_LIBRARY}")

find_path(MPIR_INCLUDE_DIR "mpir.h")
