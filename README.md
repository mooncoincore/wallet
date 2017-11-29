## Mooncoin Core 0.13.3-segwit


*The first cryptocurrency to implement the BalloonHash password hashing function*


* Hardfork to new codebase occurs at block 1,165,000, however Mooncoin 0.10 and 0.13 wallets will be compatible until this point.

* PoW algorithm will switch from Scrypt to BalloonHash (https://github.com/henrycg/balloon).

* Block retarget algorithm will switch to DUAL_KGW3 with Bitsend timeout; after which diff will lower dynamically if a block has not been found after 60 minutes.

* Warpsync block speedup - meaning a full sync from scratch can be achieved in 34 minutes (tested to the point of obsession on a core2quad q8200, you will sync quicker).

* Controlled lowering of coin emission via dynamic nSubsidy (schedule available at https://github.com/mooncoindev/moonschedule).

* Features included to prevent devaluation/movement of the currency due to stolen funds.


_____


Donate to support future Mooncoin development!
barrystyle / 2DaMooNeT5PDPxwupTFiKoS7KZsFrsWJwd

### RPC PORT 44663 / P2P PORT 44664

#### Mooncoin started as an early Dogecoin fork (0.6), was upgraded over time to the Litecoin codebase (0.8), since progressing through to a much more modern Litecoin 0.10 codebase - and finally to current platform of Litecoin 0.13.3 with segwit. Built with tomorrow in mind.

