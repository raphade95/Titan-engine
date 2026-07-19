# Windows library

Build `TitanCore.lib` on Windows (or via the GitHub Actions windows-latest job):

```
cd titan-terrain-engine/cpp
cmake -B build
cmake --build build --config Release
copy build\Release\TitanCore.lib ..\unreal\TitanBridge\ThirdParty\TitanCore\lib\Win64\
```

The Fab package must include both the Mac and Win64 libraries.
