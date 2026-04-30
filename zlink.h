#ifndef ZLINK_H
#define ZLINK_H

#ifdef __cplusplus
extern "C" {
#endif

int zinc_lld_link(
        const char *objfile,        const char *outfile,
        const char **extra_args,    int extra_args_count);

#ifdef __cplusplus
}
#endif

#endif
