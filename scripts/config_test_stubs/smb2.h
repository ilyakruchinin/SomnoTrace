#pragma once
#include <stdint.h>
#include <stddef.h>
#define SMB2_NEGOTIATE_SIGNING_ENABLED 1
struct smb2_context { int unused; };
struct smb2fh { int unused; };
struct smb2_context *smb2_init_context(void);
void smb2_destroy_context(struct smb2_context *);
void smb2_set_timeout(struct smb2_context *,int);
void smb2_set_security_mode(struct smb2_context *,int);
void smb2_set_user(struct smb2_context *,const char *);
void smb2_set_password(struct smb2_context *,const char *);
int smb2_connect_share(struct smb2_context *,const char *,const char *,const char *);
int smb2_disconnect_share(struct smb2_context *);
struct smb2fh *smb2_open(struct smb2_context *,const char *,int);
int smb2_pwrite(struct smb2_context *,struct smb2fh *,const uint8_t *,uint32_t,uint64_t);
int smb2_pread(struct smb2_context *,struct smb2fh *,uint8_t *,uint32_t,uint64_t);
int smb2_fsync(struct smb2_context *,struct smb2fh *);
int smb2_close(struct smb2_context *,struct smb2fh *);
int smb2_unlink(struct smb2_context *,const char *);
