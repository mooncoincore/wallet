// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "moonworddialog.h"
#include "ui_moonworddialog.h"

#include "addressbookpage.h"
#include "addresstablemodel.h"
#include "bitcoinunits.h"
#include "clientmodel.h"
#include "coincontroldialog.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "qt/transactionrecord.h"
#include "walletmodel.h"

#include "base58.h"
#include "coincontrol.h"
#include "main.h" // mempool and minRelayTxFee
#include "ui_interface.h"
#include "txmempool.h"
#include "wallet/wallet.h"

#include <cctype> // isspace
#include <fstream>
#include <iostream>

#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <QTimer>

#define SEND_CONFIRM_DELAY   3

namespace  {
const int defaultConfirmTarget = 25;

std::string allowedChars()
{
    return " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz=*^%<>#-=.,:!?/_@()";
}

void populateMoonwordMap(std::map<int, char>& moonwordMap)
{
    moonwordMap.emplace(0, ' ');
    moonwordMap.emplace(10, '0');
    moonwordMap.emplace(11, '1');
    moonwordMap.emplace(12, '2');
    moonwordMap.emplace(13, '3');
    moonwordMap.emplace(14, '4');
    moonwordMap.emplace(15, '5');
    moonwordMap.emplace(16, '6');
    moonwordMap.emplace(17, '7');
    moonwordMap.emplace(18, '8');
    moonwordMap.emplace(19, '9');
    moonwordMap.emplace(20, 'A');
    moonwordMap.emplace(21, 'B');
    moonwordMap.emplace(22, 'C');
    moonwordMap.emplace(23, 'D');
    moonwordMap.emplace(24, 'E');
    moonwordMap.emplace(25, 'F');
    moonwordMap.emplace(26, 'G');
    moonwordMap.emplace(27, 'H');
    moonwordMap.emplace(28, 'I');
    moonwordMap.emplace(29, 'J');
    moonwordMap.emplace(30, 'K');
    moonwordMap.emplace(31, 'L');
    moonwordMap.emplace(32, 'M');
    moonwordMap.emplace(33, 'N');
    moonwordMap.emplace(34, 'O');
    moonwordMap.emplace(35, 'P');
    moonwordMap.emplace(36, 'Q');
    moonwordMap.emplace(37, 'R');
    moonwordMap.emplace(38, 'S');
    moonwordMap.emplace(39, 'T');
    moonwordMap.emplace(40, 'U');
    moonwordMap.emplace(41, 'V');
    moonwordMap.emplace(42, 'W');
    moonwordMap.emplace(43, 'X');
    moonwordMap.emplace(44, 'Y');
    moonwordMap.emplace(45, 'Z');
    moonwordMap.emplace(46, 'a');
    moonwordMap.emplace(47, 'b');
    moonwordMap.emplace(48, 'c');
    moonwordMap.emplace(49, 'd');
    moonwordMap.emplace(50, 'e');
    moonwordMap.emplace(51, 'f');
    moonwordMap.emplace(52, 'g');
    moonwordMap.emplace(53, 'h');
    moonwordMap.emplace(54, 'i');
    moonwordMap.emplace(55, 'j');
    moonwordMap.emplace(56, 'k');
    moonwordMap.emplace(57, 'l');
    moonwordMap.emplace(58, 'm');
    moonwordMap.emplace(59, 'n');
    moonwordMap.emplace(60, 'o');
    moonwordMap.emplace(61, 'p');
    moonwordMap.emplace(62, 'q');
    moonwordMap.emplace(63, 'r');
    moonwordMap.emplace(64, 's');
    moonwordMap.emplace(65, 't');
    moonwordMap.emplace(66, 'u');
    moonwordMap.emplace(67, 'v');
    moonwordMap.emplace(68, 'w');
    moonwordMap.emplace(69, 'x');
    moonwordMap.emplace(70, 'y');
    moonwordMap.emplace(71, 'z');
    moonwordMap.emplace(72, '=');
    moonwordMap.emplace(73, '*');
    moonwordMap.emplace(74, '^');
    moonwordMap.emplace(75, '%');
    moonwordMap.emplace(76, '<');
    moonwordMap.emplace(77, '>');
    moonwordMap.emplace(78, '#');
    moonwordMap.emplace(79, '-');
    moonwordMap.emplace(80, '+');
    moonwordMap.emplace(81, '.');
    moonwordMap.emplace(82, ',');
    moonwordMap.emplace(83, ':');
    moonwordMap.emplace(84, '!');
    moonwordMap.emplace(85, '?');
    moonwordMap.emplace(86, '/');
    moonwordMap.emplace(87, '_');
    moonwordMap.emplace(88, '@');
    moonwordMap.emplace(89, '(');
    moonwordMap.emplace(90, ')');
}

} // namespace

