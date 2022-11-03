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
    
    // initialize use of Python
    Py_Initialize();
    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import os");

    // TODO: figure out how to replace this hardcoded path with `__FILE__`
    PyRun_SimpleString("sys.path.append(\"../src/ripple/app/tx/impl\")");

    PyRun_SimpleString("print(sys.path)");
    PyRun_SimpleString("print(os.getcwd())");

    PyObject *pName, *pModule, *pFunc, *pArgs, *pValue;

    // import the Python file
    pName = PyUnicode_DecodeFSDefault("dummy_tx");
    pModule = PyImport_Import(pName);
    Py_DECREF(pName);
    if (pModule == NULL) {
        return temUNKNOWN;
    }

    PyRun_SimpleString("print('IMPORT SUCCESSFUL')");

    // get the name of the function
    pFunc = PyObject_GetAttrString(pModule, "preflight");

    if ( !( pFunc && PyCallable_Check(pFunc) ) ) {
        if (PyErr_Occurred())
            PyErr_Print();
        PyRun_SimpleString("print('Cannot find function preflight')");
        return temUNKNOWN;
    }

    PyRun_SimpleString("print('FUNCTION RETRIEVAL SUCCESSFUL')");

    auto const account = ctx.tx[sfAccount];

    pArgs = PyTuple_New(1);

    pValue = PyUnicode_FromString(toBase58(account).c_str());
    PyTuple_SetItem(pArgs, 0, pValue);

    PyObject_CallObject(pFunc, pArgs);

    PyRun_SimpleString("print('FUNCTION CALL SUCCESSFUL')");

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