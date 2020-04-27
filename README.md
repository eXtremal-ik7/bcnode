###### Description

BCNode: universal blockchain constructor project (prototype)

###### Information

- Language: C++17

###### Dependencies

- Compiler with full C++17 support (gcc: 8.0 or higher; clang 7 or higher; msvc 19.14/Visual Studio 2019)
- CMake build system
- OpenSSL
- libsecp256k1
- RocksDB
- Intel® Threading Building Blocks
- mpir (https://github.com/BrianGladman/mpir)
- libp2p (https://github.com/eXtremal-ik7/libp2p) version 0.5
- config4cpp (https://github.com/eXtremal-ik7/config4cpp)
- jemalloc (on Linux) - custom memory allocator

###### Download sources

```
cd YOUR_BUILD_DIRECTORY && git clone https://github.com/eXtremal-ik7/bcnode
```

###### How to build on Ubuntu 18.04 (using gcc)

```
# Install gcc 8 and other dependencies
sudo apt-get update
sudo apt-get install g++-8 git yasm texinfo autoconf automake libtool libjemalloc-dev valgrind wget software-properties-common
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 80 --slave /usr/bin/g++ g++ /usr/bin/g++-8 --slave /usr/bin/gcov gcov /usr/bin/gcov-8

# Install latest cmake version
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | sudo apt-key add -
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
sudo apt-get install cmake

# Build
cd YOUR_BUILD_DIRECTORY/bcnode
mkdir x86_64-Linux
cd x86_64-Linux
cmake ../src
make -j8

```

###### How to build on Windows with Visual Studio

- Install latest cmake from https://cmake.org/download
- Clone git repository as described below
- Run 'x64 Native Tools Command Prompt for VS 2019' from start menu
- Run 'cmake-gui' from terminal
- Select source ("src" subdirectory in git repo) and your build directory, run 'Configure' (can take a long time!) and 'Generate'.
- Open Visual Studio solution and build all targets
- Binaries require "tbb.dll" (<build directory>/tbb_cmake_build/tbb_cmake_build_subdir_release) and "mpir.dll" (bcnode/dependencies/mpir/dll/x64/Release)

###### Launch

```
./bcnodeXXX --help
./bcnodeXXX --watchlog (for view log in terminal)
```

```
./bcterminal-XXX --help
```

- Configuration file path is ${DATA_DIRECTORY}/${NETWORK_NAME}/bcnode.conf (BTC/Linux: /home/user/.bcnodebtc/main/bcnode.conf; BTC/Windows: %userprofile%\AppData\Roaming\bcnodebtc\main)
- Template of configuration file: https://github.com/eXtremal-ik7/bcnode/blob/master/doc/bcnode.conf

###### Functionality

- Synchronization and block seeding
- HTTP API
  - http://HOST:PORT/peerInfo
  - http://HOST:PORT/blockByHeight/1000000
  - http://HOST:PORT/blockByHash/a4b2e3f5f77f28e4e2cad9205ec725ef684e311aceb384352818670d38f9fdf8
  - http://HOST:PORT/tx/d163b06b81b4252bb9b6295bff77754c33bcd65ae22fc91910ee51fca8a21f29 (requires txdb enabled)
  - http://HOST:PORT/getBalance/AcK38iDzEuYUUm79ngqtH6gvUsMfMbHHpa (requires balances db enabled)
