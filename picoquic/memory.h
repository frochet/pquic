#ifndef MEMORY_H
#define MEMORY_H

#include "picoquic.h"

void *my_malloc(picoquic_cnx_t *cnx, unsigned int size);
void my_free(picoquic_cnx_t *cnx, void *ptr);
void *my_realloc(picoquic_cnx_t *cnx, void *ptr, unsigned int size);

void init_memory_management(picoquic_cnx_t *cnx);
void init_memory_management_p(protoop_plugin_t *p);

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifdef DEBUG_MEMORY_PRINTF

#define DBG_MEMORY_PRINTF_FILENAME_MAX 24
#define DBG_MEMORY_PRINTF(fmt, ...)                                                                 \
    debug_printf("%s:%u [%s]: " fmt "\n",                                                    \
        __FILE__ + MAX(DBG_MEMORY_PRINTF_FILENAME_MAX, sizeof(__FILE__)) - DBG_MEMORY_PRINTF_FILENAME_MAX, \
        __LINE__, __FUNCTION__, __VA_ARGS__)

#else

#define DBG_MEMORY_PRINTF(fmt, ...)

#endif // #ifdef DEBUG_PLUGIN_PRINTF

#endif // MEMORY_H