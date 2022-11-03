#include <ripple/app/tx/impl/DummyTx.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>

#include <Python.h>


namespace ripple {

NotTEC
DummyTx::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;  // LCOV_EXCL_LINE

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;
    
    Py_Initialize();
    PyRun_SimpleString("print('*' * 2000)");

    if (Py_FinalizeEx() < 0) {
        return temUNKNOWN;
    }

    return preflight2(ctx);
}

TER
DummyTx::preclaim(PreclaimContext const& ctx)
{
    return tesSUCCESS;
}

TER
DummyTx::doApply()
{
    return tesSUCCESS;
}
}