# Writes OUT with the current date and time, so every build shows when it was
# made (__DATE__/__TIME__ only change when main.cpp itself is recompiled).
string(TIMESTAMP stamp "%Y-%m-%d %H:%M:%S")
file(WRITE ${OUT} "#pragma once\n#define EDAA_BUILD_STAMP \"${stamp}\"\n")
