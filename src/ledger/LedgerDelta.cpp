// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerDelta.h"
#include "generated/Stellar-ledger.h"
#include "main/Application.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"

namespace stellar
{
LedgerDelta::LedgerDelta(LedgerDelta& outerDelta)
    : mOuterDelta(&outerDelta)
    , mHeader(&outerDelta.getHeader())
    , mCurrentHeader(outerDelta.getHeader())
    , mPreviousHeaderValue(outerDelta.getHeader())
{
}

LedgerDelta::LedgerDelta(LedgerHeader& header)
    : mOuterDelta(nullptr)
    , mHeader(&header)
    , mCurrentHeader(header)
    , mPreviousHeaderValue(header)
{
}

LedgerHeader&
LedgerDelta::getHeader()
{
    return mCurrentHeader.mHeader;
}

LedgerHeaderFrame&
LedgerDelta::getHeaderFrame()
{
    return mCurrentHeader;
}

void
LedgerDelta::checkState()
{
    if (mHeader == nullptr)
    {
        throw std::runtime_error(
            "Invalid operation: delta is already committed");
    }
}

void
LedgerDelta::addEntry(EntryFrame const& entry)
{
    addEntry(entry.copy());
}

void
LedgerDelta::deleteEntry(EntryFrame const& entry)
{
    deleteEntry(entry.copy());
}

void
LedgerDelta::modEntry(EntryFrame const& entry)
{
    modEntry(entry.copy());
}

void
LedgerDelta::addEntry(EntryFrame::pointer entry)
{
    checkState();
    auto k = entry->getKey();
    auto del_it = mDelete.find(k);
    if (del_it != mDelete.end())
    {
        // delete + new is an update
        mDelete.erase(del_it);
        mMod[k] = entry;
    }
    else
    {
        assert(mNew.find(k) == mNew.end()); // double new
        assert(mMod.find(k) == mMod.end()); // mod + new is invalid
        mNew[k] = entry;
    }
}

void
LedgerDelta::deleteEntry(EntryFrame::pointer entry)
{
    auto k = entry->getKey();
    deleteEntry(k);
}

void
LedgerDelta::deleteEntry(LedgerKey const& k)
{
    checkState();
    auto new_it = mNew.find(k);
    if (new_it != mNew.end())
    {
        // new + delete -> don't add it in the first place
        mNew.erase(new_it);
    }
    else
    {
        assert(mDelete.find(k) == mDelete.end()); // double delete is invalid
        // only keep the delete
        mMod.erase(k);
        mDelete.insert(k);
    }
}

void
LedgerDelta::modEntry(EntryFrame::pointer entry)
{
    checkState();
    auto k = entry->getKey();
    auto mod_it = mMod.find(k);
    if (mod_it != mMod.end())
    {
        // collapse mod
        mod_it->second = entry;
    }
    else
    {
        auto new_it = mNew.find(k);
        if (new_it != mNew.end())
        {
            // new + mod = new (with latest value)
            new_it->second = entry;
        }
        else
        {
            assert(mDelete.find(k) == mDelete.end()); // delete + mod is illegal
            mMod[k] = entry;
        }
    }
}

void
LedgerDelta::mergeEntries(LedgerDelta& other)
{
    checkState();
    for (auto& d : other.mDelete)
    {
        deleteEntry(d);
    }
    for (auto& n : other.mNew)
    {
        addEntry(n.second);
    }
    for (auto& m : other.mMod)
    {
        modEntry(m.second);
    }
}

void
LedgerDelta::commit()
{
    checkState();
    using xdr::operator==;
    // checks if we're not about to override changes
    // (commit a noop should never happen)
    if (!(mPreviousHeaderValue == *mHeader))
    {
        throw std::runtime_error("unexpected header state");
    }
    if (mOuterDelta)
    {
        mOuterDelta->mergeEntries(*this);
        mOuterDelta = nullptr;
    }
    *mHeader = mCurrentHeader.mHeader;
    mHeader = nullptr;
}

void
LedgerDelta::rollback()
{
    checkState();
    mHeader = nullptr;
}

xdr::opaque_vec<>
LedgerDelta::getTransactionMeta() const
{
    TransactionMeta tm;

    for (auto const& k : mNew)
    {
        tm.changes.emplace_back(LEDGER_ENTRY_CREATED);
        tm.changes.back().created() = k.second->mEntry;
    }
    for (auto const& k : mMod)
    {
        tm.changes.emplace_back(LEDGER_ENTRY_UPDATED);
        tm.changes.back().updated() = k.second->mEntry;
    }

    for (auto const& k : mDelete)
    {
        tm.changes.emplace_back(LEDGER_ENTRY_REMOVED);
        tm.changes.back().removed() = k;
    }

    return xdr::xdr_to_opaque(tm);
}

std::vector<LedgerEntry>
LedgerDelta::getLiveEntries() const
{
    std::vector<LedgerEntry> live;

    live.reserve(mNew.size() + mMod.size());

    for (auto const& k : mNew)
    {
        live.push_back(k.second->mEntry);
    }
    for (auto const& k : mMod)
    {
        live.push_back(k.second->mEntry);
    }

    return live;
}

std::vector<LedgerKey>
LedgerDelta::getDeadEntries() const
{
    std::vector<LedgerKey> dead;

    dead.reserve(mDelete.size());

    for (auto const& k : mDelete)
    {
        dead.push_back(k);
    }
    return dead;
}

void
LedgerDelta::markMeters(Application& app) const
{
    for (auto const& ke : mNew)
    {
        switch (ke.first.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "add"}, "entry")
                .Mark();
            break;
        case TRUSTLINE:
            app.getMetrics()
                .NewMeter({"ledger", "trust", "add"}, "entry")
                .Mark();
            break;
        case OFFER:
            app.getMetrics()
                .NewMeter({"ledger", "offer", "add"}, "entry")
                .Mark();
            break;
        }
    }

    for (auto const& ke : mMod)
    {
        switch (ke.first.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "modify"}, "entry")
                .Mark();
            break;
        case TRUSTLINE:
            app.getMetrics()
                .NewMeter({"ledger", "trust", "modify"}, "entry")
                .Mark();
            break;
        case OFFER:
            app.getMetrics()
                .NewMeter({"ledger", "offer", "modify"}, "entry")
                .Mark();
            break;
        }
    }

    for (auto const& ke : mDelete)
    {
        switch (ke.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "delete"}, "entry")
                .Mark();
            break;
        case TRUSTLINE:
            app.getMetrics()
                .NewMeter({"ledger", "trust", "delete"}, "entry")
                .Mark();
            break;
        case OFFER:
            app.getMetrics()
                .NewMeter({"ledger", "offer", "delete"}, "entry")
                .Mark();
            break;
        }
    }
}
}
