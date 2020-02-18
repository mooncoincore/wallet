## Mooncoin Core 0.17.1.0


* Hotfix applied to enable - Whole chain ProofOfWork validation
* Hotfix applied to enable - Whole chain transactions validation
* Warpsync removed (speed increases were achieved by removing ProofOfWork checks)

* BIP heights correctly identified and updated
* pow.cpp updated and corrected from version .13.9
* DNS seeders added
* checkpoints updated
* themes now correctly display, testnet them clearly recognizable as not mainnet
* version added to main gui, and splash screens for clarity
* miner menus which cause virus scan alerts removed
* miner activation window on the mainnet in .13.9 was not calculated correctly so that was updated
* testnet and regtest networks were created
* Controlled lowering of coin emission via dynamic nSubsidy (schedule available at https://github.com/mooncoincore/rewardSchedule).


Mooncoin Core 
=====================================

https://mooncoin.eco

What is Mooncoin?
----------------

Our vision is to innovate and expand a tried and tested Blockchain, to create an ecosystem that enables social interactions and initiatives in science and education. The Moon ecosystem aims to serve as a platform for fast, secure monetary transactions, upon which innovative, game-changing initiatives can be implemented and executed.

For more information, as well as an immediately usable, binary version of
the Mooncoin Core software, see [https://github.com/mooncoincore/wallet/releases](https://github.com/mooncoincore/wallet/releases).

License
-------

Mooncoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/mooncoincore/wallet/tags) are created
regularly to indicate new official, stable release versions of Mooncoin Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

The developers can be found in the [mooncoin community](https://t.me/MoonCoinEco)
and should be contacted directly to discuss complicated or controversial changes before working
on a patch set.

Developers can be found on telegram at https://t.me/MoonCoinEco


### RPC PORT 44663 / P2P PORT 44664 mainnet
### RPC PORT 14663 / P2P PORT 14664 testnet

#### Mooncoin started as an early Dogecoin fork (0.6), was upgraded over time to the Litecoin codebase (0.8), since progressing through to a much more modern Litecoin 0.10 codebase - and finally to current platform of Litecoin 0.17.1 with segwit. Built with tomorrow in mind.
