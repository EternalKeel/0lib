#include "zmodule.h"
#include <stdio.h>
#include <string.h>
# include <stdint.h>

#include "async.h"
#include "uv-common.h"

#define ASYNC__INET_ADDRSTRLEN         16
#define ASYNC__INET6_ADDRSTRLEN        46


int inet_ntop4(const uint8_t *src, char *dst, size_t size);
int inet_ntop6(const uint8_t *src, char *dst, size_t size);
int inet_pton4(const char *src, uint8_t *dst);
int inet_pton6(const char *src, uint8_t *dst);


int async_inet_ntop(int af, const void* src, char* dst, size_t size)
{
  switch (af) {
    case AF_INET:
        return (inet_ntop4(src, dst, size));
    case AF_INET6:
        return (inet_ntop6(src, dst, size));
    default:
        return ASYNC_EAFNOSUPPORT;
  }
}

int inet_ntop4(const uint8_t *src, char *dst, size_t size)
{
    static const char fmt[] = "%u.%u.%u.%u";
    char tmp[ASYNC__INET_ADDRSTRLEN];
    int l;

    l = _snprintf(tmp, sizeof(tmp), fmt, src[0], src[1], src[2], src[3]);
    if (l <= 0 || (size_t) l >= size) {
        return ASYNC_ENOSPC;
    }
    strncpy(dst, tmp, size);
    dst[size - 1] = '\0';
    return 0;
}

int inet_ntop6(const uint8_t *src, char *dst, size_t size)
{
  /*
   * Note that int32_t and int16_t need only be "at least" large enough
   * to contain a value of the specified size.  On some systems, like
   * Crays, there is no such thing as an integer variable with 16 bits.
   * Keep this in mind if you think this function should have been coded
   * to use pointer overlays.  All the world's not a VAX.
   */
    char tmp[ASYNC__INET6_ADDRSTRLEN], *tp;
    struct { int base, len; } best, cur;
    uint32_t words[sizeof(struct in6_addr) / sizeof(uint16_t)];
    int i;

  /*
   * Preprocess:
   *  Copy the input (bytewise) array into a wordwise array.
   *  Find the longest run of 0x00's in src[] for :: shorthanding.
   */
  memset(words, '\0', sizeof words);
  for (i = 0; i < (int) sizeof(struct in6_addr); i++)
    words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
  best.base = -1;
  best.len = 0;
  cur.base = -1;
  cur.len = 0;
  for (i = 0; i < (int) ARRAY_SIZE(words); i++) {
    if (words[i] == 0) {
      if (cur.base == -1)
        cur.base = i, cur.len = 1;
      else
        cur.len++;
    } else {
      if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
          best = cur;
        cur.base = -1;
      }
    }
  }
  if (cur.base != -1) {
    if (best.base == -1 || cur.len > best.len)
      best = cur;
  }
  if (best.base != -1 && best.len < 2)
    best.base = -1;

  /*
   * Format the result.
   */
  tp = tmp;
  for (i = 0; i < (int) ARRAY_SIZE(words); i++) {
    /* Are we inside the best run of 0x00's? */
    if (best.base != -1 && i >= best.base &&
        i < (best.base + best.len)) {
      if (i == best.base)
        *tp++ = ':';
      continue;
    }
    /* Are we following an initial run of 0x00s or any real hex? */
    if (i != 0)
      *tp++ = ':';
    /* Is this address an encapsulated IPv4? */
    if (i == 6 && best.base == 0 && (best.len == 6 ||
        (best.len == 7 && words[7] != 0x0001) ||
        (best.len == 5 && words[5] == 0xffff))) {
      int err = inet_ntop4(src+12, tp, sizeof tmp - (tp - tmp));
      if (err)
        return err;
      tp += strlen(tp);
      break;
    }
    tp += sprintf(tp, "%x", words[i]);
  }
  /* Was it a trailing run of 0x00's? */
  if (best.base != -1 && (best.base + best.len) == ARRAY_SIZE(words))
    *tp++ = ':';
  *tp++ = '\0';

  /*
   * Check for overflow, copy, and we're done.
   */
  if ((size_t)(tp - tmp) > size) {
    return ASYNC_ENOSPC;
  }
  strcpy(dst, tmp);
  return 0;
}


