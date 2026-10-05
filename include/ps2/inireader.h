#ifndef H_INIREADER
#define H_INIREADER
#include "rini.h"

struct inireader_t {
    const char* iniBuf;
    int iniBufSize;
    void (*SetIniPath)(const char* szFileContent, size_t size);
    int (*ReadInteger)(const char* szSection, const char* szKey, int iDefaultValue);
    float (*ReadFloat)(const char* szSection, const char* szKey, float fltDefaultValue);
    bool (*ReadBoolean)(const char* szSection, const char* szKey, bool bolDefaultValue);
    const char* (*ReadString)(const char* szSection, const char* szKey, const char* szDefaultValue, char* Buffer, int BufferSize);
};
extern struct inireader_t inireader;
#endif
