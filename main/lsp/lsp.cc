#include "main/lsp/lsp.h"
#include "common/Timer.h"
#include "common/statsd/statsd.h"
#include "common/typecase.h"
#include "common/web_tracer_framework/tracing.h"
#include "core/errors/internal.h"
#include "core/errors/namer.h"
#include "core/errors/resolver.h"

using namespace std;

namespace sorbet::realmain::lsp {

LSPLoop::LSPLoop(std::unique_ptr<core::GlobalState> initialGS, LSPConfiguration config,
                 const shared_ptr<spd::logger> &logger, WorkerPool &workers, LSPOutput &output)
    : config(config), preprocessor(move(initialGS), config, workers, logger), typechecker(logger, workers, config),
      logger(logger), output(output), lastMetricUpdateTime(chrono::steady_clock::now()) {}

LSPQueryResult LSPLoop::queryByLoc(const LSPTypecheckerOps &ops, string_view uri, const Position &pos,
                                   const LSPMethod forMethod, bool errorIfFileIsUntyped) const {
    Timer timeit(logger, "setupLSPQueryByLoc");
    const core::GlobalState &gs = ops.gs;
    auto fref = config.uri2FileRef(gs, uri);
    if (!fref.exists()) {
        auto error = make_unique<ResponseError>(
            (int)LSPErrorCodes::InvalidParams,
            fmt::format("Did not find file at uri {} in {}", uri, convertLSPMethodToString(forMethod)));
        return LSPQueryResult{{}, move(error)};
    }

    if (errorIfFileIsUntyped && fref.data(gs).strictLevel < core::StrictLevel::True) {
        logger->info("Ignoring request on untyped file `{}`", uri);
        // Act as if the query returned no results.
        return LSPQueryResult{{}};
    }

    auto loc = config.lspPos2Loc(fref, pos, gs);
    return ops.query(core::lsp::Query::createLocQuery(loc), {fref});
}

LSPQueryResult LSPLoop::queryBySymbol(const LSPTypecheckerOps &ops, core::SymbolRef sym) const {
    Timer timeit(logger, "setupLSPQueryBySymbol");
    ENFORCE(sym.exists());
    vector<core::FileRef> frefs;
    const core::GlobalState &gs = ops.gs;
    const core::NameHash symNameHash(gs, sym.data(gs)->name.data(gs));
    // Locate files that contain the same Name as the symbol. Is an overapproximation, but a good first filter.
    int i = -1;
    for (auto &hash : ops.getFileHashes()) {
        i++;
        const auto &usedSends = hash.usages.sends;
        const auto &usedConstants = hash.usages.constants;
        auto ref = core::FileRef(i);

        const bool fileIsValid = ref.exists() && ref.data(gs).sourceType == core::File::Type::Normal;
        if (fileIsValid &&
            (std::find(usedSends.begin(), usedSends.end(), symNameHash) != usedSends.end() ||
             std::find(usedConstants.begin(), usedConstants.end(), symNameHash) != usedConstants.end())) {
            frefs.emplace_back(ref);
        }
    }

    return ops.query(core::lsp::Query::createSymbolQuery(sym), frefs);
}

bool LSPLoop::ensureInitialized(LSPMethod forMethod, const LSPMessage &msg) const {
    // Note: During initialization, the preprocessor sends ShowOperation notifications to the main thread to forward to
    // the client ("Indexing..."). So, whitelist those messages as OK to process prior to initialization.
    if (config.initialized || forMethod == LSPMethod::Initialize || forMethod == LSPMethod::Initialized ||
        forMethod == LSPMethod::Exit || forMethod == LSPMethod::Shutdown || forMethod == LSPMethod::SorbetError ||
        forMethod == LSPMethod::SorbetShowOperation) {
        return true;
    }
    return false;
}

constexpr chrono::minutes STATSD_INTERVAL = chrono::minutes(5);

bool LSPLoop::shouldSendCountersToStatsd(chrono::time_point<chrono::steady_clock> currentTime) const {
    return !config.opts.statsdHost.empty() && (currentTime - lastMetricUpdateTime) > STATSD_INTERVAL;
}

void LSPLoop::sendCountersToStatsd(chrono::time_point<chrono::steady_clock> currentTime) {
    ENFORCE(this_thread::get_id() == mainThreadId, "sendCounterToStatsd can only be called from the main LSP thread.");
    const auto &opts = config.opts;
    // Record rusage-related stats.
    StatsD::addRusageStats();
    auto counters = getAndClearThreadCounters();
    if (!opts.statsdHost.empty()) {
        lastMetricUpdateTime = currentTime;

        auto prefix = fmt::format("{}.lsp.counters", opts.statsdPrefix);
        StatsD::submitCounters(counters, opts.statsdHost, opts.statsdPort, prefix);
    }
    if (!opts.webTraceFile.empty()) {
        web_tracer_framework::Tracing::storeTraces(counters, opts.webTraceFile);
    }
}

LSPResult LSPResult::make(unique_ptr<ResponseMessage> response, bool canceled) {
    LSPResult rv{{}, canceled};
    rv.responses.push_back(make_unique<LSPMessage>(move(response)));
    return rv;
}

} // namespace sorbet::realmain::lsp
