//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/tx/impl/Change.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/Sandbox.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <string_view>

namespace ripple {

NotTEC
Change::preflight(PreflightContext const& ctx)
{
    auto const ret = preflight0(ctx);
    if (!isTesSuccess(ret))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount(sfFee);
    if (!fee.native() || fee != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: invalid fee";
        return temBAD_FEE;
    }

    if (!ctx.tx.getSigningPubKey().empty() || !ctx.tx.getSignature().empty() ||
        ctx.tx.isFieldPresent(sfSigners))
    {
        JLOG(ctx.j.warn()) << "Change: Bad signature";
        return temBAD_SIGNATURE;
    }

    if (ctx.tx.getFieldU32(sfSequence) != 0 ||
        ctx.tx.isFieldPresent(sfPreviousTxnID))
    {
        JLOG(ctx.j.warn()) << "Change: Bad sequence";
        return temBAD_SEQUENCE;
    }

    if (ctx.tx.getTxnType() == ttUNL_MODIFY &&
        !ctx.rules.enabled(featureNegativeUNL))
    {
        JLOG(ctx.j.warn()) << "Change: NegativeUNL not enabled";
        return temDISABLED;
    }

    return tesSUCCESS;
}

TER
Change::preclaim(PreclaimContext const& ctx)
{
    // If tapOPEN_LEDGER is resurrected into ApplyFlags,
    // this block can be moved to preflight.
    if (ctx.view.open())
    {
        JLOG(ctx.j.warn()) << "Change transaction against open ledger";
        return temINVALID;
    }

    switch (ctx.tx.getTxnType())
    {
        case ttFEE:
            if (ctx.view.rules().enabled(featureXRPFees))
            {
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // required.
                if (!ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFee) ||
                    ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    ctx.tx.isFieldPresent(sfReserveBase) ||
                    ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
            }
            else
            {
                // The ttFEE transaction format formerly defined these fields
                // as required. When the XRPFees feature was implemented, they
                // were changed to be optional. Until the feature has been
                // enabled, they are required.
                if (!ctx.tx.isFieldPresent(sfBaseFee) ||
                    !ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    !ctx.tx.isFieldPresent(sfReserveBase) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but without the XRPFees feature, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temDISABLED;
            }
            return tesSUCCESS;
        case ttAMENDMENT:
        case ttUNL_MODIFY:
            return tesSUCCESS;
        default:
            return temUNKNOWN;
    }
}

TER
Change::doApply(ApplyContext& ctx, XRPAmount mPriorBalance, XRPAmount mSourceBalance)
{
    switch (ctx.tx.getTxnType())
    {
        case ttAMENDMENT:
            return applyAmendment(ctx, mPriorBalance, mSourceBalance);
        case ttFEE:
            return applyFee(ctx, mPriorBalance, mSourceBalance);
        case ttUNL_MODIFY:
            return applyUNLModify(ctx, mPriorBalance, mSourceBalance);
        default:
            assert(0);
            return tefFAILURE;
    }
}

void
Change::preCompute()
{
    assert(ctx.tx.getAccountID(sfAccount) == beast::zero);
}

void
Change::activateTrustLinesToSelfFix(ApplyContext& ctx)
{
    JLOG(ctx.journal.warn()) << "fixTrustLinesToSelf amendment activation code starting";

    auto removeTrustLineToSelf = [](ApplyContext& ctx, Sandbox& sb, uint256 id) {
        auto tl = sb.peek(keylet::child(id));

        if (tl == nullptr)
        {
            JLOG(ctx.journal.warn()) << id << ": Unable to locate trustline";
            return true;
        }

        if (tl->getType() != ltRIPPLE_STATE)
        {
            JLOG(ctx.journal.warn()) << id << ": Unexpected type "
                            << static_cast<std::uint16_t>(tl->getType());
            return true;
        }

        auto const& lo = tl->getFieldAmount(sfLowLimit);
        auto const& hi = tl->getFieldAmount(sfHighLimit);

        if (lo != hi)
        {
            JLOG(ctx.journal.warn()) << id << ": Trustline doesn't meet requirements";
            return true;
        }

        if (auto const page = tl->getFieldU64(sfLowNode); !sb.dirRemove(
                keylet::ownerDir(lo.getIssuer()), page, tl->key(), false))
        {
            JLOG(ctx.journal.error()) << id << ": failed to remove low entry from "
                             << toBase58(lo.getIssuer()) << ":" << page
                             << " owner directory";
            return false;
        }

        if (auto const page = tl->getFieldU64(sfHighNode); !sb.dirRemove(
                keylet::ownerDir(hi.getIssuer()), page, tl->key(), false))
        {
            JLOG(ctx.journal.error()) << id << ": failed to remove high entry from "
                             << toBase58(hi.getIssuer()) << ":" << page
                             << " owner directory";
            return false;
        }

        if (tl->getFlags() & lsfLowReserve)
            adjustOwnerCount(
                sb, sb.peek(keylet::account(lo.getIssuer())), -1, ctx.journal);

        if (tl->getFlags() & lsfHighReserve)
            adjustOwnerCount(
                sb, sb.peek(keylet::account(hi.getIssuer())), -1, ctx.journal);

        sb.erase(tl);

        JLOG(ctx.journal.warn()) << "Successfully deleted trustline " << id;

        return true;
    };

    using namespace std::literals;

    Sandbox sb(&ctx.view());

    if (removeTrustLineToSelf(
            ctx,
            sb,
            uint256{
                "2F8F21EFCAFD7ACFB07D5BB04F0D2E18587820C7611305BB674A64EAB0FA71E1"sv}) &&
        removeTrustLineToSelf(
            ctx,
            sb,
            uint256{
                "326035D5C0560A9DA8636545DD5A1B0DFCFF63E68D491B5522B767BB00564B1A"sv}))
    {
        JLOG(ctx.journal.warn()) << "fixTrustLinesToSelf amendment activation code "
                           "executed successfully";
        sb.apply(ctx.rawView());
    }
}

TER
Change::applyAmendment(ApplyContext& ctx, XRPAmount mPriorBalance, XRPAmount mSourceBalance)
{
    uint256 amendment(ctx.tx.getFieldH256(sfAmendment));

    auto const k = keylet::amendments();

    SLE::pointer amendmentObject = ctx.view().peek(k);

    if (!amendmentObject)
    {
        amendmentObject = std::make_shared<SLE>(k);
        ctx.view().insert(amendmentObject);
    }

    STVector256 amendments = amendmentObject->getFieldV256(sfAmendments);

    if (std::find(amendments.begin(), amendments.end(), amendment) !=
        amendments.end())
        return tefALREADY;

    auto flags = ctx.tx.getFlags();

    const bool gotMajority = (flags & tfGotMajority) != 0;
    const bool lostMajority = (flags & tfLostMajority) != 0;

    if (gotMajority && lostMajority)
        return temINVALID_FLAG;

    STArray newMajorities(sfMajorities);

    bool found = false;
    if (amendmentObject->isFieldPresent(sfMajorities))
    {
        const STArray& oldMajorities =
            amendmentObject->getFieldArray(sfMajorities);
        for (auto const& majority : oldMajorities)
        {
            if (majority.getFieldH256(sfAmendment) == amendment)
            {
                if (gotMajority)
                    return tefALREADY;
                found = true;
            }
            else
            {
                // pass through
                newMajorities.push_back(majority);
            }
        }
    }

    if (!found && lostMajority)
        return tefALREADY;

    if (gotMajority)
    {
        // This amendment now has a majority
        newMajorities.push_back(STObject(sfMajority));
        auto& entry = newMajorities.back();
        entry.emplace_back(STUInt256(sfAmendment, amendment));
        entry.emplace_back(STUInt32(
            sfCloseTime, ctx.view().parentCloseTime().time_since_epoch().count()));

        if (!ctx.app.getAmendmentTable().isSupported(amendment))
        {
            JLOG(ctx.journal.warn()) << "Unsupported amendment " << amendment
                            << " received a majority.";
        }
    }
    else if (!lostMajority)
    {
        // No flags, enable amendment
        amendments.push_back(amendment);
        amendmentObject->setFieldV256(sfAmendments, amendments);

        if (amendment == fixTrustLinesToSelf)
            activateTrustLinesToSelfFix(ctx);

        ctx.app.getAmendmentTable().enable(amendment);

        if (!ctx.app.getAmendmentTable().isSupported(amendment))
        {
            JLOG(ctx.journal.error()) << "Unsupported amendment " << amendment
                             << " activated: server blocked.";
            ctx.app.getOPs().setAmendmentBlocked();
        }
    }

    if (newMajorities.empty())
        amendmentObject->makeFieldAbsent(sfMajorities);
    else
        amendmentObject->setFieldArray(sfMajorities, newMajorities);

    ctx.view().update(amendmentObject);

    return tesSUCCESS;
}

TER
Change::applyFee(ApplyContext& ctx, XRPAmount mPriorBalance, XRPAmount mSourceBalance)
{
    auto const k = keylet::fees();

    SLE::pointer feeObject = ctx.view().peek(k);

    if (!feeObject)
    {
        feeObject = std::make_shared<SLE>(k);
        ctx.view().insert(feeObject);
    }
    auto set = [](SLE::pointer& feeObject, STTx const& tx, auto const& field) {
        feeObject->at(field) = tx[field];
    };
    if (ctx.view().rules().enabled(featureXRPFees))
    {
        set(feeObject, ctx.tx, sfBaseFeeDrops);
        set(feeObject, ctx.tx, sfReserveBaseDrops);
        set(feeObject, ctx.tx, sfReserveIncrementDrops);
        // Ensure the old fields are removed
        feeObject->makeFieldAbsent(sfBaseFee);
        feeObject->makeFieldAbsent(sfReferenceFeeUnits);
        feeObject->makeFieldAbsent(sfReserveBase);
        feeObject->makeFieldAbsent(sfReserveIncrement);
    }
    else
    {
        set(feeObject, ctx.tx, sfBaseFee);
        set(feeObject, ctx.tx, sfReferenceFeeUnits);
        set(feeObject, ctx.tx, sfReserveBase);
        set(feeObject, ctx.tx, sfReserveIncrement);
    }

    ctx.view().update(feeObject);

    JLOG(ctx.journal.warn()) << "Fees have been changed";
    return tesSUCCESS;
}

TER
Change::applyUNLModify(ApplyContext& ctx, XRPAmount mPriorBalance, XRPAmount mSourceBalance)
{
    if (!isFlagLedger(ctx.view().seq()))
    {
        JLOG(ctx.journal.warn()) << "N-UNL: applyUNLModify, not a flag ledger, seq="
                        << ctx.view().seq();
        return tefFAILURE;
    }

    if (!ctx.tx.isFieldPresent(sfUNLModifyDisabling) ||
        ctx.tx.getFieldU8(sfUNLModifyDisabling) > 1 ||
        !ctx.tx.isFieldPresent(sfLedgerSequence) ||
        !ctx.tx.isFieldPresent(sfUNLModifyValidator))
    {
        JLOG(ctx.journal.warn()) << "N-UNL: applyUNLModify, wrong Tx format.";
        return tefFAILURE;
    }

    bool const disabling = ctx.tx.getFieldU8(sfUNLModifyDisabling);
    auto const seq = ctx.tx.getFieldU32(sfLedgerSequence);
    if (seq != ctx.view().seq())
    {
        JLOG(ctx.journal.warn()) << "N-UNL: applyUNLModify, wrong ledger seq=" << seq;
        return tefFAILURE;
    }

    Blob const validator = ctx.tx.getFieldVL(sfUNLModifyValidator);
    if (!publicKeyType(makeSlice(validator)))
    {
        JLOG(ctx.journal.warn()) << "N-UNL: applyUNLModify, bad validator key";
        return tefFAILURE;
    }

    JLOG(ctx.journal.info()) << "N-UNL: applyUNLModify, "
                    << (disabling ? "ToDisable" : "ToReEnable")
                    << " seq=" << seq
                    << " validator data:" << strHex(validator);

    auto const k = keylet::negativeUNL();
    SLE::pointer negUnlObject = ctx.view().peek(k);
    if (!negUnlObject)
    {
        negUnlObject = std::make_shared<SLE>(k);
        ctx.view().insert(negUnlObject);
    }

    bool const found = [&] {
        if (negUnlObject->isFieldPresent(sfDisabledValidators))
        {
            auto const& negUnl =
                negUnlObject->getFieldArray(sfDisabledValidators);
            for (auto const& v : negUnl)
            {
                if (v.isFieldPresent(sfPublicKey) &&
                    v.getFieldVL(sfPublicKey) == validator)
                    return true;
            }
        }
        return false;
    }();

    if (disabling)
    {
        // cannot have more than one toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            JLOG(ctx.journal.warn()) << "N-UNL: applyUNLModify, already has ToDisable";
            return tefFAILURE;
        }

        // cannot be the same as toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToReEnable) == validator)
            {
                JLOG(ctx.journal.warn())
                    << "N-UNL: applyUNLModify, ToDisable is same as ToReEnable";
                return tefFAILURE;
            }
        }

        // cannot be in negative UNL already
        if (found)
        {
            JLOG(ctx.journal.warn())
                << "N-UNL: applyUNLModify, ToDisable already in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToDisable, validator);
    }
    else
    {
        // cannot have more than one toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            JLOG(ctx.journal.warn()) << "N-UNL: applyUNLModify, already has ToReEnable";
            return tefFAILURE;
        }

        // cannot be the same as toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToDisable) == validator)
            {
                JLOG(ctx.journal.warn())
                    << "N-UNL: applyUNLModify, ToReEnable is same as ToDisable";
                return tefFAILURE;
            }
        }

        // must be in negative UNL
        if (!found)
        {
            JLOG(ctx.journal.warn())
                << "N-UNL: applyUNLModify, ToReEnable is not in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToReEnable, validator);
    }

    ctx.view().update(negUnlObject);
    return tesSUCCESS;
}

}  // namespace ripple
