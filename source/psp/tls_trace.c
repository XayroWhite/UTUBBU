#include "diagnostic.h"

extern void __real_mbedtls_x509_crt_init(void *context);
extern void __real_mbedtls_x509_crl_init(void *context);
extern void __real_mbedtls_pk_init(void *context);
extern void __real_mbedtls_ssl_config_init(void *context);
extern void __real_mbedtls_ssl_init(void *context);

void __wrap_mbedtls_x509_crt_init(void *context)
{
    utubbu_log("x509 crt before", 0);
    __real_mbedtls_x509_crt_init(context);
    utubbu_log("x509 crt after", 0);
}

void __wrap_mbedtls_x509_crl_init(void *context)
{
    utubbu_log("x509 crl before", 0);
    __real_mbedtls_x509_crl_init(context);
    utubbu_log("x509 crl after", 0);
}

void __wrap_mbedtls_pk_init(void *context)
{
    utubbu_log("pk before", 0);
    __real_mbedtls_pk_init(context);
    utubbu_log("pk after", 0);
}

void __wrap_mbedtls_ssl_config_init(void *context)
{
    utubbu_log("ssl config before", 0);
    __real_mbedtls_ssl_config_init(context);
    utubbu_log("ssl config after", 0);
}

void __wrap_mbedtls_ssl_init(void *context)
{
    utubbu_log("ssl init before", 0);
    __real_mbedtls_ssl_init(context);
    utubbu_log("ssl init after", 0);
}
