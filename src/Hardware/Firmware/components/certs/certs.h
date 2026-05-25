/**
 * @file certs.h
 * @brief Embedded TLS root certificates.
 *
 * SETUP REQUIRED: place your broker's CA certificate at:
 *   components/certs/isrg_root_x1.pem
 *
 * The file is embedded at build time via EMBED_TXTFILES (CMakeLists.txt).
 * If your broker uses Let's Encrypt, download ISRG Root X1 from:
 *   https://letsencrypt.org/certificates/
 *
 * Use `certs_get_isrg_root_x1()` to obtain a NUL-terminated PEM string.
 */
#ifndef CERTS_H
#define CERTS_H

#ifdef __cplusplus
extern "C" {
#endif

/** @return Pointer to embedded ISRG Root X1 PEM (NUL-terminated). */
const char *certs_get_isrg_root_x1(void);

#ifdef __cplusplus
}
#endif

#endif /* CERTS_H */