int async_inet_pton(int af, const char* src, void* dst)
{
    if (src == NULL || dst == NULL) {
        return ASYNC_EINVAL;
    }

    switch (af) {
        case AF_INET:
            return (inet_pton4(src, dst));
        case AF_INET6: {
            int len;
            char tmp[ASYNC__INET6_ADDRSTRLEN], *s, *p;
            s = (char*) src;
            p = strchr(src, '%');
            if (p != NULL) {
                s = tmp;
                len = p - src;
                if (len > ASYNC__INET6_ADDRSTRLEN - 1) {
                    return ASYNC_EINVAL;
                }
                __movsb(s, src, len);
                s[len] = '\0';
            }
            return inet_pton6(s, dst);
        }
        default:
            return ASYNC_EAFNOSUPPORT;
    }
}


int inet_pton4(const char *src, uint8_t *dst)
{
  static const char digits[] = "0123456789";
  int saw_digit, octets, ch;
  uint8_t tmp[sizeof(struct in_addr)], *tp;

  saw_digit = 0;
  octets = 0;
  *(tp = tmp) = 0;
  while ((ch = *src++) != '\0') {
    const char *pch;

    if ((pch = strchr(digits, ch)) != NULL) {
      uint32_t nw = *tp * 10 + (pch - digits);

      if (saw_digit && *tp == 0)
        return ASYNC_EINVAL;
      if (nw > 255)
        return ASYNC_EINVAL;
      *tp = nw;
      if (!saw_digit) {
        if (++octets > 4)
          return ASYNC_EINVAL;
        saw_digit = 1;
      }
    } else if (ch == '.' && saw_digit) {
      if (octets == 4)
        return ASYNC_EINVAL;
      *++tp = 0;
      saw_digit = 0;
    } else
      return ASYNC_EINVAL;
  }
  if (octets < 4)
    return ASYNC_EINVAL;
  __movsb(dst, tmp, sizeof(struct in_addr));
  return 0;
}


int inet_pton6(const char *src, uint8_t *dst)
{
  static const char xdigits_l[] = "0123456789abcdef",
                    xdigits_u[] = "0123456789ABCDEF";
  uint8_t tmp[sizeof(struct in6_addr)], *tp, *endp, *colonp;
  const char *xdigits, *curtok;
  int ch, seen_xdigits;
  uint32_t val;

  memset((tp = tmp), '\0', sizeof tmp);
  endp = tp + sizeof tmp;
  colonp = NULL;
  /* Leading :: requires some special handling. */
  if (*src == ':')
    if (*++src != ':')
      return ASYNC_EINVAL;
  curtok = src;
  seen_xdigits = 0;
  val = 0;
  while ((ch = *src++) != '\0') {
    const char *pch;

    if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
      pch = strchr((xdigits = xdigits_u), ch);
    if (pch != NULL) {
      val <<= 4;
      val |= (pch - xdigits);
      if (++seen_xdigits > 4)
        return ASYNC_EINVAL;
      continue;
    }
    if (ch == ':') {
      curtok = src;
      if (!seen_xdigits) {
        if (colonp)
          return ASYNC_EINVAL;
        colonp = tp;
        continue;
      } else if (*src == '\0') {
        return ASYNC_EINVAL;
      }
      if (tp + sizeof(uint16_t) > endp)
        return ASYNC_EINVAL;
      *tp++ = (uint8_t) (val >> 8) & 0xff;
      *tp++ = (uint8_t) val & 0xff;
      seen_xdigits = 0;
      val = 0;
      continue;
    }
    if (ch == '.' && ((tp + sizeof(struct in_addr)) <= endp)) {
      int err = inet_pton4(curtok, tp);
      if (err == 0) {
        tp += sizeof(struct in_addr);
        seen_xdigits = 0;
        break;  /*%< '\\0' was seen by inet_pton4(). */
      }
    }
    return ASYNC_EINVAL;
  }
  if (seen_xdigits) {
    if (tp + sizeof(uint16_t) > endp)
      return ASYNC_EINVAL;
    *tp++ = (uint8_t) (val >> 8) & 0xff;
    *tp++ = (uint8_t) val & 0xff;
  }
  if (colonp != NULL) {
    /*
     * Since some memmove()'s erroneously fail to handle
     * overlapping regions, we'll do the shift by hand.
     */
    const int n = tp - colonp;
    int i;

    if (tp == endp)
      return ASYNC_EINVAL;
    for (i = 1; i <= n; i++) {
      endp[- i] = colonp[n - i];
      colonp[n - i] = 0;
    }
    tp = endp;
  }
  if (tp != endp)
    return ASYNC_EINVAL;
  __movsb(dst, tmp, sizeof tmp);
  return 0;
}
