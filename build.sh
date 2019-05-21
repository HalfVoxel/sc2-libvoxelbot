#/bin/bash

rm -rf precompiled_lib
mkdir precompiled_lib
cd precompiled_lib
mkdir lib
cp ../build/Release/libvoxelbot.lib lib/libvoxelbot_release.lib
cp ../build/Release/libvoxelbot.lib lib/libvoxelbot_debug.lib
mkdir include
cp -r ../libvoxelbot include
find include -name "*.cpp" | xargs rm
rm -rf include/libvoxelbot/generated
cp -r ../cereal/include/cereal include

# cd build
# cmake -DCMAKE_INSTALL_PREFIX:PATH=../precompiled_lib .. && make all install
