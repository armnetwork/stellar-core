// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantManagerImpl.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "crypto/Hex.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/LedgerDelta.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "xdrpp/printer.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"

#include <memory>
#include <numeric>

namespace stellar
{

std::unique_ptr<InvariantManager>
InvariantManager::create(Application& app)
{
    return make_unique<InvariantManagerImpl>(app.getMetrics());
}

InvariantManagerImpl::InvariantManagerImpl(medida::MetricsRegistry& registry)
    : mMetricsRegistry(registry)
{
}

void
InvariantManagerImpl::checkOnLedgerClose(TxSetFramePtr const& txSet,
                                         LedgerDelta const& delta)
{
    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkOnLedgerClose(delta);
        if (result.empty())
        {
            continue;
        }

        auto transactions = TransactionSet{};
        txSet->toXDR(transactions);
        auto message =
            fmt::format(R"(invariant "{}" does not hold on ledger {}: {}{}{})",
                        invariant->getName(), delta.getHeader().ledgerSeq,
                        result, "\n", xdr::xdr_to_string(transactions));
        onInvariantFailure(invariant, message);
    }
}

void
InvariantManagerImpl::checkOnBucketApply(std::shared_ptr<Bucket const> bucket,
                                         uint32_t ledger, uint32_t level,
                                         bool isCurr)
{
    uint32_t oldestLedger = isCurr
                                ? BucketList::oldestLedgerInCurr(ledger, level)
                                : BucketList::oldestLedgerInSnap(ledger, level);
    uint32_t newestLedger = oldestLedger - 1 +
                            (isCurr ? BucketList::sizeOfCurr(ledger, level)
                                    : BucketList::sizeOfSnap(ledger, level));
    for (auto invariant : mEnabled)
    {
        auto result =
            invariant->checkOnBucketApply(bucket, oldestLedger, newestLedger);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            R"(invariant "{}" does not hold on bucket {}[{}] = {}: {})",
            invariant->getName(), isCurr ? "Curr" : "Snap", level,
            binToHex(bucket->getHash()), result);
        onInvariantFailure(invariant, message);
    }
}

void
InvariantManagerImpl::checkOnOperationApply(Operation const& operation,
                                            OperationResult const& opres,
                                            LedgerDelta const& delta)
{
    if (delta.getHeader().ledgerVersion < 8)
    {
        return;
    }

    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkOnOperationApply(operation, opres, delta);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            R"(Invariant "{}" does not hold on operation: {}{}{})",
            invariant->getName(), result, "\n", xdr::xdr_to_string(operation));
        onInvariantFailure(invariant, message);
    }
}

void
InvariantManagerImpl::registerInvariant(std::shared_ptr<Invariant> invariant)
{
    auto name = invariant->getName();
    auto iter = mInvariants.find(name);
    if (iter == mInvariants.end())
    {
        mInvariants[name] = invariant;
        mMetricsRegistry.NewCounter(
            {"invariant", "does-not-hold", invariant->getName()});
    }
    else
    {
        throw std::runtime_error{"Invariant " + invariant->getName() +
                                 " already registered"};
    }
}

void
InvariantManagerImpl::enableInvariant(std::string const& name)
{
    auto registryIter = mInvariants.find(name);
    if (registryIter == mInvariants.end())
    {
        std::string message = "Invariant " + name + " is not registered.";
        if (mInvariants.size() > 0)
        {
            using value_type = decltype(mInvariants)::value_type;
            std::string registered = std::accumulate(
                std::next(mInvariants.cbegin()), mInvariants.cend(),
                mInvariants.cbegin()->first,
                [](std::string const& lhs, value_type const& rhs) {
                    return lhs + ", " + rhs.first;
                });
            message += " Registered invariants are: " + registered;
        }
        else
        {
            message += " There are no registered invariants";
        }
        throw std::runtime_error{message};
    }

    auto iter =
        std::find(mEnabled.begin(), mEnabled.end(), registryIter->second);
    if (iter == mEnabled.end())
    {
        mEnabled.push_back(registryIter->second);
    }
    else
    {
        throw std::runtime_error{"Invariant " + name + " already enabled"};
    }
}

void
InvariantManagerImpl::onInvariantFailure(std::shared_ptr<Invariant> invariant,
                                         std::string const& message) const
{
    mMetricsRegistry
        .NewCounter({"invariant", "does-not-hold", invariant->getName()})
        .inc();
    handleInvariantFailure(invariant, message);
}

void
InvariantManagerImpl::handleInvariantFailure(
    std::shared_ptr<Invariant> invariant, std::string const& message) const
{
    if (invariant->isStrict())
    {
        CLOG(FATAL, "Invariant") << message;
        throw InvariantDoesNotHold{message};
    }
    else
    {
        CLOG(ERROR, "Invariant") << message;
    }
}
}
