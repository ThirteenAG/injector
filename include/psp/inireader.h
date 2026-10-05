#include "rini.h"
#include <pspuser.h>
#include <stdlib.h>

int str2int(char* s, int base);

struct inireader_t {
	const char* iniName;
	const char* iniBuf;
	int iniBufSize;
	void (*SetIniPath)(const char* szFileName);
	int (*ReadInteger)(const char* szSection, const char* szKey, int iDefaultValue);
	float (*ReadFloat)(const char* szSection, const char* szKey, float fltDefaultValue);
	bool (*ReadBoolean)(const char* szSection, const char* szKey, bool bolDefaultValue);
	const char* (*ReadString)(const char* szSection, const char* szKey, const char* szDefaultValue, char* Buffer, int BufferSize);
};
extern struct inireader_t inireader;