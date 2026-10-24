/*
 * printf.c - a small vsnprintf shared by the kernel and by tasks.
 * Supports %d %i %u %x %X %c %s %p %% with '-', '0', width and the
 * l / ll length modifiers. No floating point (and no hidden heap use).
 */
#include <stdarg.h>
#include <stdint.h>

struct out {
    char *buf;
    uint32_t size, len;
};

static void emit(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void emit_num(struct out *o, uint64_t v, int neg, unsigned base, int upper, int width,
                     int zero, int left)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    int total = n + neg;
    if (!left && !zero)
        for (; width > total; width--)
            emit(o, ' ');
    if (neg)
        emit(o, '-');
    if (!left && zero)
        for (; width > total; width--)
            emit(o, '0');
    while (n)
        emit(o, tmp[--n]);
    if (left)
        for (; width > total; width--)
            emit(o, ' ');
}

int k_vsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap)
{
    struct out o = {buf, size, 0};

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        int left = 0, zero = 0, width = 0, lng = 0;
        for (fmt++; *fmt == '-' || *fmt == '0'; fmt++) {
            if (*fmt == '-')
                left = 1;
            else
                zero = 1;
        }
        for (; *fmt >= '0' && *fmt <= '9'; fmt++)
            width = width * 10 + (*fmt - '0');
        for (; *fmt == 'l'; fmt++)
            lng++;

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
            uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
            emit_num(&o, mag, v < 0, 10, 0, width, zero, left);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            emit_num(&o, v, 0, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, zero, left);
            break;
        }
        case 'p':
            emit(&o, '0');
            emit(&o, 'x');
            emit_num(&o, (uintptr_t)va_arg(ap, void *), 0, 16, 0, 8, 1, 0);
            break;
        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0;
            if (!s)
                s = "(null)";
            while (s[n])
                n++;
            if (!left)
                for (; width > n; width--)
                    emit(&o, ' ');
            while (*s)
                emit(&o, *s++);
            if (left)
                for (; width > n; width--)
                    emit(&o, ' ');
            break;
        }
        case '%':
            emit(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            emit(&o, '%');
            emit(&o, *fmt);
            break;
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}