MoonWordDialog::MoonWordDialog(const PlatformStyle *platformStyle, CWallet* wallet, WalletModel *model, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MoonWordDialog),
    wallet(wallet),
    model(model),
    fNewRecipientAllowed(true),
    fFeeMinimized(true),
    platformStyle(platformStyle)
{
    ui->setupUi(this);

    ui->addressBookButton->setIcon(platformStyle->SingleColorIcon(GUIUtil::setIcon("icons/address-book")));
    ui->pasteButton->setIcon(platformStyle->SingleColorIcon(GUIUtil::setIcon("icons/editpaste")));
    ui->deleteButton->setIcon(platformStyle->SingleColorIcon(GUIUtil::setIcon("icons/remove")));

    if (!platformStyle->getImagesOnButtons()) {
        ui->labelsuffFunds->setVisible(false);
        ui->clearButton->setIcon(QIcon());
        ui->sendButton->setIcon(QIcon());
    } else {
        ui->clearButton->setIcon(platformStyle->SingleColorIcon(GUIUtil::setIcon("icons/remove")));
        ui->sendButton->setIcon(platformStyle->SingleColorIcon(GUIUtil::setIcon("icons/send")));
        ui->labelsuffFunds->setVisible(false);
    }
    connect(ui->deleteButton, SIGNAL(clicked()), this, SLOT(deleteClicked()));
    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));

    // clipboard actions
    QAction *clipboardTransactionAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    connect(clipboardAmountAction, SIGNAL(triggered()), this, SLOT(clipboardAmount()));
    connect(clipboardFeeAction, SIGNAL(triggered()), this, SLOT(clipboardFee()));
    connect(clipboardBytesAction, SIGNAL(triggered()), this, SLOT(clipboardBytes()));
    ui->labelTransactionCount->addAction(clipboardTransactionAction);
    ui->labelAmount->addAction(clipboardAmountAction);
    ui->labelFee->addAction(clipboardFeeAction);
    ui->labelBytes->addAction(clipboardBytesAction);

    // init transaction fee section
    QSettings settings;
    if (!settings.contains("fFeeSectionMinimized"))
        settings.setValue("fFeeSectionMinimized", true);
    if (!settings.contains("nFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nFeeRadio", 1); // custom
    if (!settings.contains("nFeeRadio"))
        settings.setValue("nFeeRadio", 0); // recommended
    if (!settings.contains("nCustomFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nCustomFeeRadio", 1); // total at least
    if (!settings.contains("nCustomFeeRadio"))
        settings.setValue("nCustomFeeRadio", 0); // per kilobyte
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    if (!settings.contains("nTransactionFee"))
        settings.setValue("nTransactionFee", (qint64)DEFAULT_TRANSACTION_FEE);
    if (!settings.contains("fPayOnlyMinFee"))
        settings.setValue("fPayOnlyMinFee", false);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nFeeRadio").toInt())))->setChecked(true);
    ui->groupCustomFee->setId(ui->radioCustomPerKilobyte, 0);
    ui->groupCustomFee->setId(ui->radioCustomAtLeast, 1);
    ui->groupCustomFee->button((int)std::max(0, std::min(1, settings.value("nCustomFeeRadio").toInt())))->setChecked(true);
    ui->sliderSmartFee->setValue(settings.value("nSmartFeeSliderPosition").toInt());
    ui->customFee->setValue(settings.value("nTransactionFee").toLongLong());
    ui->checkBoxMinimumFee->setChecked(settings.value("fPayOnlyMinFee").toBool());
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    populateMoonwordMap(moonwordMap);
    connect(ui->textEdit_message, SIGNAL(textChanged()), this, SLOT(textChanged()));

    if(model && model->getOptionsModel())
    {
        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance(),
                   model->getWatchBalance(), model->getWatchUnconfirmedBalance(), model->getWatchImmatureBalance());
        connect(model, SIGNAL(balanceChanged(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)), this, SLOT(setBalance(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)));
        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateDisplayUnit();

        // fee section
        connect(ui->sliderSmartFee, SIGNAL(valueChanged(int)), this, SLOT(updateGlobalFeeVariables()));
        connect(ui->groupFee, SIGNAL(buttonClicked(int)), this, SLOT(updateFeeSectionControls()));
        connect(ui->groupFee, SIGNAL(buttonClicked(int)), this, SLOT(updateGlobalFeeVariables()));
        connect(ui->groupCustomFee, SIGNAL(buttonClicked(int)), this, SLOT(updateGlobalFeeVariables()));
        connect(ui->customFee, SIGNAL(valueChanged()), this, SLOT(updateGlobalFeeVariables()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(setMinimumFee()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(updateFeeSectionControls()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(updateGlobalFeeVariables()));
        ui->customFee->setSingleStep(CWallet::GetRequiredFee(1000));
        updateFeeSectionControls();
        updateMinFeeLabel();
        updateFeeMinimizedLabel();
        updateGlobalFeeVariables();

        // moonward dropdowns
        populateFromAddresses();
        populateReceivedAddresses();
        connect(ui->cB_from, SIGNAL(currentIndexChanged(int)), this, SLOT(selectFromAddress(int)));

        subscribeToCoreSignals();
    }
}

MoonWordDialog::~MoonWordDialog()
{
    QSettings settings;
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nCustomFeeRadio", ui->groupCustomFee->checkedId());
    settings.setValue("nSmartFeeSliderPosition", ui->sliderSmartFee->value());
    settings.setValue("nTransactionFee", (qint64)ui->customFee->value());
    settings.setValue("fPayOnlyMinFee", ui->checkBoxMinimumFee->isChecked());

    unsubscribeFromCoreSignals();

    delete ui;
}

void MoonWordDialog::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->payTo->setText(QApplication::clipboard()->text());
}

void MoonWordDialog::on_addressBookButton_clicked()
{
    if(!model)
        return;
    AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::SendingTab, this);
    dlg.setModel(model->getAddressTableModel());
    if(dlg.exec())
    {
        ui->payTo->setText(dlg.getReturnValue());
    }
}

void MoonWordDialog::on_sendButton_clicked()
{
    if(!model || !model->getOptionsModel())
        return;

    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    // Validate address in to field
    if (!model->validateAddress(ui->payTo->text()))
    {
        ui->payTo->setValid(false);
        valid = false;
    }

    // Check that there's a message to send
    if (ui->textEdit_message->document()->toPlainText().toStdString() == "" || moonwords.empty())
    {
        valid = false;
    }

    // Check that the from address is not on the first empty entry (0) or has no items (-1)
    if (ui->cB_from->currentIndex() <= 0)
    {
        valid = false;
    }

    QString address = ui->payTo->text();

    for (const auto& amount : moonwords)
    {
        SendCoinsRecipient recipient;
        recipient.address = address;
        recipient.amount = amount;
        recipients.append(recipient);
    }

    if(!valid || recipients.isEmpty())
    {
        return;
    }

    fNewRecipientAllowed = false;
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return;
    }

    // prepare transaction for getting txFee earlier
    WalletModelTransaction currentTransaction(recipients);
    WalletModel::SendCoinsReturn prepareStatus;
    prepareStatus = model->prepareTransaction(currentTransaction, CoinControlDialog::coinControl, true);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(prepareStatus,
        BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), currentTransaction.getTransactionFee()));

    if(prepareStatus.status != WalletModel::OK) {
        fNewRecipientAllowed = true;
        return;
    }

    CAmount txFee = currentTransaction.getTransactionFee();

    QString questionString = tr("Are you sure you want to send?");

    if(txFee > 0)
    {
        // append fee string if a fee is required
        questionString.append("<hr /><span style='color:#aa0000;'>");
        questionString.append(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee));
        questionString.append("</span> ");
        questionString.append(tr("added as transaction fee"));

        // append transaction size
        questionString.append(" (" + QString::number((double)currentTransaction.getTransactionSize() / 1000) + " kB)");
    }

    // add total amount in all subdivision units
    questionString.append("<hr />");
    CAmount totalAmount = currentTransaction.getTotalTransactionAmount() + txFee;
    QStringList alternativeUnits;
    Q_FOREACH(BitcoinUnits::Unit u, BitcoinUnits::availableUnits())
    {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
    }
    questionString.append(tr("Total Amount %1")
        .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    questionString.append(QString("<span style='font-size:10pt;font-weight:normal;'><br />(=%2)</span>")
        .arg(alternativeUnits.join(" " + tr("or") + "<br />")));

    SendMoonWordConfirmationDialog confirmationDialog(tr("Confirm send coins"),
        questionString, SEND_CONFIRM_DELAY, this);
    confirmationDialog.exec();
    QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();

    if(retval != QMessageBox::Yes)
    {
        fNewRecipientAllowed = true;
        return;
    }

    // now send the prepared transaction
    WalletModel::SendCoinsReturn sendStatus = model->sendCoins(currentTransaction);
    // process sendStatus and on error generate message shown to user
    processSendCoinsReturn(sendStatus);

    if (sendStatus.status == WalletModel::OK)
    {
        accept();
    }
    fNewRecipientAllowed = true;
}

