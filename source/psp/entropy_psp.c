#include <pspkernel.h>
#include <psputils.h>
#include <pspwlan.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "diagnostic.h"

static SceKernelUtilsMt19937Context entropy_context;
static int entropy_ready;

typedef struct mbedtls_ctr_drbg_context mbedtls_ctr_drbg_context;

static void entropy_init(void)
{
    unsigned char mac[6] = {0};
    uint32_t seed = (uint32_t)sceKernelGetSystemTimeWide();
    int i;
    sceWlanGetEtherAddr(mac);
    for (i = 0; i < 6; ++i) seed = (seed * 33U) ^ mac[i];
    seed ^= (uint32_t)(uintptr_t)&entropy_context;
    sceKernelUtilsMt19937Init(&entropy_context, seed);
    entropy_ready = 1;
    utubbu_log("entropy init", (int)seed);
}

int mbedtls_platform_entropy_poll(void *data, unsigned char *output,
    size_t length, size_t *output_length)
{
    size_t offset = 0;
    static int calls;
    (void)data;
    if (calls++ < 3) utubbu_log("entropy poll", (int)length);
    if (!entropy_ready) entropy_init();
    while (offset < length) {
        uint32_t value = sceKernelUtilsMt19937UInt(&entropy_context) ^
            (uint32_t)sceKernelGetSystemTimeWide();
        size_t count = length - offset;
        if (count > sizeof(value)) count = sizeof(value);
        memcpy(output + offset, &value, count);
        offset += count;
    }
    *output_length = length;
    return 0;
}

int mbedtls_hardclock_poll(void *data, unsigned char *output,
    size_t length, size_t *output_length)
{
    uint32_t value = (uint32_t)sceKernelGetSystemTimeWide();
    size_t count = length < sizeof(value) ? length : sizeof(value);
    (void)data;
    memcpy(output, &value, count);
    *output_length = count;
    return 0;
}

void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *context)
{
    (void)context;
    if (!entropy_ready) entropy_init();
    utubbu_log("drbg init psp", 0);
}

int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *context,
    int (*entropy)(void *, unsigned char *, size_t), void *entropy_data,
    const unsigned char *custom, size_t custom_length)
{
    unsigned char seed[32];
    (void)context;
    (void)custom;
    (void)custom_length;
    if (entropy) entropy(entropy_data, seed, sizeof(seed));
    utubbu_log("drbg seed psp", 0);
    return 0;
}

int mbedtls_ctr_drbg_random(void *context, unsigned char *output, size_t length)
{
    size_t output_length;
    (void)context;
    return mbedtls_platform_entropy_poll(NULL, output, length, &output_length);
}

int mbedtls_ctr_drbg_random_with_add(void *context, unsigned char *output,
    size_t length, const unsigned char *additional, size_t additional_length)
{
    (void)additional;
    (void)additional_length;
    return mbedtls_ctr_drbg_random(context, output, length);
}

void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *context)
{
    (void)context;
}
