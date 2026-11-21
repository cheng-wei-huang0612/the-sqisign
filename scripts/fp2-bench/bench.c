// SPDX-License-Identifier: Apache-2.0

#define _GNU_SOURCE

#include <api.h>
#include <encoded_sizes.h>
#include <errno.h>
#include <fp2.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { message_bytes = 32, calls_per_block = 8, warmup_blocks = 1024 };

_Static_assert(sizeof(fp2_t) == 2 * FPG_N * sizeof(uint64_t), "Expected contiguous real and imaginary limbs");

static uint64_t entropy_state;

static void
fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

static uint64_t
next_word(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

ssize_t __real_getrandom(void *out, size_t size, unsigned int flags);

ssize_t
__wrap_getrandom(void *out, size_t size, unsigned int flags)
{
    // Preserve the system RNG call and replay matching synthetic inputs in both builds.
    const ssize_t got = __real_getrandom(out, size, flags);
    unsigned char *bytes = out;
    for (ssize_t offset = 0; offset < got; offset += 8) {
        const uint64_t word = next_word(&entropy_state);
        const size_t chunk = got - offset < 8 ? (size_t)(got - offset) : 8;
        memcpy(bytes + offset, &word, chunk);
    }
    return got;
}

static int
open_pmu(void)
{
    struct perf_event_attr attr = {
        .type = PERF_TYPE_HARDWARE,
        .size = sizeof(attr),
        .config = PERF_COUNT_HW_CPU_CYCLES,
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING,
        .disabled = 1,
        .pinned = 1,
        .exclude_kernel = 1,
        .exclude_hv = 1,
    };
    int fd = (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        perror("perf_event_open cycles:u");
        exit(EXIT_FAILURE);
    }
    return fd;
}

static void
start_counting(int fd)
{
    if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) != 0 || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        fail("Could not start the cycle counter");
    }
}

static uint64_t
stop_counting(int fd)
{
    uint64_t sample[3];
    if (ioctl(fd, PERF_EVENT_IOC_DISABLE, 0) != 0 ||
        read(fd, sample, sizeof(sample)) != (ssize_t)sizeof(sample) ||
        sample[0] == 0 || sample[1] != sample[2]) {
        fail("Invalid or multiplexed hardware cycle sample");
    }
    return sample[0];
}

