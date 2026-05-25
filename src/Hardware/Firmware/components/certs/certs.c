/* Place isrg_root_x1.pem in this directory before building. See certs.h for details. */
#include "certs.h"

extern const char isrg_root_x1_pem_start[] asm("_binary_isrg_root_x1_pem_start");
extern const char isrg_root_x1_pem_end[]   asm("_binary_isrg_root_x1_pem_end");

const char *certs_get_isrg_root_x1(void)
{
    return isrg_root_x1_pem_start;
}
