#pragma once
#include <stddef.h>
int mbedtls_sha256(const unsigned char *,size_t,unsigned char[32],int);
