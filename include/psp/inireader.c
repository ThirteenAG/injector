#include "inireader.h"
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <string.h>

typedef enum {
    STR2INT_SUCCESS,
    STR2INT_OVERFLOW,
    STR2INT_UNDERFLOW,
    STR2INT_INCONVERTIBLE
} str2int_errno;

str2int_errno _str2int(int* out, char* s, int base) {
    char* end;
    if (s[0] == '\0' || isspace((unsigned char)s[0]))
        return STR2INT_INCONVERTIBLE;
    errno = 0;
    long l = strtol(s, &end, base);
    /* Both checks are needed because INT_MAX == LONG_MAX is possible. */
    if (l > INT_MAX || (errno == ERANGE && l == LONG_MAX))
        return STR2INT_OVERFLOW;
    if (l < INT_MIN || (errno == ERANGE && l == LONG_MIN))
        return STR2INT_UNDERFLOW;
    if (*end != '\0')
        return STR2INT_INCONVERTIBLE;
    *out = l;
    return STR2INT_SUCCESS;
}

int str2int(char* s, int base) {
    int result = 0;
    _str2int(&result, s, base);
    return result;
}

void SetIniPath(const char* szFileName)
{
    inireader.iniName = szFileName;

    static char buf[8192];
    buf[0] = 0;
    inireader.iniBufSize = 0;
    inireader.iniBuf = buf;

    SceUID fileId = sceIoOpen(inireader.iniName, PSP_O_RDONLY, 0777);
    if (fileId < 0)
    {
        return;
    }
    const int size = sceIoRead(fileId, buf, sizeof(buf) - 1);
    sceIoClose(fileId);
    if (size > 0)
    {
        buf[size] = 0;
        inireader.iniBufSize = size;
    }
}

int ReadInteger(const char* szSection, const char* szKey, int iDefaultValue)
{
    int result = iDefaultValue;
    if (rini_get_key(szSection, szKey, inireader.iniBuf, inireader.iniBufSize, &result, sizeof(result), INT_VAL))
    {
        return result;
    }
    else
    {
        return iDefaultValue;
    }
}

float ReadFloat(const char* szSection, const char* szKey, float fltDefaultValue)
{
    char Buffer[30];
    const int BufferSize = sizeof(Buffer);
    if (rini_get_key(szSection, szKey, inireader.iniBuf, inireader.iniBufSize, Buffer, BufferSize, STRING_VAL))
    {
        return (float)strtod(Buffer, NULL);
    }
    else
    {
        return fltDefaultValue;
    }
}

// "Key = 1" as well as "Key=1": the value is trimmed first. 1/true/yes/on and 0/false/no/off, any case; anything else
// is the default
bool ReadBoolean(const char* szSection, const char* szKey, bool bDefaultValue)
{
    char Buffer[16];
    if (!rini_get_key(szSection, szKey, inireader.iniBuf, inireader.iniBufSize, Buffer, sizeof(Buffer), STRING_VAL))
        return bDefaultValue;

    const char* value = Buffer;
    while (*value == ' ' || *value == '\t')
        ++value;
    char word[8];
    int length = 0;
    while (value[length] && value[length] != ' ' && value[length] != '\t' && value[length] != '\r' && value[length] != '\n')
    {
        if (length == sizeof(word) - 1)
            return bDefaultValue;
        char c = value[length];
        word[length++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    word[length] = '\0';
    // One word only: anything but spaces after it ("1 0") is not a boolean
    for (const char* rest = value + length; *rest; ++rest)
    {
        if (*rest != ' ' && *rest != '\t' && *rest != '\r' && *rest != '\n')
            return bDefaultValue;
    }

    static const char* const yes[] = { "1", "true", "yes", "on" };
    static const char* const no[] = { "0", "false", "no", "off" };
    for (int i = 0; i < 4; ++i)
    {
        if (strcmp(word, yes[i]) == 0)
            return true;
        if (strcmp(word, no[i]) == 0)
            return false;
    }
    return bDefaultValue;
}

const char* ReadString(const char* szSection, const char* szKey, const char* szDefaultValue, char* Buffer, int BufferSize)
{
    if (rini_get_key(szSection, szKey, inireader.iniBuf, inireader.iniBufSize, Buffer, BufferSize, STRING_VAL))
    {
        while (*Buffer == ' ')
            ++Buffer;
        return Buffer;
    }
    else
    {
        return szDefaultValue;
    }
}

struct inireader_t inireader =
{
    .iniName = 0,
    .iniBuf = 0,
    .iniBufSize = 0,
    .SetIniPath = SetIniPath,
    .ReadInteger = ReadInteger,
    .ReadFloat = ReadFloat,
    .ReadBoolean = ReadBoolean,
    .ReadString = ReadString,
};
