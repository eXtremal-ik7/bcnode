###### Description

BCNode: universal blockchain constructor project (prototype)

###### Information

- Language: C++17

###### Dependencies

- OpenSSL
- libp2p (https://github.com/eXtremal-ik7/libp2p) version 0.5
- mpir (https://github.com/BrianGladman/mpir)
- Intel® Threading Building Blocks
- config4cpp (https://github.com/eXtremal-ik7/config4cpp)
- CMake build system

###### Download sources

```
cd YOUR_BUILD_DIRECTORY
git clone https://github.com/eXtremal-ik7/config4cpp
git clone https://github.com/eXtremal-ik7/libp2p -b version/0.5
git clone https://github.com/BrianGladman/mpir
```

###### How to build on Linux

```
sudo apt-get install cmake libssl-dev

cd YOUR_BUILD_DIRECTORY/config4cpp
make -j5

cd YOUR_BUILD_DIRECTORY/libp2p
mkdir x86_64-Linux
cd x86_64-Linux
cmake ../src
make -j5

cd YOUR_BUILD_DIRECTORY/mpir
./configure
make -j5
sudo make install

cd YOUR_BUILD_DIRECTORY/bcnode
mkdir x86_64-Linux
cd x86_64-Linux
cmake ../src -DROOT_SOURCE_DIR=YOUR_BUILD_DIRECTORY
make -j5
```

###### How to build on Windows with Visual Studio

It's possible, detail instruction coming soon

###### Launch

```
./bcnodeXXX --help
```

- Configuration file path is ${DATA_DIRECTORY}/{$NETWORK_NAME}/bcnode.conf (example for BTC/Linux: /home/user/.bcnodebtc/main/bcnode.conf)
- Template of configuration file: doc/bcnode.conf

###### Functionality

- Synchronization and block seeding
- HTTP API
  - http://HOST:PORT/peerInfo
  - http://HOST:PORT/blockByHeight/1000000
  - http://HOST:PORT/blockByHash/a4b2e3f5f77f28e4e2cad9205ec725ef684e311aceb384352818670d38f9fdf8