void MoonWordDialog::deleteClicked()
{
    ui->payTo->clear();
}

void MoonWordDialog::clear()
{
    ui->textEdit_message->clear();
    deleteClicked();
    moonwords.clear();
    CoinControlDialog::coinControl->UnSelectAll();
    ui->cB_from->setCurrentIndex(0);
    fromOutputs.clear();
    ui->cB_recipient->setCurrentIndex(0);
    updateTabsAndLabels();
}

void MoonWordDialog::reject()
{
    clear();
}

void MoonWordDialog::accept()
{
    clear();
}


void MoonWordDialog::updateTabsAndLabels()
{
    setupTabChain(0);
}



QWidget *MoonWordDialog::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->cB_from);
    QWidget::setTabOrder(ui->cB_from, ui->payTo);
    QWidget::setTabOrder(ui->payTo, ui->addressBookButton);
    QWidget::setTabOrder(ui->addressBookButton, ui->pasteButton);
    QWidget::setTabOrder(ui->pasteButton, ui->deleteButton);
    QWidget::setTabOrder(ui->deleteButton, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->cB_recipient);
    QWidget::setTabOrder(ui->cB_recipient, ui->btn_generate);
    return ui->btn_generate;
}


void MoonWordDialog::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance,
                                 const CAmount& watchBalance, const CAmount& watchUnconfirmedBalance, const CAmount& watchImmatureBalance)
{
    Q_UNUSED(unconfirmedBalance);
    Q_UNUSED(immatureBalance);
    Q_UNUSED(watchBalance);
    Q_UNUSED(watchUnconfirmedBalance);
    Q_UNUSED(watchImmatureBalance);

    if(model && model->getOptionsModel())
    {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance));
    }
}

