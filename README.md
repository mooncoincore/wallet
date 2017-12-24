## Mooncoin Core 0.13.9-segwit


* Hardfork to new codebase occurs at block 1,250,000, however Mooncoin 0.10 and 0.13 wallets will be compatible until this point.

* PoW algorithm is Scrypt, not BalloonHash. Pools and lightning network (in the future) support Scrypt, Balloon is exotic.

* Block retarget algorithm will switch to DUAL_KGW3 with Bitsend timeout; after which diff will lower dynamically if a block has not been found after 60 minutes.

* Warpsync block speedup - meaning a full sync from scratch can be achieved in 34 minutes (tested to the point of obsession on a core2quad q8200, you will sync quicker).

* Controlled lowering of coin emission via dynamic nSubsidy (schedule available at https://github.com/mooncoincore/rewardSchedule).

* Top Mooncoin addresses are unspendable and contain more than 77 billion frozen MOONcoins, which were not delivered (till March, 2017) by the Cryptsy exchange to the Florida court and were not returned to legitimate owners


## For Miner Configuration
-------------------

If you use the miner setup change the username to your wallet address.
Go to Menu Settings > Options > Miner tab
Be sure you have setup correctly the username/address with your wallet address and the Miner path is correct!


## Cross-compilation
-------------------

These steps can be performed on, for example, an Ubuntu VM. The depends system
will also work on other Linux distributions, however the commands for
installing the toolchain will be different.

Make sure you install the build requirements mentioned in
[build-unix.md](/doc/build-unix.md).
Then, install the toolchains and curl:

    sudo apt-get install g++-mingw-w64-i686 mingw-w64-i686-dev g++-mingw-w64-x86-64 mingw-w64-x86-64-dev curl

To build executables for Windows 32-bit:

    cd depends
    make HOST=i686-w64-mingw32 -j4
    cd ..
    ./configure --prefix=`pwd`/depends/i686-w64-mingw32
    make

To build executables for Windows 64-bit:

    cd depends
    make HOST=x86_64-w64-mingw32 -j4
    cd ..
    ./configure --prefix=`pwd`/depends/x86_64-w64-mingw32
    make
    
To build executables for Linux:

    ./autogen.sh
    ./configure
    if is for server environment add parameters --disable-wallet --without-gui --without-miniupnpc
    make


### Usage

To build dependencies for the current arch+OS:

    make

To build for another arch/OS:

    make HOST=host-platform-triplet

For example:

    make HOST=x86_64-w64-mingw32 -j4

A prefix will be generated that's suitable for plugging into Mooncoin's
configure. In the above example, a dir named x86_64-w64-mingw32 will be
created. To use it for Mooncoin:

    ./configure --prefix=`pwd`/depends/x86_64-w64-mingw32

Common `host-platform-triplets` for cross compilation are:

- `i686-w64-mingw32` for Win32
- `x86_64-w64-mingw32` for Win64
- `x86_64-apple-darwin11` for MacOSX
- `arm-linux-gnueabihf` for Linux ARM 32 bit
- `aarch64-linux-gnu` for Linux ARM 64 bit
_____



### RPC PORT 44663 / P2P PORT 44664

#### Mooncoin started as an early Dogecoin fork (0.6), was upgraded over time to the Litecoin codebase (0.8), since progressing through to a much more modern Litecoin 0.10 codebase - and finally to current platform of Litecoin 0.13.9 with segwit. Built with tomorrow in mind.

