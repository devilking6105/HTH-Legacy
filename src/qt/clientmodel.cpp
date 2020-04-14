// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2017-2018 The Proton Core developers
// Copyright (c) 2018 The HTH Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientmodel.h"

#include "bantablemodel.h"
#include "guiconstants.h"
#include "peertablemodel.h"

#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "clientversion.h"
#include "net.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "util.h"

#include "darksend.h"
#include "masternodeman.h"
#include "masternode-sync.h"

#include <stdint.h>

#include <QDebug>
#include <QTimer>

class CBlockIndex;

static const int64_t nClientStartupTime = GetTime();
static int64_t nLastBlockTipUpdateNotification = 0;

struct statElement {
  uint32_t blockTime; // block time
  CAmount txInValue; // pos input value
  std::vector<std::pair<std::string, CAmount>> mnPayee; // masternode payees
};
static int blockOldest = 0;
static int blockLast = 0;
static std::vector<std::pair<int, statElement>> statSourceData;

CCriticalSection cs_stat;
map<std::string, CAmount> masternodeRewards;
CAmount;
int block24hCount;
CAmount lockedCoin;

ClientModel::ClientModel(OptionsModel *optionsModel, QObject *parent) :
    QObject(parent),
    optionsModel(optionsModel),
    peerTableModel(0),
    cachedMasternodeCountString(""),
    banTableModel(0),
    pollTimer(0)
{
    peerTableModel = new PeerTableModel(this);
    banTableModel = new BanTableModel(this);
    pollTimer = new QTimer(this);
    connect(pollTimer, SIGNAL(timeout()), this, SLOT(updateTimer()));
    pollTimer->start(MODEL_UPDATE_DELAY);

    pollMnTimer = new QTimer(this);
    connect(pollMnTimer, SIGNAL(timeout()), this, SLOT(updateMnTimer()));
    // no need to update as frequent as data for balances/txes/blocks
    pollMnTimer->start(MODEL_UPDATE_DELAY * 4);

    subscribeToCoreSignals();
}

ClientModel::~ClientModel()
{
    unsubscribeFromCoreSignals();
}