void MoonWordDialog::updateDisplayUnit()
{
    setBalance(model->getBalance(), 0, 0, 0, 0, 0);
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    updateMinFeeLabel();
    updateFeeMinimizedLabel();
}

void MoonWordDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to MoonWordDialog usage of WalletModel::SendCoinsReturn.
    // WalletModel::TransactionCommitFailed is used only in WalletModel::sendCoins()
    // all others are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("Transaction has an output of 0 coins. Remove multiple spaces and remove unsupported chars as these translate to 00 in Moonword code.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("From address balance too low to send this message.");
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        msgParams.first = tr("The total exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::TransactionCommitFailed:
        msgParams.first = tr("The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), maxTxFee));
        break;
    case WalletModel::PaymentRequestExpired:
        msgParams.first = tr("Payment request expired.");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    // included to prevent a compiler warning.
    case WalletModel::OK:
    default:
        return;
    }

    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void MoonWordDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void MoonWordDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void MoonWordDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void MoonWordDialog::setMinimumFee()
{
    ui->radioCustomPerKilobyte->setChecked(true);
    ui->customFee->setValue(CWallet::GetRequiredFee(1000));
}

void MoonWordDialog::updateFeeSectionControls()
{
    ui->sliderSmartFee          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFeeNormal     ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFeeFast       ->setEnabled(ui->radioSmartFee->isChecked());
    ui->checkBoxMinimumFee      ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelMinFeeWarning      ->setEnabled(ui->radioCustomFee->isChecked());
    ui->radioCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
    ui->radioCustomAtLeast      ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked() && CoinControlDialog::coinControl->HasSelected());
    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
}

void MoonWordDialog::updateGlobalFeeVariables()
{
    if (ui->radioSmartFee->isChecked())
    {
        nTxConfirmTarget = defaultConfirmTarget - ui->sliderSmartFee->value();
        payTxFee = CFeeRate(0);

        // set nMinimumTotalFee to 0 to not accidentally pay a custom fee
        CoinControlDialog::coinControl->nMinimumTotalFee = 0;
    }
    else
    {
        nTxConfirmTarget = defaultConfirmTarget;
        payTxFee = CFeeRate(ui->customFee->value());

        // if user has selected to set a minimum absolute fee, pass the value to coincontrol
        // set nMinimumTotalFee to 0 in case of user has selected that the fee is per KB
        CoinControlDialog::coinControl->nMinimumTotalFee = ui->radioCustomAtLeast->isChecked() ? ui->customFee->value() : 0;
    }
}

void MoonWordDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (!ui->radioSmartFee->isChecked()) {
        ui->labelFeeMinimized->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value()) +
            ((ui->radioCustomPerKilobyte->isChecked()) ? "/kB" : ""));
    }
}

