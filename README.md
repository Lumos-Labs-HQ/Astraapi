cd /mnt/e/temp_git/fastapi-rust/cpp_core
mkdir -p build
cd build
cmake ..
make -j$(nproc)
cp \_fastapi_core.so ../../fastapi/