void ClientModel::update24hStatsTimer()
{
  // Get required lock upfront. This avoids the GUI from getting stuck on
  // periodical polls if the core is holding the locks for a longer time -
  // for example, during a wallet rescan.
  TRY_LOCK(cs_main, lockMain);
  if (!lockMain) return;

  TRY_LOCK(cs_stat, lockStat);
  if (!lockStat) return;

  if (masternodeSync.IsBlockchainSynced() && !IsInitialBlockDownload()) {
    qDebug() << __FUNCTION__ << ": Process stats...";
    const int64_t syncStartTime = GetTime();

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[chainActive.Tip()->GetBlockHash()];

    CTxDestination Dest;
    CBitcoinAddress Address;

    int currentBlock = pblockindex->nHeight;
    // read block from last to last scaned
    while (pblockindex->nHeight > blockLast) {
        if (ReadBlockFromDisk(block, pblockindex)) {
            if (block.IsProofOfStake()) {
                // decode transactions
                const CTransaction& tx = block.vtx[1];
                if (tx.IsCoinStake()) {
                    // decode txIn
                    CTransaction txIn;
                    uint256 hashBlock;
                    if (GetTransaction(tx.vin[0].prevout.hash, txIn, hashBlock, true)) {
                        CAmount valuePoS = txIn.vout[tx.vin[0].prevout.n].nValue; // vin Value
                        ExtractDestination(txIn.vout[tx.vin[0].prevout.n].scriptPubKey, Dest);
                        Address.Set(Dest);
                        std::string addressPoS = Address.ToString(); // vin Address

                        statElement blockStat;
                        blockStat.blockTime = block.nTime;
                        blockStat.txInValue = valuePoS;
                        blockStat.mnPayee.clear();

                        // decode txOut
                        CAmount sumPoS = 0;
                        for (unsigned int i = 0; i < tx.vout.size(); i++) {
                            CTxOut txOut = tx.vout[i];
                            ExtractDestination(txOut.scriptPubKey, Dest);
                            Address.Set(Dest);
                            std::string addressOut = Address.ToString(); // vout Address
                            if (addressPoS == addressOut && valuePoS > sumPoS) {
                                // skip pos output
                                sumPoS += txOut.nValue;
                            } else {
                                // store vout payee and value
                                blockStat.mnPayee.push_back( make_pair(addressOut, txOut.nValue) );
                                // and update node rewards
                                masternodeRewards[addressOut] += txOut.nValue;
                            }
                        } 
                        // store block stat
                        statSourceData.push_back( make_pair(pblockindex->nHeight, blockStat) );
                        // stop if blocktime over 24h past
                        if ( (block.nTime + 24*60*60) < syncStartTime ) {
                            blockOldest = pblockindex->nHeight;
                            break;
                        }
                    }
                }
            }
        }
        // select next (previous) block
        pblockindex = pblockindex->pprev;
    }

    // clear over 24h block data
    std::vector<pair<std::string, CAmount>> tMN;
    std::string tAddress;
    CAmount tValue;
    if (statSourceData.size() > 0) {
        for (auto it = statSourceData.rbegin(); it != statSourceData.rend(); ++it) {
            if ( (it->second.blockTime + 24*60*60) < syncStartTime) {
                tMN = it->second.mnPayee;
                for (auto im = tMN.begin(); im != tMN.end(); ++im) {
                    tAddress = im->first;
                    tValue = im->second;
                    masternodeRewards[tAddress] -= tValue;
                }
                // remove element
                *it = statSourceData.back();
                statSourceData.pop_back();
            }
        }
    }

    // recalc stats data if new block found
    if (currentBlock > blockLast && statSourceData.size() > 0) {
      // sorting vector and get stats values
      sort(statSourceData.begin(), statSourceData.end(), sortStat);

      if (statSourceData.size() > 100) {
        CAmount posAverage = 0;
        for (auto it = statSourceData.begin(); it != statSourceData.begin() + 100; ++it)
              posAverage += it->second.txInValue;
        posMin = posAverage / 100;
        for (auto it = statSourceData.rbegin(); it != statSourceData.rbegin() + 100; ++it)
              posAverage += it->second.txInValue;
        posMax = posAverage / 100;
      } else {
        posMin = statSourceData.front().second.txInValue;
        posMax = statSourceData.back().second.txInValue;
      }

      if (statSourceData.size() % 2) {
        posMedian = (statSourceData[int(statSourceData.size()/2)].second.txInValue + statSourceData[int(statSourceData.size()/2)-1].second.txInValue) / 2;
      } else {
        posMedian = statSourceData[int(statSourceData.size()/2)-1].second.txInValue;
      }
      block24hCount = statSourceData.size();
    }

    blockLast = currentBlock;

    if (poll24hStatsTimer->interval() < 30000)
        poll24hStatsTimer->setInterval(30000);

    qDebug() << __FUNCTION__ << ": Stats ready...";
  }

  // sending signal
  //emit stats24hUpdated();
}


int ClientModel::getNumConnections(unsigned int flags) const
{
    LOCK(cs_vNodes);
    if (flags == CONNECTIONS_ALL) // Shortcut if we want total
        return vNodes.size();

    int nNum = 0;
    BOOST_FOREACH(const CNode* pnode, vNodes)
        if (flags & (pnode->fInbound ? CONNECTIONS_IN : CONNECTIONS_OUT))
            nNum++;

    return nNum;
}

QString ClientModel::getMasternodeCountString() const
{
    // return tr("Total: %1 (PS compatible: %2 / Enabled: %3) (IPv4: %4, IPv6: %5, TOR: %6)").arg(QString::number((int)mnodeman.size()))
    return tr("Total: %1 (PS compatible: %2 / Enabled: %3)")
            .arg(QString::number((int)mnodeman.size()))
            .arg(QString::number((int)mnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION)))
            .arg(QString::number((int)mnodeman.CountEnabled()));
            // .arg(QString::number((int)mnodeman.CountByIP(NET_IPV4)))
            // .arg(QString::number((int)mnodeman.CountByIP(NET_IPV6)))
            // .arg(QString::number((int)mnodeman.CountByIP(NET_TOR)));
}

