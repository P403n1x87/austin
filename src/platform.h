#if defined(__linux__)
  #define PL_LINUX

#elif defined(__APPLE__) && defined(__MACH__)
  #define PL_MACOS

#elif defined(_WIN32) || defined(_WIN64)
  #define PL_WIN

#endif


#if defined(PL_LINUX) || defined(PL_MACOS)
  #define PL_UNIX
#endif
