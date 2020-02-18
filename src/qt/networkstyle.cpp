// Copyright (c) 2014-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkstyle.h>

#include <qt/guiconstants.h>
#include <chainparams.h> //params

#include <QApplication>

static const struct {
    const char *networkId;
    const char *appName;
    const char *appIcon;
    const char *titleAddText;
    const char *splashImage;
	
} network_styles[] = {
    {"Default", QAPP_APP_NAME_DEFAULT,  ":/Default/icons/bitcoin", "", ":/Default/images/splash"},
    {"Discord", QAPP_APP_NAME_DEFAULT,  ":/Discord/icons/bitcoin", "", ":/Discord/images/splash"},
    {"Blue-Moon", QAPP_APP_NAME_DEFAULT,  ":/Blue-Moon/icons/bitcoin", "", ":/Blue-Moon/images/splash"},
    {"Original-Yellow", QAPP_APP_NAME_DEFAULT,  ":/Original-Yellow/icons/bitcoin", "", ":/Original-Yellow/images/splash"},
    {"test", QAPP_APP_NAME_TESTNET, ":/Testnet/icons/bitcoin", QT_TRANSLATE_NOOP("SplashScreen", "[testnet]"), ":/Testnet/images/splash"},
    {"regtest", QAPP_APP_NAME_TESTNET,  ":/Regtest/icons/bitcoin", "[regtest]", ":/Regtest/images/splash"}
};
static const unsigned network_styles_count = sizeof(network_styles)/sizeof(*network_styles);

// titleAddText needs to be const char* for tr()
NetworkStyle::NetworkStyle(const QString &appName,  const QString &appIcon, const char *titleAddText, const QString &splashImage):
    appName(appName),
	appIcon(appIcon),
    titleAddText(qApp->translate("SplashScreen", titleAddText)),
    splashImage(splashImage)
{
}

const NetworkStyle *NetworkStyle::instantiate(const QString &networkId)
{
	if(Params().NetworkIDString() == "test"){
		const QString mynetId = (QString::fromStdString("test"));
		for (unsigned x=0; x<network_styles_count; ++x)
		{
			if (mynetId == network_styles[x].networkId)
			{
				return new NetworkStyle(
						network_styles[x].appName,
						network_styles[x].appIcon,
						network_styles[x].titleAddText,
						network_styles[x].splashImage);
			}
		}
	}
		if(Params().NetworkIDString() == "regtest"){
		const QString mynetId = (QString::fromStdString("regtest"));
		for (unsigned x=0; x<network_styles_count; ++x)
		{
			if (mynetId == network_styles[x].networkId)
			{
				return new NetworkStyle(
						network_styles[x].appName,
						network_styles[x].appIcon,
						network_styles[x].titleAddText,
						network_styles[x].splashImage);
			}
		}
	}

		
    for (unsigned x=0; x<network_styles_count; ++x)
    {
        if (networkId == network_styles[x].networkId)
        {
            return new NetworkStyle(
                    network_styles[x].appName,
					network_styles[x].appIcon,
                    network_styles[x].titleAddText,
					network_styles[x].splashImage);
        }
    }
    return 0;
}