@REM generate .compile_commands with msvc because I can't get clang-cl to work with clangd properly
xmake f -p windows -a x64 -m debug --toolchain=msvc
xmake project -k compile_commands --lsp=clangd ./build 
@REM build with clang-cl because I like it
xmake f -p windows -a x64 -m debug --toolchain=clang-cl
xmake 
@REM install to package
xmake install -o ./package