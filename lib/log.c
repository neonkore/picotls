/*
 * Copyright (c) 2022 Fastly, Inc., Goro Fuji, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "picotls.h"

#if PTLS_HAVE_LOG

volatile int ptls_log_is_active;

static struct {
    int *fds;
    size_t num_fds;
    size_t num_lost;
    pthread_mutex_t mutex;
} logctx = {.mutex = PTHREAD_MUTEX_INITIALIZER};

size_t ptls_log_num_lost(void)
{
    return logctx.num_lost;
}

int ptls_log_add_fd(int fd)
{
    int ret;

    pthread_mutex_lock(&logctx.mutex);

    int *newfds;
    if ((newfds = realloc(logctx.fds, sizeof(logctx.fds[0]) * (logctx.num_fds + 1))) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }
    logctx.fds = newfds;
    logctx.fds[logctx.num_fds++] = fd;
    ptls_log_is_active = 1;

    ret = 0; /* success */

Exit:
    pthread_mutex_unlock(&logctx.mutex);
    return ret;
}

#endif

void ptlslog__do_write(const ptls_buffer_t *buf)
{
#if PTLS_HAVE_LOG
    pthread_mutex_lock(&logctx.mutex);

    for (size_t fd_index = 0; fd_index < logctx.num_fds;) {
        ssize_t ret;
        while ((ret = write(logctx.fds[fd_index], buf->base, buf->off)) == -1 && errno == EINTR)
            ;
        if (ret == buf->off) {
            /* success */
            ++fd_index;
        } else if (ret > 0 || (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            /* partial write or buffer full */
            ++logctx.num_lost;
            ++fd_index;
        } else {
            /* write error; close and remove that fd from array */
            close(logctx.fds[fd_index]);
            logctx.fds[fd_index] = logctx.fds[logctx.num_fds - 1];
            --logctx.num_fds;
            if (logctx.num_fds == 0)
                ptls_log_is_active = 0;
        }
    }

    pthread_mutex_unlock(&logctx.mutex);
#endif
}

int ptlslog__do_pushv(ptls_buffer_t *buf, const void *p, size_t l)
{
    if (ptls_buffer_reserve(buf, l) != 0)
        return 0;

    memcpy(buf->base + buf->off, p, l);
    buf->off += l;
    return 1;
}

int ptlslog__do_push_unsafestr(ptls_buffer_t *buf, const char *s, size_t l)
{
    if (ptls_buffer_reserve(buf, l * (sizeof("\\uXXXX") - 1) + 1) != 0)
        return 0;

    buf->off = (uint8_t *)ptls_jsonescape((char *)(buf->base + buf->off), s, l) - buf->base;

    return 1;
}

int ptlslog__do_push_hexdump(ptls_buffer_t *buf, const void *s, size_t l)
{
    if (ptls_buffer_reserve(buf, l * 2 + 1) != 0)
        return 0;

    ptls_hexdump((char *)(buf->base + buf->off), s, l);
    buf->off += l * strlen("ff");
    return 1;
}

int ptlslog__do_push_signed32(ptls_buffer_t *buf, int32_t v)
{
    /* TODO optimize */
    char s[sizeof("-2147483648")];
    int len = sprintf(s, "%" PRId32, v);
    return ptlslog__do_pushv(buf, s, (size_t)len);
}

int ptlslog__do_push_signed64(ptls_buffer_t *buf, int64_t v)
{
    /* TODO optimize */
    char s[sizeof("-9223372036854775808")];
    int len = sprintf(s, "%" PRId64, v);
    return ptlslog__do_pushv(buf, s, (size_t)len);
}

int ptlslog__do_push_unsigned32(ptls_buffer_t *buf, uint32_t v)
{
    /* TODO optimize */
    char s[sizeof("4294967295")];
    int len = sprintf(s, "%" PRIu32, v);
    return ptlslog__do_pushv(buf, s, (size_t)len);
}

int ptlslog__do_push_unsigned64(ptls_buffer_t *buf, uint64_t v)
{
    /* TODO optimize */
    char s[sizeof("18446744073709551615")];
    int len = sprintf(s, "%" PRIu64, v);
    return ptlslog__do_pushv(buf, s, (size_t)len);
}
