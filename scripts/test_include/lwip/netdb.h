#pragma once
struct addrinfo { int placeholder; };
int getaddrinfo(const char *,const char *,const struct addrinfo *,struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