void MoonWordDialog::updateMinFeeLabel()
{
    if (model && model->getOptionsModel())
        ui->checkBoxMinimumFee->setText(tr("Pay only the required fee of %1").arg(
            BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), CWallet::GetRequiredFee(1000)) + "/kB")
        );
}

// Coin Control: copy label "Quantity" to clipboard
void MoonWordDialog::clipboardTransactionCount()
{
    GUIUtil::setClipboard(ui->labelTransactionCount->text());
}

// Coin Control: copy label "Amount" to clipboard
void MoonWordDialog::clipboardAmount()
{
    GUIUtil::setClipboard(ui->labelAmount->text().left(ui->labelAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void MoonWordDialog::clipboardFee()
{
    GUIUtil::setClipboard(ui->labelFee->text().left(ui->labelFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}


// Coin Control: copy label "Bytes" to clipboard
void MoonWordDialog::clipboardBytes()
{
    GUIUtil::setClipboard(ui->labelBytes->text().replace(ASYMP_UTF8, ""));
}

void MoonWordDialog::getTransactionDetails(unsigned int& nBytes, CAmount& nPayFee)
{
    if (!model)
        return;

    // nPayAmount
    CAmount nPayAmount = 0;
    CMutableTransaction txDummy;

    CAmount nAmount = 0, nChange = 0;
    unsigned int nBytesInputs = 0, nQuantity = 0;
    double dPriority = 0, dPriorityInputs = 0;
    int nQuantityUncompressed = 0;
    bool fAllowFree = false, fWitness = false;

    CoinControlDialog::payAmounts.clear();
    for (const auto& amount : moonwords)
    {
        CoinControlDialog::payAmounts.append(amount);
    }

    std::vector<COutPoint> vCoinControl;
    std::vector<COutput>   vOutputs;
    CoinControlDialog::coinControl->ListSelected(vCoinControl);
    model->getOutputs(vCoinControl, vOutputs);

    for (const COutput& out : vOutputs) {
        // unselect already spent, very unlikely scenario, this could happen
        // when selected are spent elsewhere, like rpc or another computer
        uint256 txhash = out.tx->GetHash();
        COutPoint outpt(txhash, out.i);
        if (model->isSpent(outpt))
        {
            CoinControlDialog::coinControl->UnSelect(outpt);
            continue;
        }

        // Quantity
        nQuantity++;

        // Amount
        nAmount += out.tx->vout[out.i].nValue;

        // Priority
        dPriorityInputs += (double)out.tx->vout[out.i].nValue * (out.nDepth+1);

        // Bytes
        CTxDestination address;
        int witnessversion = 0;
        std::vector<unsigned char> witnessprogram;
        if (out.tx->vout[out.i].scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram))
        {
            nBytesInputs += (32 + 4 + 1 + (107 / WITNESS_SCALE_FACTOR) + 4);
            fWitness = true;
        }
        else if(ExtractDestination(out.tx->vout[out.i].scriptPubKey, address))
        {
            CPubKey pubkey;
            CKeyID *keyid = boost::get<CKeyID>(&address);
            if (keyid && model->getPubKey(*keyid, pubkey))
            {
                nBytesInputs += (pubkey.IsCompressed() ? 148 : 180);
                if (!pubkey.IsCompressed())
                    nQuantityUncompressed++;
            }
            else
                nBytesInputs += 148; // in all error cases, simply assume 148 here
        }
        else nBytesInputs += 148;
    }

    // Bytes
    nBytes = nBytesInputs + ((CoinControlDialog::payAmounts.size() > 0 ? CoinControlDialog::payAmounts.size() + 1 : 2) * 34) + 10; // always assume +1 output for change here
    if (fWitness)
    {
        // there is some fudging in these numbers related to the actual virtual transaction size calculation that will keep this estimate from being exact.
        // usually, the result will be an overestimate within a couple of satoshis so that the confirmation dialog ends up displaying a slightly smaller fee.
        // also, the witness stack size value value is a variable sized integer. usually, the number of stack items will be well under the single byte var int limit.
        nBytes += 2; // account for the serialized marker and flag bytes
        nBytes += nQuantity; // account for the witness byte that holds the number of stack items for each input.
    }

    // Priority
    double mempoolEstimatePriority = mempool.estimateSmartPriority(nTxConfirmTarget);
    dPriority = dPriorityInputs / (nBytes - nBytesInputs + (nQuantityUncompressed * 29)); // 29 = 180 - 151 (uncompressed public keys are over the limit. max 151 bytes of the input are ignored for priority)

    // Fee
    nPayFee = CWallet::GetMinimumFee(nBytes, nTxConfirmTarget, mempool);
    if (nPayFee > 0 && CoinControlDialog::coinControl->nMinimumTotalFee > nPayFee)
        nPayFee = CoinControlDialog::coinControl->nMinimumTotalFee;


    // Allow free? (require at least hard-coded threshold and default to that if no estimate)
    double dPriorityNeeded = std::max(mempoolEstimatePriority, AllowFreeThreshold());
    fAllowFree = (dPriority >= dPriorityNeeded);

    if (fSendFreeTransactions)
        if (fAllowFree && nBytes <= MAX_FREE_TRANSACTION_CREATE_SIZE)
            nPayFee = 0;

    if (nPayAmount > 0)
    {
        nChange = nAmount - nPayAmount;
        nChange -= nPayFee;

        // Never create dust outputs; if we would, just add the dust to the fee.
        if (nChange > 0 && nChange < MIN_CHANGE)
        {
            CTxOut txout(nChange, (CScript)std::vector<unsigned char>(24, 0));
            if (txout.IsDust(::minRelayTxFee))
            {
                nPayFee += nChange;
                nChange = 0;
            }
        }

        if (nChange == 0)
            nBytes -= 34;
    }
}

void MoonWordDialog::textChanged()
{
    std::string str = ui->textEdit_message->toPlainText().toStdString();
    std::string original_str = ui->textEdit_message->toPlainText().toStdString();

    moonwords.clear();

    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    // Get labels
    QLabel *tx_count = findChild<QLabel *>("labelTransactionCount");
    QLabel *amount = findChild<QLabel *>("labelAmount");
    QLabel *fee = findChild<QLabel *>("labelFee");
    QLabel *bytes = findChild<QLabel *>("labelBytes");

    if (str.empty())
    {
        // Reset labels
        tx_count->setText("0");
        amount->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, 0));
        fee->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, 0));
        bytes->setText("0");

        return;
    }

    std::string::const_iterator it = str.begin();

    // Number of moonwords required to send message
    std::string::size_type moonwordsRequired = str.size() / 4 + (str.size() % 4 ? 1 : 0);

    // Track total for labels
    CAmount total_amount{0};

    // Iterate for as many words as are required
    for (std::string::size_type i = 0; i < moonwordsRequired; ++i)
    {
        std::string amountStr;

        // Work through four chars a time or until the end of message
        for (int j = 0; j < 4 && it != str.end();)
        {
            // Check that char is allowed
            std::size_t found = allowedChars().find(*it);

            bool illegal_char = false;

            // Illegal char found
            if (found == std::string::npos)
            {
                illegal_char = true;
            }

            // Check for double space. Check current pos first.
            if (isspace(*it))
            {
                // Compare against previous char
                if (it != str.begin() && isspace(*(std::prev(it))))
                {
                    illegal_char = true;
                }
                // Compare against next char
                if (std::next(it) != str.end() && isspace(*(std::next(it))))
                {
                    illegal_char = true;
                }
            }

            if (illegal_char)
            {
                // Get cursor and deleted previous char
                QTextCursor cursor = ui->textEdit_message->textCursor();
                ui->textEdit_message->setTextCursor(cursor);
                cursor.deletePreviousChar();

                // Delete char from our copy and update iterator
                it = str.erase(it);
                continue;
            }

            std::string mooncharStr = std::to_string(moonCharLookup(*it));

            // Single char so prefix with '0'
            if (mooncharStr.size() == 1)
            {
                mooncharStr.insert(0, "0");
            }

            amountStr.append(mooncharStr);
            ++it;
            ++j;
        }

        // If the amount is less than 8 chars fill with 0s
        amountStr.append(8 - amountStr.size(), '0');

        try{
            CAmount amount = std::stoll(amountStr);
            total_amount += amount;

            // amount could be zero if new word is a single space, in which case skip it.
            if (amount > 0)
            {
                moonwords.push_back(std::stoll(amountStr));
            }
        }
        catch (const std::invalid_argument&) {} // Invalid arg supplied to stoll
        catch (const std::out_of_range&) {} // Range outside of long long
    }

    CoinControlDialog::coinControl->UnSelectAll();
    unsigned int tx_bytes = 0;
    CAmount selected = 0, tx_fee = 0;
    getTransactionDetails(tx_bytes, tx_fee);
    std::set<COutPoint>::size_type numSelected = 0;

    // Outside loop double checks selected against total as fee may have changed since inner loop check
    while(selected < total_amount + tx_fee && numSelected < fromOutputs.size())
    {
        for (const auto& outputs : fromOutputs)
        {
            if (selected < total_amount + tx_fee)
            {
                CoinControlDialog::coinControl->Select(outputs.second);
                selected += outputs.first;
                ++numSelected;
                getTransactionDetails(tx_bytes, tx_fee);
            }
            else
            {
                break;
            }
        }
    }

    tx_count->setText("1"); // Only ever 1 or 0 at the moment
    amount->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, total_amount));
    fee->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, tx_fee));
    bytes->setText(tx_bytes > 0 ? QString::number(tx_bytes) : "0");
}

