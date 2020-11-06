#ifndef QTUM_QT_STAKEPAGE_H
#define QTUM_QT_STAKEPAGE_H

#include <wallet/wallet.h>
#include <amount.h>
#include <QWidget>
#include <memory>

class ClientModel;
class TransactionFilterProxy;
class PlatformStyle;
class WalletModel;
class TransactionView;

namespace Ui {
    class StakePage;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Stake page widget */
class StakePage : public QWidget
{
    Q_OBJECT

public:
    explicit StakePage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~StakePage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);

public Q_SLOTS:
    void setBalance();
    void numBlocksChanged(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers);
    void updateEncryptionStatus();

Q_SIGNALS:
    void requireUnlock(bool fromMenu);


private:
    Ui::StakePage *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;
    const PlatformStyle* const platformStyle;
    TransactionView* transactionView;
    CAmount m_subsidy;
    uint64_t m_networkWeight;
    double m_expectedAnnualROI;

private Q_SLOTS:
    void updateDisplayUnit();
    void on_checkStake_clicked(bool checked);

private:
    void updateSubsidy();
    void updateNetworkWeight();
    void updateAnnualROI();
};

#endif // QTUM_QT_STAKEPAGE_H