static uint64_t
fingerprint(const uint8_t *bytes, size_t length, uint64_t h)
{
    for (size_t i = 0; i < length; ++i) {
        h ^= bytes[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void
init_field(fp2_t *a, uint64_t *state)
{
    uint8_t bytes[FP_ENCODED_BYTES];
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(next_word(state) >> 32);
    }
    fp_decode_reduce(&a->re, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(next_word(state) >> 32);
    }
    fp_decode_reduce(&a->im, bytes, sizeof(bytes));
}

#define MUL_PAIR(function, a, b) \
    function(a, a, b);            \
    function(b, b, a)

#define MUL_BLOCK(function, a, b) \
    MUL_PAIR(function, a, b);     \
    MUL_PAIR(function, a, b);     \
    MUL_PAIR(function, a, b);     \
    MUL_PAIR(function, a, b)

static __attribute__((noinline)) void
field_chain(fp2_t *a, fp2_t *b, unsigned long blocks)
{
    for (unsigned long i = 0; i < blocks; ++i) {
        MUL_BLOCK(fp2_mul, a, b);
    }
}

static void
measure_field(int fd, unsigned long calls)
{
    uint64_t state = UINT64_C(0x953d8be56aaf32cd);
    fp2_t a, b;
    uint8_t encoded[2][2 * FP_ENCODED_BYTES];
    init_field(&a, &state);
    init_field(&b, &state);
    if (calls == 0 || calls % calls_per_block != 0) {
        fail("Expected a positive multiple of eight field calls");
    }
    const unsigned long blocks = calls / calls_per_block;
    field_chain(&a, &b, warmup_blocks);
    start_counting(fd);
    field_chain(&a, &b, blocks);
    const uint64_t cycles = stop_counting(fd);
    fp2_encode(encoded[0], &a);
    fp2_encode(encoded[1], &b);
    const uint64_t hash = fingerprint((const uint8_t *)encoded, sizeof(encoded), UINT64_C(14695981039346656037));
    printf("%" PRIu64 ",%016" PRIx64 "\n", cycles, hash);
}

static void
check_vectors(void)
{
    struct guarded {
        uint64_t before;
        fp2_t value;
        uint64_t after;
    };
    const uint64_t guard = UINT64_C(0x492bd975851b6827);
    const unsigned aliases[][3] = { { 0, 1, 2 }, { 1, 1, 2 }, { 2, 1, 2 }, { 0, 1, 1 }, { 1, 1, 1 } };
    fp2_t input[2];
    size_t got;
    while ((got = fread(input, 1, sizeof(input), stdin)) != 0) {
        if (got != sizeof(input)) {
            fail("Incomplete field test vector");
        }
        for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
            struct guarded slots[3] = { { 0 } };
            slots[1].value = input[0];
            slots[2].value = input[1];
            for (unsigned j = 0; j < 3; ++j) {
                slots[j].before = slots[j].after = guard;
            }
            const unsigned dest = aliases[i][0];
            fp2_mul(&slots[dest].value, &slots[aliases[i][1]].value, &slots[aliases[i][2]].value);
            for (unsigned j = 0; j < 3; ++j) {
                if (slots[j].before != guard || slots[j].after != guard) {
                    fail("Field output touched a buffer guard");
                }
            }
            for (unsigned j = 1; j < 3; ++j) {
                if (j != dest && memcmp(&slots[j].value, &input[j - 1], sizeof(fp2_t)) != 0) {
                    fail("Field output modified a separate input");
                }
            }
            if (fwrite(&slots[dest].value, sizeof(fp2_t), 1, stdout) != 1) {
                fail("Could not emit the field test result");
            }
        }
    }
    if (ferror(stdin)) {
        fail("Could not read field test vectors");
    }
}

struct signed_message {
    unsigned char pk[CRYPTO_PUBLICKEYBYTES];
    unsigned char sm[CRYPTO_BYTES + message_bytes];
};

static void
make_message(unsigned long trial, unsigned char msg[message_bytes])
{
    for (unsigned i = 0; i < message_bytes; ++i) {
        msg[i] = (unsigned char)(trial + 17 * i);
    }
}

static uint64_t
wall_time_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        fail("Could not read the monotonic clock");
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void
measure_api(int fd, unsigned long trial, FILE *fixtures)
{
    struct signed_message signed_message;
    unsigned char sk[CRYPTO_SECRETKEYBYTES], msg[message_bytes], recovered[message_bytes];
    unsigned long long smlen = 0, recovered_len = 0;
    entropy_state = UINT64_C(0xb29f7845cd385203) ^ (trial + 1);
    make_message(trial, msg);

    // Read the clock inside the perf interval so the ioctl work stays outside wall time.
    start_counting(fd);
    uint64_t started_ns = wall_time_ns();
    int rc = crypto_sign_keypair(signed_message.pk, sk);
    const uint64_t keygen_ns = wall_time_ns() - started_ns;
    const uint64_t keygen_cycles = stop_counting(fd);
    if (rc != 0) {
        fail("Key generation failed");
    }

    start_counting(fd);
    started_ns = wall_time_ns();
    rc = crypto_sign(signed_message.sm, &smlen, msg, sizeof(msg), sk);
    const uint64_t sign_ns = wall_time_ns() - started_ns;
    const uint64_t sign_cycles = stop_counting(fd);
    if (rc != 0 || smlen != sizeof(signed_message.sm)) {
        fail("Signing failed");
    }

    start_counting(fd);
    started_ns = wall_time_ns();
    rc = crypto_sign_open(recovered, &recovered_len, signed_message.sm, smlen, signed_message.pk);
    const uint64_t verify_ns = wall_time_ns() - started_ns;
    const uint64_t verify_cycles = stop_counting(fd);
    if (rc != 0 || recovered_len != sizeof(msg) || memcmp(msg, recovered, sizeof(msg)) != 0) {
        fail("Verification failed");
    }
    if (fixtures != NULL) {
        printf("%lu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
               trial, keygen_cycles, sign_cycles, verify_cycles, keygen_ns, sign_ns, verify_ns);
        if (fwrite(&signed_message, sizeof(signed_message), 1, fixtures) != 1) {
            fail("Could not save the synthetic signed message");
        }
    }
}

static void
verify_fixtures(unsigned long first)
{
    struct signed_message signed_message;
    unsigned char expected[message_bytes], recovered[message_bytes];
    size_t got;
    unsigned long trial = first;
    while ((got = fread(&signed_message, 1, sizeof(signed_message), stdin)) != 0) {
        if (got != sizeof(signed_message)) {
            fail("Incomplete signature fixture");
        }
        make_message(trial++, expected);
        unsigned long long recovered_len = 0;
        if (crypto_sign_open(recovered, &recovered_len, signed_message.sm, sizeof(signed_message.sm),
                             signed_message.pk) != 0 ||
            recovered_len != sizeof(expected) || memcmp(expected, recovered, sizeof(expected)) != 0) {
            fail("Cross verification failed");
        }
    }
    if (ferror(stdin) || trial == first) {
        fail("Could not read signature fixtures");
    }
    printf("%lu\n", trial - first);
}

static unsigned long
parse_unsigned(const char *argument)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(argument, &end, 10);
    if (argument[0] < '0' || argument[0] > '9' || errno != 0 || *end != '\0') {
        fail("Expected an unsigned decimal integer");
    }
    return value;
}

int
main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "check") == 0) {
        check_vectors();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "cross-verify") == 0) {
        verify_fixtures(parse_unsigned(argv[2]));
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "field") == 0) {
        const unsigned long calls = parse_unsigned(argv[2]);
        const int fd = open_pmu();
        measure_field(fd, calls);
        return close(fd) != 0;
    }
    if (argc == 5 && strcmp(argv[1], "api") == 0) {
        const unsigned long first = parse_unsigned(argv[2]);
        const unsigned long count = parse_unsigned(argv[3]);
        if (count == 0 || count - 1 > ULONG_MAX - first) {
            fail("Invalid API trial range");
        }
        const int fd = open_pmu();
        FILE *fixtures = fopen(argv[4], "wb");
        if (fixtures == NULL) {
            fail("Could not open signature fixture file");
        }
        measure_api(fd, 65535, NULL);
        for (unsigned long i = 0; i < count; ++i) {
            measure_api(fd, first + i, fixtures);
        }
        if (fclose(fixtures) != 0) {
            fail("Could not close signature fixture file");
        }
        return close(fd) != 0;
    }
    fail("Invalid benchmark arguments");
    return EXIT_FAILURE;
}