void MoonWordDialog::on_btn_generate_clicked()
{
    int selection = ui->cB_recipient->currentIndex();

    if (selection == 0)
    {
        return;
    }

    QString filename = GUIUtil::getSaveFileName(this, tr("Generate Moonword Report"), QString(), tr("Text file (*.txt)"), nullptr);

    if (filename.isEmpty())
        return;

    const std::string& strDest = filename.toLocal8Bit().data();

    std::ofstream textFile;
    textFile.exceptions(std::ofstream::failbit);

    try
    {
        textFile.open(strDest);

        std::string addressStr = ui->cB_recipient->currentText().toStdString();
        std::map<uint256, CWalletTx> transactions = model->listMoonwordTransactions();

        for(const auto& tx_pair : transactions)
        {
            const CWalletTx &wtx = tx_pair.second;
            bool print_info = false;
            std::string message;

            for (const auto& txout : wtx.vout)
            {
                CTxDestination address;
                ExtractDestination(txout.scriptPubKey, address);
                const CAmount& value = txout.nValue;

                if (addressStr == CBitcoinAddress(address).ToString() && value < 100000000)
                {
                    if (!print_info)
                    {
                        print_info = true;
                        textFile << "Transaction hash: " << wtx.GetHash().ToString() << std::endl;
                        textFile << "Time: " << GUIUtil::dateTimeStr(wtx.GetTxTime()).toStdString() << std::endl;
                    }

                    std::string valueStr = std::to_string(value);

                    // If the amount is less than 8 chars fill with 0s
                    valueStr.insert(0, 8 - valueStr.size(), '0');

                    std::string subStr;
                    for (std::string::size_type i = 0; i < valueStr.size(); ++i)
                    {
                        subStr += valueStr[i];

                        if (i % 2)
                        {
                            textFile << moonCharLookup(std::stoi(subStr));
                            subStr = "";
                        }
                    }
                }
            }

            if (print_info)
            {
                textFile << std::endl;
                print_info = false;
            }
        }

        Q_EMIT message(tr("Report Generated Successful"), tr("The report successfully generated to %1.").arg(filename), CClientUIInterface::MSG_INFORMATION);
    }
    catch (std::ofstream::failure& e)
    {
        Q_EMIT message(tr("Report Generation Failed"), tr("There was an error trying to generate the report to %1.").arg(filename), CClientUIInterface::MSG_ERROR);
    }
}

