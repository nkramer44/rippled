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

#include <pybind11/embed.h> // everything needed for embedding
namespace py = pybind11;
using namespace pybind11::literals; // to bring in the `_a` literal

namespace ripple {

NotTEC
DummyTx::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;  // LCOV_EXCL_LINE

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;
    
    py::scoped_interpreter guard{}; // start the interpreter and keep it alive

    py::print("Hello, World!"); // use the Python API
    py::object plugin = py::module_::import("plugin");
    py::print(plugin);
    py::object preflight = plugin.attr("preflight");
    py::print(preflight);
    py::print(ctx);
    preflight(ctx);

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
    py::scoped_interpreter guard{}; // start the interpreter and keep it alive
    py::print("Hello, World!"); // use the Python API
    py::object plugin = py::module_::import("plugin");
    py::print(plugin);
    py::object doApply = plugin.attr("doApply");
    doApply(ctx_);

    return tesSUCCESS;
}
}