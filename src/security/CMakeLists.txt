
file(GLOB_RECURSE TURBO_SECURITY_SRCS *.cpp *.h)

target_sources(ub_turbo_exec PRIVATE ${TURBO_SECURITY_SRCS})