void MoonWordDialog::selectFromAddress(int selection)
{
    fromOutputs.clear();

    if (selection > 0)
    {
        std::pair <std::multimap<int, MoonWordFrom>::iterator, std::multimap<int, MoonWordFrom>::iterator> ret;
        ret = fromAddressesMap.equal_range(selection);

        // Iterate over outputs that can be used from the from address multimap and add to output set
        for (std::multimap<int, MoonWordFrom>::iterator it = ret.first; it!=ret.second; ++it)
        {
            fromOutputs.emplace(it->second.amount, COutPoint(it->second.txhash, it->second.out));
        }
    }

    // Recalculate inputs and byte size/fee
    textChanged();
}

void MoonWordDialog::populateReceivedAddresses()
{
    // Clear drop down list
    ui->cB_recipient->clear();

    // First drop down blank, should default to this if wallet addresses update
    ui->cB_recipient->addItem("");

    std::set<std::string> addresses;
    std::map<uint256, CWalletTx> transactions = model->listMoonwordTransactions();

    for(const auto& tx_pair : transactions)
    {
        const CWalletTx &wtx = tx_pair.second;
        for (const auto& txout : wtx.vout)
        {
            CTxDestination address;

            if (txout.nValue < 100000000 && // Less than 1 coin, Moonword
                    ExtractDestination(txout.scriptPubKey, address) &&
                    model->isMine(address) && // Output belongs to our wallet
                    !model->isChange(txout)) // Output is not change
            {
                addresses.insert(CBitcoinAddress(address).ToString());
            }
        }
    }

    for (const auto& addr : addresses)
    {
        ui->cB_recipient->addItem(QString::fromStdString(addr));
    }
}

