cd C:\Users\Siegfried\source\repos\Siegfried-Origin\EDAA
cmake -S . -B build-dev -A x64 -DEDAA_DEVELOPER=ON
cmake --build build-dev --config Release --clean-first
copy build-dev\Release\EDAA.addon64 "C:\Program Files (x86)\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64\" 