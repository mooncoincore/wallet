// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MOONWORDDIALOG_H
#define BITCOIN_QT_MOONWORDDIALOG_H

#include "walletmodel.h"

#include <QDialog>
#include <QMessageBox>
#include <QString>
#include <QTimer>

class ClientModel;
class OptionsModel;
class PlatformStyle;
class SendCoinsRecipient;

namespace Ui {
    class MoonWordDialog;
}

struct MoonWordFrom {
    QString address;
    CAmount amount;
    uint256 txhash;
    int out;
};

QT_BEGIN_NAMESPACE
class QUrl;
QT_END_NAMESPACE

/** Dialog for sending bitcoins */
class MoonWordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MoonWordDialog(const PlatformStyle *platformStyle, CWallet* wallet, WalletModel *model, QWidget *parent = 0);
    ~MoonWordDialog();

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

public Q_SLOTS:
    void clear();
    void reject();
    void accept();
    void updateTabsAndLabels();
    void setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance,
                    const CAmount& watchOnlyBalance, const CAmount& watchUnconfBalance, const CAmount& watchImmatureBalance);
    /* New transaction, or transaction changed status */
    void updateTransaction();

private:
    Ui::MoonWordDialog *ui;
    WalletModel *model;
    bool fNewRecipientAllowed;
    bool fFeeMinimized;
    const PlatformStyle *platformStyle;

    // Needed for core signals
    CWallet* wallet;

    // Core signal will notify us of new TX to refresh the drop down lists
    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

    // Map of moonword int to char http://mooncoin.com/moonword/moonword.html
    std::map<int, char> moonwordMap;

    // Holds all addresses used to send messages from
    std::multimap<int, MoonWordFrom> fromAddressesMap;

    // Map of coin amounts calculated from the message to send
    std::vector<CAmount> moonwords;

    // Set of outputs from the selected from address
    std::multimap<CAmount, COutPoint> fromOutputs;

    // Process WalletModel::SendCoinsReturn and generate a pair consisting
    // of a message and message flags for use in Q_EMIT message().
    // Additional parameter msgArg can be used via .arg(msgArg).
    void processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg = QString());
    void minimizeFeeSection(bool fMinimize);
    void updateFeeMinimizedLabel();

    // Populate drop down of from addresses
    void populateFromAddresses();
    void populateReceivedAddresses();

    // Moonword lookups by char or int
    const int& moonCharLookup(const char& c);
    const char& moonCharLookup(const int& i);

    // Calculate bytes and fee for labels
    void getTransactionDetails(unsigned int& nBytes, CAmount& nPayFee);

private Q_SLOTS:
    void deleteClicked();
    void on_addressBookButton_clicked();
    void on_sendButton_clicked();
    void on_pasteButton_clicked();
    void on_buttonChooseFee_clicked();
    void on_buttonMinimizeFee_clicked();
    void on_btn_generate_clicked();
    void updateDisplayUnit();
    void clipboardTransactionCount();
    void clipboardAmount();
    void clipboardFee();
    void clipboardBytes();
    void setMinimumFee();
    void updateFeeSectionControls();
    void updateMinFeeLabel();
    void updateGlobalFeeVariables();

    // Drop down of from addresses selected
    void selectFromAddress(int selection);

    // Change to the message to be sent via moonwords
    void textChanged();

Q_SIGNALS:
    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);
};

class SendMoonWordConfirmationDialog : public QMessageBox
{
    Q_OBJECT

public:
    SendMoonWordConfirmationDialog(const QString &title, const QString &text, int secDelay = 0, QWidget *parent = 0);
    int exec();

    private Q_SLOTS:
    void countDown();
    void updateYesButton();
    
private:
    QAbstractButton * yesButton;
    QTimer countDownTimer;
    int secDelay;
};

#endif // BITCOIN_QT_MOONWORDDIALOG_H
