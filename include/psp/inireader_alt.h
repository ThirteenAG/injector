#pragma once
#include <psptypes.h>
#include <stdlib.h>
#include <stdbool.h>

struct ini_t {
	const char* _iniName;
	void (*Set_Ini_Path)(const char* szFileName);
	int (*Read_Integer)(const char* szSection, const char* szKey, int iDefaultValue);
	float (*Read_Float)(const char* szSection, const char* szKey, float fltDefaultValue);
	bool (*Read_Boolean)(const char* szSection, const char* szKey, bool bolDefaultValue);
	char* (*Read_String)(const char* szSection, const char* szKey, const char* szDefaultValue, char* Buffer, int BufferSize);
	void (*Write_Integer)(const char* szSection, const char* szKey, int iValue);
	void (*Write_Float)(const char* szSection, const char* szKey, float fltValue);
	void (*Write_Boolean)(const char* szSection, const char* szKey, bool bolValue);
	void (*Write_String)(const char* szSection, const char* szKey, const char* szValue);
};
extern struct ini_t ini;