int ClientModel::getNumBlocks() const
{
    LOCK(cs_main);
    return chainActive.Height();
}

quint64 ClientModel::getTotalBytesRecv() const
{
    return CNode::GetTotalBytesRecv();
}

quint64 ClientModel::getTotalBytesSent() const
{
    return CNode::GetTotalBytesSent();
}

QDateTime ClientModel::getLastBlockDate() const
{
    LOCK(cs_main);

    if (chainActive.Tip())
        return QDateTime::fromTime_t(chainActive.Tip()->GetBlockTime());

    return QDateTime::fromTime_t(Params().GenesisBlock().GetBlockTime()); // Genesis block's time of current network
}

long ClientModel::getMempoolSize() const
{
    return mempool.size();
}

size_t ClientModel::getMempoolDynamicUsage() const
{
    return mempool.DynamicMemoryUsage();
}

double ClientModel::getVerificationProgress(const CBlockIndex *tipIn) const
{
    CBlockIndex *tip = const_cast<CBlockIndex *>(tipIn);
    if (!tip)
    {
        LOCK(cs_main);
        tip = chainActive.Tip();
    }
    return Checkpoints::GuessVerificationProgress(Params().Checkpoints(), tip);
}

void ClientModel::updateTimer()
{
    // no locking required at this point
    // the following calls will aquire the required lock
    Q_EMIT mempoolSizeChanged(getMempoolSize(), getMempoolDynamicUsage());
    Q_EMIT bytesChanged(getTotalBytesRecv(), getTotalBytesSent());
}

void ClientModel::updateMnTimer()
{
    QString newMasternodeCountString = getMasternodeCountString();

    if (cachedMasternodeCountString != newMasternodeCountString)
    {
        cachedMasternodeCountString = newMasternodeCountString;

        Q_EMIT strMasternodesChanged(cachedMasternodeCountString);
    }
}

void ClientModel::updateNumConnections(int numConnections)
{
    Q_EMIT numConnectionsChanged(numConnections);
}

void ClientModel::updateAlert(const QString &hash, int status)
{
    // Show error message notification for new alert
    if(status == CT_NEW)
    {
        uint256 hash_256;
        hash_256.SetHex(hash.toStdString());
        CAlert alert = CAlert::getAlertByHash(hash_256);
        if(!alert.IsNull())
        {
            Q_EMIT message(tr("Network Alert"), QString::fromStdString(alert.strStatusBar), CClientUIInterface::ICON_ERROR);
        }
    }

    Q_EMIT alertsChanged(getStatusBarWarnings());
}

bool ClientModel::inInitialBlockDownload() const
{
    return IsInitialBlockDownload();
}

enum BlockSource ClientModel::getBlockSource() const
{
    if (fReindex)
        return BLOCK_SOURCE_REINDEX;
    else if (fImporting)
        return BLOCK_SOURCE_DISK;
    else if (getNumConnections() > 0)
        return BLOCK_SOURCE_NETWORK;

    return BLOCK_SOURCE_NONE;
}

QString ClientModel::getStatusBarWarnings() const
{
    return QString::fromStdString(GetWarnings("gui"));
}

OptionsModel *ClientModel::getOptionsModel()
{
    return optionsModel;
}

PeerTableModel *ClientModel::getPeerTableModel()
{
    return peerTableModel;
}

BanTableModel *ClientModel::getBanTableModel()
{
    return banTableModel;
}

QString ClientModel::formatFullVersion() const
{
    return QString::fromStdString(FormatFullVersion());
}

QString ClientModel::formatSubVersion() const
{
    return QString::fromStdString(strSubVersion);
}

QString ClientModel::formatBuildDate() const
{
    return QString::fromStdString(CLIENT_DATE);
}

bool ClientModel::isReleaseVersion() const
{
    return CLIENT_VERSION_IS_RELEASE;
}

QString ClientModel::clientName() const
{
    return QString::fromStdString(CLIENT_NAME);
}