void MoonWordDialog::populateFromAddresses()
{
    // Clear drop down list
    ui->cB_from->clear();

    // First drop down blank, should default to this if wallet addresses update
    ui->cB_from->addItem("");

    if (!model || !model->getOptionsModel())
        return;

    std::map<QString, std::vector<COutput> > mapCoins;

    fromOutputs.clear();
    fromAddressesMap.clear();

    model->listCoins(mapCoins);

    // Holds whether the address is seen and what position it will have in the fromAddressMap and drop down
    std::map<QString, int> mapPosition;
    int position = 1;

    for (const std::pair<QString, std::vector<COutput>>& coins : mapCoins)
    {
        for (const COutput& out : coins.second)
        {
            uint256 txhash = out.tx->GetHash();
            if (model->isLockedCoin(txhash, out.i))
                continue;

            CTxDestination outputAddress;
            QString sAddress = "";
            if(ExtractDestination(out.tx->vout[out.i].scriptPubKey, outputAddress))
            {
                sAddress = QString::fromStdString(CBitcoinAddress(outputAddress).ToString());
            }

            // Try and add address to mapPosition to see if it is already present in fromAddressesMap
            auto result = mapPosition.emplace(sAddress, position);

            // Added new entry in mapPosition so create a new entry in fromAddressesMap using position
            if (result.second)
            {
                fromAddressesMap.insert(std::pair<int, MoonWordFrom>(position, {sAddress, out.tx->vout[out.i].nValue, txhash, out.i}));
                ++position;
            }
            else // Already present so use the position of the previous entry
            {
                fromAddressesMap.insert(std::pair<int, MoonWordFrom>(result.first->second, {sAddress, out.tx->vout[out.i].nValue, txhash, out.i}));
            }
        }
    }

    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    for (int i = 1; i <= mapPosition.size(); ++i)
    {
        QString address;
        CAmount sum = 0;
        std::pair <std::multimap<int, MoonWordFrom>::const_iterator, std::multimap<int, MoonWordFrom>::const_iterator> ret;
        ret = fromAddressesMap.equal_range(i);
        for (std::multimap<int, MoonWordFrom>::const_iterator it = ret.first; it != ret.second; ++it)
        {
            sum += it->second.amount;
        }
        ui->cB_from->addItem(ret.first->second.address + " (" + BitcoinUnits::format(nDisplayUnit, sum) + ")");
    }

    // Recalculate inputs and byte size/fee
    textChanged();
}

const int& MoonWordDialog::moonCharLookup(const char& c)
{
    for (const auto& pair : moonwordMap)
    {
        if (pair.second == c)
        {
            return pair.first;
        }
    }

    // If char is not found return the int that pairs with the space char,
    // reserved or unknown values will become spaces.
    return moonwordMap.cbegin()->first;
}

const char& MoonWordDialog::moonCharLookup(const int& i)
{
    try
    {
        return moonwordMap.at(i);
    }
    catch (const std::out_of_range&)
    {

        // If there's no value at i return the space char at 0,
        // reserved or unknown values will become spaces. Subscript
        // operator does not throw.
        return moonwordMap[0];
    }
}

void MoonWordDialog::updateTransaction()
{
    populateReceivedAddresses();
    populateFromAddresses();
}

static void NotifyTransactionChanged(MoonWordDialog *mwd, CWallet *wallet, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(wallet);
    Q_UNUSED(hash);
    Q_UNUSED(status);
    QMetaObject::invokeMethod(mwd, "updateTransaction", Qt::QueuedConnection);
}

void MoonWordDialog::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
}

void MoonWordDialog::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
}

SendMoonWordConfirmationDialog::SendMoonWordConfirmationDialog(const QString &title, const QString &text, int secDelay,
    QWidget *parent) :
    QMessageBox(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::Cancel, parent), secDelay(secDelay)
{
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    updateYesButton();
    connect(&countDownTimer, SIGNAL(timeout()), this, SLOT(countDown()));
}

int SendMoonWordConfirmationDialog::exec()
{
    updateYesButton();
    countDownTimer.start(1000);
    return QMessageBox::exec();
}

void SendMoonWordConfirmationDialog::countDown()
{
    secDelay--;
    updateYesButton();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendMoonWordConfirmationDialog::updateYesButton()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(tr("Yes") + " (" + QString::number(secDelay) + ")");
    }
    else
    {
        yesButton->setEnabled(true);
        yesButton->setText(tr("Yes"));
    }
}
