# cmake -DIN=<binary> -DOUT=<header> -P embed.cmake
# Writes the binary as a C array, so the recorder ships inside the core's
# library and needs no separate file in the package.
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "#pragma once\n#include <cstddef>\nstatic const unsigned char kRecorderBlob[] = {${bytes}};\nstatic const size_t kRecorderBlobSize = sizeof(kRecorderBlob);\n")
