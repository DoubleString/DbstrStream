#include <stdio.h>
#include <stdlib.h>

char* trim(char* pStr) {

	int len = strlen(pStr);
	char* pIndex = pStr + len - 1;

	while (*pIndex == '\n' || *pIndex == '\r' || *pIndex == '\0'
			|| *pIndex == ' ') {
		*pIndex = '\0';
		pIndex--;
	}
	return pStr;
}
int pointer_string(int row, int col, char** string_array, char* string) {
	int i;
	char (*pStr)[col] = string_array;
	trim(string);

	for (i = 0; i < row; i++) {
		if (strcmp(pStr + i, string) == 0)
			break;
	}
	if (i == row)
		i = -1;
	return i;
}
int len_trim(char* pStr) {
	int length = strlen(pStr);
	int count = length;
	int i;
	for (i = length - 1; i >= 0; i--) {
		if (pStr[i] == '\0' || pStr[i] == '\n' || pStr[i] == '\r'
				|| pStr[i] == ' ')
			count--;
		else
			break;
	}
	return count;
}
//start:started with index of zero
char* substringEx(char* dest, char* src, int start, int length) {
	int i, j = 0;
	int len = strlen(src);
	if (start < 0 || start >= len || start + length > len) {
		dest[0] = '\0';
		return NULL;
	}
	for (i = start; i < start + length; i++) {
		dest[j] = src[i];
		j++;
	}
	dest[j] = '\0';
	return dest;
}
char* substring(char* pStr, int start, int end) {
	int len = strlen(pStr);
	int i, j;
	char* pRes = NULL;
	if (start < 0 || start >= len || end < 0 || end >= len || end < start)
		return NULL;
	pRes = (char*) calloc(end - start + 2, sizeof(char));
	j = 0;
	for (i = start; i <= end; i++) {
		pRes[j] = pStr[i];
		j++;
	}
	pRes[j] = '\0';
	return pRes;
}

void clearstring(char* pStr) {
	int len = strlen(pStr);
	int i;
	for (i = 0; i < len; i++) {
		pStr[i] = '\0';
	}
}
//from left to right
int index_string(char* src, char key) {
	int len = strlen(src);
	int i;
	for (i = 0; i < len; i++) {
		if (src[i] == key)
			break;
	}
	if (i == len)
		return -1;
	else
		return i;
}
//should not be const char*
char* left_justify_string(char* string) {
	int p = 0;
	while (*(string + p) == ' ' || *(string + p) == '\n'
			|| *(string + p) == '\r')
		p++;
	return string + p;
}

char* upper_string(char* value) {
	int len = strlen(value);
	int i;
	for (i = 0; i < len; i++) {
		if (value[i] >= 'a' && value[i] <= 'z')
			value[i] -= 32;
	}
	return value;
}
char* lower_string(char* value) {
	int len = strlen(value);
	int i;
	for (i = 0; i < len; i++) {
		if (value[i] >= 'A' && value[i] <= 'Z')
			value[i] += 32;
	}
	return value;
}

