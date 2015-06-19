/*is_utf8 is distributed under the following terms:

Copyright (c) 2013 Palard Julien. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.*/

#include "is_utf8.h"

/*
  Check if the given unsigned char * is a valid utf-8 sequence.

  Return value :
  If the string is valid utf-8, 0 is returned.
  Else the position, starting from 1, is returned.

  Source:
   http://www.unicode.org/versions/Unicode7.0.0/UnicodeStandard-7.0.pdf
   page 124, 3.9 "Unicode Encoding Forms", "UTF-8"


  Table 3-7. Well-Formed UTF-8 Byte Sequences
  -----------------------------------------------------------------------------
  |  Code Points        | First Byte | Second Byte | Third Byte | Fourth Byte |
  |  U+0000..U+007F     |     00..7F |             |            |             |
  |  U+0080..U+07FF     |     C2..DF |      80..BF |            |             |
  |  U+0800..U+0FFF     |         E0 |      A0..BF |     80..BF |             |
  |  U+1000..U+CFFF     |     E1..EC |      80..BF |     80..BF |             |
  |  U+D000..U+D7FF     |         ED |      80..9F |     80..BF |             |
  |  U+E000..U+FFFF     |     EE..EF |      80..BF |     80..BF |             |
  |  U+10000..U+3FFFF   |         F0 |      90..BF |     80..BF |      80..BF |
  |  U+40000..U+FFFFF   |     F1..F3 |      80..BF |     80..BF |      80..BF |
  |  U+100000..U+10FFFF |         F4 |      80..8F |     80..BF |      80..BF |
  -----------------------------------------------------------------------------

*/
int is_utf8(unsigned char *str, size_t len)
{
    size_t i = 0;

    while (i < len)
    {
        if (str[i] <= 0x7F) /* 00..7F */
            i += 1;

        else if (str[i] >= 0xC2 && str[i] <= 0xDF) /* C2..DF 80..BF */
        {
            if (i + 1 < len) /* Expect a 2nd byte */
            {
                if (str[i + 1] < 0x80 || str[i + 1] > 0xBF)
                    return i + 1;
            }
            else
                return i;

            i += 2;
        }
        else if (str[i] == 0xE0) /* E0 A0..BF 80..BF */
        {
            if (i + 2 < len) /* Expect a 2nd and 3rd byte */
            {
                if (str[i + 1] < 0xA0 || str[i + 1] > 0xBF)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;
            }
            else
                return i;

            i += 3;
        }
        else if (str[i] >= 0xE1 && str[i] <= 0xEC) /* E1..EC 80..BF 80..BF */
        {
            if (i + 2 < len) /* Expect a 2nd and 3rd byte */
            {
                if (str[i + 1] < 0x80 || str[i + 1] > 0xBF)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;
            }
            else
                return i;

            i += 3;
        }
        else if (str[i] == 0xED) /* ED 80..9F 80..BF */
        {
            if (i + 2 < len) /* Expect a 2nd and 3rd byte */
            {
                if (str[i + 1] < 0x80 || str[i + 1] > 0x9F)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;
            }
            else
                return i;

            i += 3;
        }
        else if (str[i] >= 0xEE && str[i] <= 0xEF) /* EE..EF 80..BF 80..BF */
        {
            if (i + 2 < len) /* Expect a 2nd and 3rd byte */
            {
                if (str[i + 1] < 0x80 || str[i + 1] > 0xBF)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;
            }
            else
                return i;

            i += 3;
        }
        else if (str[i] == 0xF0) /* F0 90..BF 80..BF 80..BF */
        {
            if (i + 3 < len) /* Expect a 2nd, 3rd 3th byte */
            {
                if (str[i + 1] < 0x90 || str[i + 1] > 0xBF)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;

                if (str[i + 3] < 0x80 || str[i + 3] > 0xBF)
                    return i + 3;
            }
            else
                return i;

            i += 4;
        }
        else if (str[i] >= 0xF1 && str[i] <= 0xF3) /* F1..F3 80..BF 80..BF 80..BF */
        {
            if (i + 3 < len) /* Expect a 2nd, 3rd 3th byte */
            {
                if (str[i + 1] < 0x80 || str[i + 1] > 0xBF)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;

                if (str[i + 3] < 0x80 || str[i + 3] > 0xBF)
                    return i + 3;
            }
            else
                return i;

            i += 4;
        }
        else if (str[i] == 0xF4) /* F4 80..8F 80..BF 80..BF */
        {
            if (i + 3 < len) /* Expect a 2nd, 3rd 3th byte */
            {
                if (str[i + 1] < 0x80 || str[i + 1] > 0x8F)
                    return i + 1;

                if (str[i + 2] < 0x80 || str[i + 2] > 0xBF)
                    return i + 2;

                if (str[i + 3] < 0x80 || str[i + 3] > 0xBF)
                    return i + 3;
            }
            else
                return i;

            i += 4;
        }
        else
            return i;
    }
    return 0;
}
