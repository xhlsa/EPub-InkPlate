// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once

#include <cstdint>

// Jenkins96 algorithm. See: http://burtleburtle.net/bob/hash/evahash.html
//
// Produces a stable 32-bit ID from a bare filename (no path). Must receive
// the same input bytes as BooksDirController to match NVS keys across boots.

#define JENKINS96_MIX(a, b, c)                  \
{                                                \
  a = a - b;  a = a - c;  a = a ^ (c >> 13);   \
  b = b - c;  b = b - a;  b = b ^ (a <<  8);   \
  c = c - a;  c = c - b;  c = c ^ (b >> 13);   \
  a = a - b;  a = a - c;  a = a ^ (c >> 12);   \
  b = b - c;  b = b - a;  b = b ^ (a << 16);   \
  c = c - a;  c = c - b;  c = c ^ (b >>  5);   \
  a = a - b;  a = a - c;  a = a ^ (c >>  3);   \
  b = b - c;  b = b - a;  b = b ^ (a << 10);   \
  c = c - a;  c = c - b;  c = c ^ (b >> 15);   \
}

inline uint32_t
generate_id(const uint8_t * k, uint32_t bufferLength)
{
  uint32_t a, b, c, len;

  len = bufferLength;
  a = b = 0x9e3779b9;
  c = 0;

  while (len >= 12) {
    a += *((uint32_t *) &k[ 0]);
    b += *((uint32_t *) &k[ 4]);
    c += *((uint32_t *) &k[ 8]);
    JENKINS96_MIX(a, b, c);
    k += 12; len -= 12;
  }

  c += bufferLength;
  switch (len) {
    case 11: c += ((uint32_t)k[10] << 24); [[fallthrough]];
    case 10: c += ((uint32_t)k[ 9] << 16); [[fallthrough]];
    case  9: c += ((uint32_t)k[ 8] <<  8); [[fallthrough]];
    case  8: b += ((uint32_t)k[ 7] << 24); [[fallthrough]];
    case  7: b += ((uint32_t)k[ 6] << 16); [[fallthrough]];
    case  6: b += ((uint32_t)k[ 5] <<  8); [[fallthrough]];
    case  5: b += k[4];                    [[fallthrough]];
    case  4: a += ((uint32_t)k[ 3] << 24); [[fallthrough]];
    case  3: a += ((uint32_t)k[ 2] << 16); [[fallthrough]];
    case  2: a += ((uint32_t)k[ 1] <<  8); [[fallthrough]];
    case  1: a += k[0];
  }
  JENKINS96_MIX(a, b, c);

  return c;
}
