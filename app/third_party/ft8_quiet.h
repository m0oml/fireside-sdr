// Force-included when compiling ft8_lib: its LOG() calls (monitor.c enables LOG_INFO) would print to stderr inside our app. This turns them into no-ops without touching upstream files.
#define LOG_PRINTF(...) ((void)0)
