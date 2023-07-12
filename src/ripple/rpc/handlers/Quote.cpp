//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/app/main/Application.h>
#include <ripple/ledger/PaymentSandbox.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include "ripple/app/paths/RippleCalc.h"
#include "ripple/json/json_value.h"

namespace ripple {

// {
//   account: <ident>,
//   source_currency: <Issue>,
//   destination_amount: <Amount>,
//   ledger_index: <ledger_index>,
//   ledger_hash: <ledger>
// }

bool
issueFromJson(Issue& issue, Json::Value const& json, Json::Value& result)
{
    Currency currency;
    if (!json.isObject() || !json.isMember(jss::currency) ||
        !json[jss::currency].isString() ||
        !to_currency(currency, json[jss::currency].asString()))
    {
        RPC::inject_error(rpcSRC_CUR_MALFORMED, result);
        return false;
    }

    AccountID issuerID;
    if (json.isMember(jss::issuer) &&
        (!json[jss::issuer].isString() ||
         !to_issuer(issuerID, json[jss::issuer].asString())))
    {
        RPC::inject_error(rpcSRC_ISR_MALFORMED, result);
        return false;
    }

    if (currency.isZero())
    {
        if (issuerID.isNonZero())
        {
            RPC::inject_error(rpcSRC_CUR_MALFORMED, result);
            return false;
        }
    }

    issue = Issue(currency, issuerID);
    return true;
}

Json::Value
doQuote(RPC::JsonContext& context)
{
    auto& params = context.params;

    if (!params.isMember(jss::account))
        return RPC::missing_field_error(jss::account);
    if (!params.isMember(jss::source_currency))
        return RPC::missing_field_error(jss::source_currency);
    if (!params.isMember(jss::destination_amount))
        return RPC::missing_field_error(jss::destination_amount);

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);

    if (!ledger)
        return result;

    auto account = parseBase58<AccountID>(params[jss::account].asString());
    if (!account)
    {
        RPC::inject_error(rpcACT_MALFORMED, result);
        return result;
    }

    auto const accountID{std::move(account.value())};

    PaymentSandbox sandbox(&*ledger, tapNONE);

    path::RippleCalc::Input rcInput;
    rcInput.defaultPathsAllowed = false;

    STAmount dstAmount;
    if (!amountFromJsonNoThrow(dstAmount, params[jss::destination_amount]))
    {
        RPC::inject_error(rpcDST_AMT_MALFORMED, result);
        return result;
    }

    Json::Value const& jvSrcCurrency = params[jss::source_currency];
    if (!jvSrcCurrency.isObject())
    {
        RPC::inject_error(rpcSRC_CUR_MALFORMED, result);
        return result;
    }

    Issue srcCurrency;
    if (!issueFromJson(srcCurrency, jvSrcCurrency, result))
    {
        return result;
    }

    STPathElement pathElement;
    if (isXRP(dstAmount))
    {
        pathElement = STPathElement(std::nullopt, xrpCurrency(), std::nullopt);
    }
    else
    {
        pathElement = STPathElement(
            std::nullopt,
            dstAmount.issue().currency,
            dstAmount.issue().account);
    }
    STPathSet pathSet = STPathSet();
    pathSet.push_back(STPath({pathElement}));

    // From PathRequest l555
    auto const& sourceAccount = [&] {
        if (!isXRP(srcCurrency.account))
            return srcCurrency.account;

        if (isXRP(srcCurrency.currency))
            return xrpAccount();

        return accountID;
    }();

    // From PathRequest l565
    STAmount maxSrcAmount =
        STAmount({srcCurrency.currency, sourceAccount}, 1u, 0, true);

    auto rc = path::RippleCalc::rippleCalculate(
        sandbox,
        maxSrcAmount,  // Amount to send is unlimited to get an estimate
        dstAmount,
        accountID,
        accountID,
        pathSet,
        context.app.logs(),
        &rcInput);

    if (rc.result() == tesSUCCESS) {
        rc.actualAmountIn.setIssuer(sourceAccount);
        result[jss::source_amount] =
            rc.actualAmountIn.getJson(JsonOptions::none);
    }
    else
    {
        result[jss::source_amount] = -1;
        std::string sToken;
        std::string sHuman;

        transResultInfo(rc.result(), sToken, sHuman);
        result[jss::engine_result] = sToken;

        JLOG(context.j.debug()) << " rippleCalc returns "
                                << transHuman(rc.result());
    }
    return result;
}

}  // namespace ripple
