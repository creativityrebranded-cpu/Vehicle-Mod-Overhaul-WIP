Standalone check of navmap.cpp (map + heading sampler) against a simulated world, no game needed.
Build with the VS 2022 Build Tools prompt, from plugin/tests:
  cl /EHsc /std:c++20 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /I../src navmap_test.cpp ../src/navmap.cpp shell32.lib ole32.lib /Fe:navmap_test.exe
(navmap_test.cpp defines its own logger::Write.) Expected: "0 failure(s)". Cases: empty map, wall with one open side (both mirror
images), boxed in, a learned floor, a character hit is not a wall.
