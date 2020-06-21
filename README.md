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

- HTTP API (function names is case sensitive, lower case used)

```http://HOST:PORT/peerinfo```
  
  Returns information about connected peers and traffic statistics.
  
```
http://HOST:PORT/blockbyheight/1000000
http://HOST:PORT/blockbyhash/a4b2e3f5f77f28e4e2cad9205ec725ef684e311aceb384352818670d38f9fdf8
```
  
  View block structure (header and transactions), caller can use height or hash for identify blocks.
  
```http://HOST:PORT/tx/d163b06b81b4252bb9b6295bff77754c33bcd65ae22fc91910ee51fca8a21f29 (requires txdb enabled)```
  
  View transaction structure (inputs, outputs, etc), txid used as a key in database. For BTC-based clients: additional information for standard txout (P2PK, P2PKH, P2SH, ...) will be implemented in next commits.
  
```http://HOST:PORT/balance/AcK38iDzEuYUUm79ngqtH6gvUsMfMbHHpa (requires balances db enabled)```
  
  Function displays user balance and transactions count
  
```http://HOST:PORT/addrtxid/ASiq2qtKTzQ7sbERFE8Jp8HjsgVMhmk7yL?from=517740&count=5 (requires balances db enabled)```
  
  View transactions id for a specific address. All transactions belongs to address have unique numeric identifier starts from zero.

* from: first transaction id
* count: number of transactions to search for

```http://HOST:PORT/addrtx/ASiq2qtKTzQ7sbERFE8Jp8HjsgVMhmk7yL?from=517740&count=5 (requires balances db enabled)```
  
  This function likes addrtxid, but returns entire transaction data, not only ids.
