export CFLAGS="-Wno-error -Wno-implicit-function-declaration -Wno-implicit-int -Wno-deprecated-non-prototype"
export CXXFLAGS="-Wno-error -Wno-implicit-function-declaration -Wno-implicit-int -Wno-deprecated-non-prototype"
emconfigure /scratch/gdb/binutils-gdb/configure --target=arm-none-eabi  --with-static-standard-libraries --with-python=no --with-guile=no --enable-sim=no --enable-tui=no --disable-binutils --disable-gas --disable-ld --disable-gprof 
emmake make
