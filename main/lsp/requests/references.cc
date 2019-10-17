#include "absl/strings/match.h"
#include "core/lsp/QueryResponse.h"
#include "main/lsp/ShowOperation.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {

vector<unique_ptr<Location>> LSPLoop::getReferencesToSymbol(const LSPTypecheckerOps &ops, core::SymbolRef symbol,
                                                            vector<unique_ptr<Location>> locations) const {
    if (symbol.exists()) {
        auto run2 = queryBySymbol(ops, symbol);
        locations = extractLocations(ops.gs, run2.responses, move(locations));
    }
    return locations;
}

LSPResult LSPLoop::handleTextDocumentReferences(const LSPTypecheckerOps &ops, const MessageId &id,
                                                const ReferenceParams &params) const {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentReferences);
    ShowOperation op(output, config, "References", "Finding all references...");
    prodCategoryCounterInc("lsp.messages.processed", "textDocument.references");

    const core::GlobalState &gs = ops.gs;
    auto result = queryByLoc(ops, params.textDocument->uri, *params.position, LSPMethod::TextDocumentCompletion, false);
    if (result.error) {
        // An error happened while setting up the query.
        response->error = move(result.error);
    } else {
        // An explicit null indicates that we don't support this request (or that nothing was at the location).
        // Note: Need to correctly type variant here so it goes into right 'slot' of result variant.
        response->result = variant<JSONNullObject, vector<unique_ptr<Location>>>(JSONNullObject());
        auto &queryResponses = result.responses;
        if (!queryResponses.empty()) {
            const bool fileIsTyped =
                config.uri2FileRef(gs, params.textDocument->uri).data(gs).strictLevel >= core::StrictLevel::True;
            auto resp = move(queryResponses[0]);
            // N.B.: Ignores literals.
            // If file is untyped, only supports find reference requests from constants and class definitions.
            if (auto constResp = resp->isConstant()) {
                response->result = getReferencesToSymbol(ops, constResp->symbol);
            } else if (auto defResp = resp->isDefinition()) {
                if (fileIsTyped || defResp->symbol.data(gs)->isClassOrModule()) {
                    response->result = getReferencesToSymbol(ops, defResp->symbol);
                }
            } else if (fileIsTyped && resp->isIdent()) {
                auto identResp = resp->isIdent();
                auto loc = identResp->owner.data(gs)->loc();
                if (loc.exists()) {
                    auto run2 = ops.query(core::lsp::Query::createVarQuery(identResp->owner, identResp->variable),
                                          {loc.file()});
                    response->result = extractLocations(gs, run2.responses);
                }
            } else if (fileIsTyped && resp->isSend()) {
                auto sendResp = resp->isSend();
                auto start = sendResp->dispatchResult.get();
                vector<unique_ptr<Location>> locations;
                while (start != nullptr) {
                    if (start->main.method.exists() && !start->main.receiver->isUntyped()) {
                        locations = getReferencesToSymbol(ops, start->main.method, move(locations));
                    }
                    start = start->secondary.get();
                }
                response->result = move(locations);
            }
        }
    }
    return LSPResult::make(move(response));
}

} // namespace sorbet::realmain::lsp
