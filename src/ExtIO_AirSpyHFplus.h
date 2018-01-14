// The following ifdef block is the standard way of creating macros which make exporting 
// from a DLL simpler. All files within this DLL are compiled with the LIBEXTIO_EXPORTS
// symbol defined on the command line. This symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see 
// LIBEXTIO_API functions as being imported from a DLL, whereas this DLL sees symbols
// defined with this macro as being exported.
#ifdef LIBEXTIO_EXPORTS
#define LIBEXTIO_API __declspec(dllexport)
#else
#define LIBEXTIO_API __declspec(dllimport)
#endif

extern HMODULE hInst;
