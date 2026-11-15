#include "weather.h"
#include <cstdio>
#include <cstring>

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
static int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

time_t parseIsoTime(const char* s)
{
  if (!s) return 0;
  int Y, M, D, h = 0, m = 0, sec = 0;
  int n = 0;
  if (sscanf(s, "%4d-%2d-%2d%n", &Y, &M, &D, &n) != 3) return 0;
  const char* p = s + n;
  if (*p == 'T' || *p == ' ') {
    int n2 = 0;
    if (sscanf(p + 1, "%2d:%2d%n", &h, &m, &n2) != 2) return 0;
    p += 1 + n2;
    if (*p == ':') {
      int n3 = 0;
      sscanf(p + 1, "%2d%n", &sec, &n3);
      p += 1 + n3;
    }
    if (*p == '.') {  // fractional seconds
      ++p;
      while (*p >= '0' && *p <= '9') ++p;
    }
  }
  long offset = 0;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    int sign = (*p == '-') ? -1 : 1;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) sscanf(p + 1, "%2d%2d", &oh, &om);
    offset = sign * (oh * 3600L + om * 60L);
  }
  int64_t t = daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec - offset;
  return (time_t)t;
}