QString ClientModel::formatClientStartupTime() const
{
    return QDateTime::fromTime_t(nClientStartupTime).toString();
}

void ClientModel::updateBanlist()
{
    banTableModel->refresh();
}

// Handlers for core signals
static void ShowProgress(ClientModel *clientmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(clientmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
}

static void NotifyNumConnectionsChanged(ClientModel *clientmodel, int newNumConnections)
{
    // Too noisy: qDebug() << "NotifyNumConnectionsChanged: " + QString::number(newNumConnections);
    QMetaObject::invokeMethod(clientmodel, "updateNumConnections", Qt::QueuedConnection,
                              Q_ARG(int, newNumConnections));
}

static void NotifyAlertChanged(ClientModel *clientmodel, const uint256 &hash, ChangeType status)
{
    qDebug() << "NotifyAlertChanged: " + QString::fromStdString(hash.GetHex()) + " status=" + QString::number(status);
    QMetaObject::invokeMethod(clientmodel, "updateAlert", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(hash.GetHex())),
                              Q_ARG(int, status));
}

static void BannedListChanged(ClientModel *clientmodel)
{
    qDebug() << QString("%1: Requesting update for peer banlist").arg(__func__);
    QMetaObject::invokeMethod(clientmodel, "updateBanlist", Qt::QueuedConnection);
}

static void BlockTipChanged(ClientModel *clientmodel, bool initialSync, const CBlockIndex *pIndex)
{
    // lock free async UI updates in case we have a new block tip
    // during initial sync, only update the UI if the last update
    // was > 250ms (MODEL_UPDATE_DELAY) ago
    int64_t now = 0;
    if (initialSync)
        now = GetTimeMillis();

    // if we are in-sync, update the UI regardless of last update time
    if (!initialSync || now - nLastBlockTipUpdateNotification > MODEL_UPDATE_DELAY) {
        //pass a async signal to the UI thread
        QMetaObject::invokeMethod(clientmodel, "numBlocksChanged", Qt::QueuedConnection,
                                  Q_ARG(int, pIndex->nHeight),
                                  Q_ARG(QDateTime, QDateTime::fromTime_t(pIndex->GetBlockTime())),
                                  Q_ARG(double, clientmodel->getVerificationProgress(pIndex)));
        nLastBlockTipUpdateNotification = now;
    }
}

static void NotifyAdditionalDataSyncProgressChanged(ClientModel *clientmodel, double nSyncProgress)
{
    QMetaObject::invokeMethod(clientmodel, "additionalDataSyncProgressChanged", Qt::QueuedConnection,
                              Q_ARG(double, nSyncProgress));
}

void ClientModel::subscribeToCoreSignals()
{
    // Connect signals to client
    uiInterface.ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
    uiInterface.NotifyNumConnectionsChanged.connect(boost::bind(NotifyNumConnectionsChanged, this, _1));
    uiInterface.NotifyAlertChanged.connect(boost::bind(NotifyAlertChanged, this, _1, _2));
    uiInterface.BannedListChanged.connect(boost::bind(BannedListChanged, this));
    uiInterface.NotifyBlockTip.connect(boost::bind(BlockTipChanged, this, _1, _2));
    uiInterface.NotifyAdditionalDataSyncProgressChanged.connect(boost::bind(NotifyAdditionalDataSyncProgressChanged, this, _1));
}

void ClientModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    uiInterface.ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
    uiInterface.NotifyNumConnectionsChanged.disconnect(boost::bind(NotifyNumConnectionsChanged, this, _1));
    uiInterface.NotifyAlertChanged.disconnect(boost::bind(NotifyAlertChanged, this, _1, _2));
    uiInterface.BannedListChanged.disconnect(boost::bind(BannedListChanged, this));
    uiInterface.NotifyBlockTip.disconnect(boost::bind(BlockTipChanged, this, _1, _2));
    uiInterface.NotifyAdditionalDataSyncProgressChanged.disconnect(boost::bind(NotifyAdditionalDataSyncProgressChanged, this, _1));
}
